#include "ClusterLodGpu.h"

#include "ClusterLod.h"
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

}

uint64_t GpuData::GetMemoryBytes() const
{
    return uint64_t(m_Nodes.size()) * sizeof(srrhi::ClusterNode) +
        uint64_t(m_NodeBounds.size()) * sizeof(srrhi::ClusterBounds) +
        uint64_t(m_Groups.size()) * sizeof(srrhi::ClusterGroup) +
        uint64_t(m_GeneratingGroups.size()) * sizeof(uint32_t) +
        uint64_t(m_Geometry.size()) * sizeof(srrhi::ClusterGeometry);
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

void BuildGpuData(const std::vector<Scene::Mesh>& meshes, size_t meshDataCount, GpuData& outData)
{
    outData.m_Nodes.clear();
    outData.m_NodeBounds.clear();
    outData.m_Groups.clear();
    outData.m_GeneratingGroups.clear();
    outData.m_Geometry.assign(meshDataCount, srrhi::ClusterGeometry{});

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
                continue;

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

            clusterBase += counts.m_ClusterCount;
        }
    }
}

} // namespace ClusterLod
