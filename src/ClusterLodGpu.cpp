#include "ClusterLodGpu.h"

#include "ClusterLod.h"
#include "meshoptimizer.h"
#include "shaders/srrhi/cpp/Cluster.h"

namespace ClusterLod
{

namespace
{

    // The node tree is depth-first flattened by the builder, so the deepest leaf distance is the number of node
    // passes the traversal has to run.
    uint32_t GetTreeDepth(const PrimitiveClusters& clusters)
    {
        if (clusters.m_Nodes.empty())
            return 0;

        uint32_t maxDepth = 0;
        std::vector<std::pair<uint32_t, uint32_t>> stack;   // node index, depth
        stack.emplace_back(0, 0);

        while (!stack.empty())
        {
            const auto [nodeIndex, depth] = stack.back();
            stack.pop_back();
            maxDepth = std::max(maxDepth, depth);

            const Node& node = clusters.m_Nodes[nodeIndex];
            if (IsLeaf(node))
                continue;

            for (uint32_t i = 0; i < GetChildCount(node); ++i)
                stack.emplace_back(GetChildOffset(node) + i, depth + 1);
        }
        return maxDepth;
    }

    // Error floor for the log-space ratio below: level 0 has an error of exactly 0 on both sides.
    constexpr float kErrorFloor = 1e-6f;

    // The per-mesh LOD whose quadric error is closest to `clusterError`, in log space. That is the BLAS a discrete
    // instance at this cluster level is patched with, so ray tracing shows the same surface detail the raster
    // cluster of that level does. The per-mesh chain reduces by a different factor per level than the cluster
    // chain does, so the indices are not interchangeable.
    uint32_t FindMatchingMeshLOD(const srrhi::MeshData& meshData, float clusterError)
    {
        const uint32_t lodCount = std::clamp<uint32_t>(meshData.m_LODCount, 1u, srrhi::CommonConsts::MAX_LOD_COUNT);
        uint32_t bestLod = 0;
        float bestCost = FLT_MAX;
        for (uint32_t lod = 0; lod < lodCount; ++lod)
        {
            const float error = std::max(meshData.m_LODErrors[lod], 0.0f);
            const float cost = std::fabs(std::log((error + kErrorFloor) / (clusterError + kErrorFloor)));
            if (cost < bestCost)
            {
                bestCost = cost;
                bestLod = lod;
            }
        }
        return bestLod;
    }

    // One level's per-mesh LOD: the index whose quadric error is closest to this level's worst group. A discrete
    // instance is drawn from that level's clusters and patched with that BLAS, so the two paths show the same
    // surface detail.
    uint32_t BuildLevelMeshLOD(const PrimitiveClusters& clusters, uint32_t level, const srrhi::MeshData& meshData)
    {
        float levelError = 0.0f;
        bool bHasGroups = false;
        for (const Group& group : clusters.m_Groups)
        {
            if (group.m_LODLevel != level)
                continue;
            levelError = bHasGroups ? std::max(levelError, group.m_Metric.m_MaxQuadricError)
                                    : group.m_Metric.m_MaxQuadricError;
            bHasGroups = true;
        }

        return FindMatchingMeshLOD(meshData, levelError);
    }

}

uint64_t GpuData::GetMemoryBytes() const
{
    return uint64_t(m_Nodes.size()) * sizeof(srrhi::ClusterNode) +
        uint64_t(m_NodeBounds.size()) * sizeof(srrhi::ClusterBounds) +
        uint64_t(m_Groups.size()) * sizeof(srrhi::ClusterGroup) +
        uint64_t(m_GeneratingGroups.size()) * sizeof(uint32_t) +
        uint64_t(m_Geometry.size()) * sizeof(srrhi::ClusterGeometry) +
        uint64_t(m_Records.size()) * sizeof(srrhi::ClusterRecord) +
        uint64_t(m_LevelMeshLODs.size()) * sizeof(uint32_t) +
        uint64_t(m_StreamGroups.size()) * sizeof(StreamGroup);
}

Counts GetCounts(const PrimitiveClusters& clusters)
{
    Counts counts;
    counts.m_NodeCount = uint32_t(clusters.m_Nodes.size());
    counts.m_GroupCount = uint32_t(clusters.m_Groups.size());
    counts.m_ClusterCount = uint32_t(clusters.m_Clusters.size());
    counts.m_MaxTreeDepth = GetTreeDepth(clusters);
    return counts;
}

void BuildGpuData(const std::vector<Scene::Mesh>& meshes, const std::vector<srrhi::MeshData>& meshData,
                  GpuData& outData)
{
    outData.m_Nodes.clear();
    outData.m_NodeBounds.clear();
    outData.m_Groups.clear();
    outData.m_GeneratingGroups.clear();
    outData.m_Records.clear();
    outData.m_StreamGroups.clear();
    outData.m_LevelMeshLODs.clear();
    outData.m_PrimitivesWithoutData = 0;
    outData.m_Geometry.assign(meshData.size(), srrhi::ClusterGeometry{});

    uint32_t clusterBase = 0;

    for (const Scene::Mesh& mesh : meshes)
    {
        SDL_assert(mesh.m_Clusters.size() == mesh.m_Primitives.size());
        if (mesh.m_Clusters.size() != mesh.m_Primitives.size())
            continue;

        for (size_t pi = 0; pi < mesh.m_Primitives.size(); ++pi)
        {
            const PrimitiveClusters& clusters = mesh.m_Clusters[pi];
            if (clusters.IsEmpty())
            {
                outData.m_PrimitivesWithoutData++;
                continue;
            }

            const uint32_t meshDataIndex = mesh.m_Primitives[pi].m_MeshDataIndex;
            SDL_assert(meshDataIndex < outData.m_Geometry.size());
            if (meshDataIndex >= outData.m_Geometry.size())
                continue;

            srrhi::ClusterGeometry& geometry = outData.m_Geometry[meshDataIndex];
            const Counts counts = GetCounts(clusters);

            geometry.m_NodeOffset = uint32_t(outData.m_Nodes.size());
            geometry.m_NodeCount = counts.m_NodeCount;
            geometry.m_GroupOffset = uint32_t(outData.m_Groups.size());
            geometry.m_GroupCount = counts.m_GroupCount;
            geometry.m_ClusterOffset = clusterBase;
            geometry.m_ClusterCount = counts.m_ClusterCount;
            geometry.m_LODLevelCount = clusters.m_LODLevelCount;
            geometry.m_MaxTreeDepth = counts.m_MaxTreeDepth;

            // The per-mesh LOD each cluster level maps to, for the ray-traced BLAS the discrete path patches.
            geometry.m_LODLevelOffset = uint32_t(outData.m_LevelMeshLODs.size());
            SDL_assert(meshDataIndex < meshData.size());
            for (uint32_t level = 0; level < clusters.m_LODLevelCount; ++level)
            {
                outData.m_LevelMeshLODs.push_back(BuildLevelMeshLOD(clusters, level, meshData[meshDataIndex]));
            }

            // A cluster no coarser group replaced is drawn at any distance, so a discrete instance can only seed at
            // a level above every level that holds one.
            for (uint32_t c = 0; c < counts.m_ClusterCount; ++c)
            {
                if (clusters.m_GeneratingGroups[c] != kNoGroup)
                    continue;
                const uint32_t level = std::min(clusters.m_Clusters[c].m_LODLevel, 31u);
                geometry.m_UnreplacedLevels |= 1u << level;
            }

            // The builder numbers children, groups and clusters per primitive; the traversal walks the whole
            // scene through one shared buffer, so every index it follows has to be rebased here.
            for (uint32_t n = 0; n < counts.m_NodeCount; ++n)
            {
                const Node& node = clusters.m_Nodes[n];
                const uint32_t packed = IsLeaf(node)
                    ? MakeLeafNode(geometry.m_GroupOffset + GetGroupIndex(node), GetGroupClusterCount(node)).m_Packed
                    : MakeChildNode(geometry.m_NodeOffset + GetChildOffset(node), GetChildCount(node)).m_Packed;

                outData.m_Nodes.push_back(Node{ packed });
                outData.m_NodeBounds.push_back(clusters.m_NodeBounds[n]);
            }

            for (uint32_t g = 0; g < counts.m_GroupCount; ++g)
            {
                // The group is the shader's record verbatim; only its cluster range moves into the scene-wide array.
                Group gpuGroup = clusters.m_Groups[g];
                gpuGroup.m_ClusterOffset = geometry.m_ClusterOffset + gpuGroup.m_ClusterOffset;
                outData.m_Groups.push_back(gpuGroup);
            }

            // The cut rule is per cluster, so the generating group goes up as its own array.
            for (uint32_t c = 0; c < counts.m_ClusterCount; ++c)
            {
                const uint32_t generating = clusters.m_GeneratingGroups[c];
                outData.m_GeneratingGroups.push_back(generating == kNoGroup ? 0xFFFFFFFFu : geometry.m_GroupOffset + generating);
            }

            // The overlap table moves into the scene-wide link array, so both its offset and its links are rebased.
            SDL_assert(clusters.m_Overlaps.size() == size_t(counts.m_GroupCount));
            for (uint32_t g = 0; g < counts.m_GroupCount; ++g)
            {
                const GroupOverlap& local = clusters.m_Overlaps[g];
                const uint32_t linkCount = uint32_t(local.m_CoarseCount) + uint32_t(local.m_FineCount);
                SDL_assert(size_t(local.m_Offset) + linkCount <= clusters.m_OverlapGroups.size());

                GroupOverlap global;
                global.m_Offset = uint32_t(outData.m_OverlapGroups.size());
                global.m_CoarseCount = local.m_CoarseCount;
                global.m_FineCount = local.m_FineCount;

                for (uint32_t i = 0; i < linkCount; ++i)
                {
                    const uint32_t link = clusters.m_OverlapGroups[local.m_Offset + i];
                    SDL_assert(link < counts.m_GroupCount);
                    outData.m_OverlapGroups.push_back(geometry.m_GroupOffset + link);
                }
                outData.m_Overlaps.push_back(global);
            }

            // The draw side: the cooked Cluster packs its counts into bitfields, so it is expanded into the
            // shader's own record. The blob words are not built here: the cooked cache stores them per group in
            // exactly this order and GeometryStreaming uploads them from the file, so the offsets are arithmetic
            // - a group's triangles first, then its clusters' vertex attributes. A freshly cooked scene still has
            // the source arrays, and their micro/vertex indices are validated here; a loaded one has them empty.
            const bool bHasBlobData = !clusters.m_Triangles.empty();
            SDL_assert(clusters.m_Triangles.empty() == clusters.m_VertexIndices.empty());

            uint32_t triCursor = 0;
            uint32_t vertexCursor = 0;
            for (uint32_t c = 0; c < counts.m_ClusterCount; ++c)
            {
                const Cluster& cluster = clusters.m_Clusters[c];

                srrhi::ClusterRecord record;
                record.m_TriangleCount = cluster.GetTriangleCount();
                record.m_VertexCount = cluster.GetVertexCount();
                record.m_LODLevel = cluster.m_LODLevel;
                record.m_GroupIndex = 0;   // filled in below, from the group that owns the cluster
                record.m_TriangleOffset = triCursor;
                record.m_AttributeOffset = vertexCursor;
                record.m_CenterRadius[0] = cluster.m_CenterRadius[0];
                record.m_CenterRadius[1] = cluster.m_CenterRadius[1];

                triCursor += record.m_TriangleCount;
                vertexCursor += record.m_VertexCount;

                SDL_assert(record.m_TriangleCount <= srrhi::CommonConsts::kMaxClusterTriangles);
                SDL_assert(record.m_VertexCount <= srrhi::CommonConsts::kMaxClusterVertices);

                if (bHasBlobData)
                {
                    for (uint32_t t = 0; t < record.m_TriangleCount * 3; ++t)
                    {
                        SDL_assert(clusters.m_Triangles[cluster.m_TriangleOffset + t] < record.m_VertexCount);
                    }

                    for (uint32_t v = 0; v < record.m_VertexCount; ++v)
                    {
                        // A bad index turns the cluster's triangles into something that covers nothing, and the
                        // mesh shader draws nothing at all, so the CPU is the only place this can be caught.
                        SDL_assert(clusters.m_VertexIndices[cluster.m_VertexOffset + v] <
                            mesh.m_Primitives[pi].m_VertexCount);
                    }
                }

                outData.m_Records.push_back(record);
            }

            // A group names its clusters as a contiguous range of the primitive's own cluster array, so the
            // cluster record can point back at the bounds its cut decision was made with. The same loop turns
            // the record's element offsets group-local and records where the group's blob lives in the source
            // arrays: a group is the streaming unit, so its cluster data is always one contiguous run.
            uint32_t groupedClusters = 0;
            for (uint32_t g = 0; g < counts.m_GroupCount; ++g)
            {
                const Group& group = clusters.m_Groups[g];

                StreamGroup streamGroup;
                streamGroup.m_LODLevel = group.m_LODLevel;
                streamGroup.m_bCoarsestLevel = group.m_LODLevel + 1 == clusters.m_LODLevelCount;
                const uint32_t firstCluster = group.m_ClusterOffset;
                if (group.m_ClusterCount > 0)
                {
                    const srrhi::ClusterRecord& first = outData.m_Records[geometry.m_ClusterOffset + firstCluster];
                    streamGroup.m_TriangleElement = first.m_TriangleOffset;
                    streamGroup.m_VertexElement = first.m_AttributeOffset;
                }

                for (uint32_t i = 0; i < group.m_ClusterCount; ++i)
                {
                    const uint32_t localCluster = group.m_ClusterOffset + i;
                    SDL_assert(localCluster < counts.m_ClusterCount);

                    srrhi::ClusterRecord& record = outData.m_Records[geometry.m_ClusterOffset + localCluster];
                    record.m_GroupIndex = geometry.m_GroupOffset + g;

                    streamGroup.m_TriangleCount += record.m_TriangleCount;
                    streamGroup.m_VertexCount += record.m_VertexCount;
                }

                for (uint32_t i = 0; i < group.m_ClusterCount; ++i)
                {
                    srrhi::ClusterRecord& record = outData.m_Records[geometry.m_ClusterOffset + group.m_ClusterOffset + i];
                    record.m_TriangleOffset -= streamGroup.m_TriangleElement;
                    // Triangles come first in the blob, then each cluster's vertex attributes in cluster order:
                    // six words (a VertexQuantized) per vertex, so the cluster's run is dense.
                    record.m_AttributeOffset =
                        streamGroup.m_TriangleCount + (record.m_AttributeOffset - streamGroup.m_VertexElement) * 6u;
                }

                groupedClusters += group.m_ClusterCount;
                outData.m_StreamGroups.push_back(streamGroup);
            }

            // Every cluster has to belong to exactly one group, or a record's offsets would address a blob that
            // does not contain it and the mesh shader would read another group's geometry.
            SDL_assert(groupedClusters == counts.m_ClusterCount);

            clusterBase += counts.m_ClusterCount;
        }
    }
}

} // namespace ClusterLod
