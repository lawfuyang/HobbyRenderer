#include "ClusterLod.h"

#include "Utilities.h"

#include "meshoptimizer.h"

namespace ClusterLod
{

namespace
{

    uint32_t GetClusterGroup(const PrimitiveClusters& clusters, uint32_t clusterIndex)
    {
        // Groups own contiguous cluster runs, so the owning group is the last one starting at or before the cluster.
        uint32_t low = 0;
        uint32_t high = uint32_t(clusters.m_Groups.size());
        while (low + 1 < high)
        {
            const uint32_t mid = (low + high) / 2;
            if (clusters.m_Groups[mid].m_ClusterOffset <= clusterIndex)
                low = mid;
            else
                high = mid;
        }
        SDL_assert(clusters.m_Groups[low].m_ClusterOffset <= clusterIndex);
        SDL_assert(clusterIndex < clusters.m_Groups[low].m_ClusterOffset + clusters.m_Groups[low].m_ClusterCount);
        return low;
    }

    // Unordered edge key of two welded positions (a hash of a hash is fine here: this only has to group equal
    // edges together, and HashMap collisions cost a comparison that cannot produce a false match because both
    // sides go through the same function).
    size_t MakeEdgeKey(size_t a, size_t b)
    {
        return a < b ? (a * 0x9E3779B97F4A7C15ull) ^ b : (b * 0x9E3779B97F4A7C15ull) ^ a;
    }

}

size_t HashPosition(const float* positions, size_t stride, uint32_t vertex)
{
    const float* p = reinterpret_cast<const float*>(
        reinterpret_cast<const char*>(positions) + size_t(vertex) * stride);

    size_t hash = 0;
    for (uint32_t i = 0; i < 3; ++i)
        hash = HashCombine(hash, std::hash<float>{}(p[i]));
    return hash;
}

float GetGroupPixelError(const Group& group, const CutParams& params)
{
    const float dx = group.m_Metric.m_SphereX - params.m_ViewPosition.x;
    const float dy = group.m_Metric.m_SphereY - params.m_ViewPosition.y;
    const float dz = group.m_Metric.m_SphereZ - params.m_ViewPosition.z;
    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz) - group.m_Metric.m_SphereRadius * params.m_WorldScale;
    const float nearest = std::max(distance, 1e-4f);
    return group.m_Metric.m_MaxQuadricError * params.m_WorldScale * params.m_FocalPixels / nearest;
}

void EvaluateCut(const PrimitiveClusters& clusters, const CutParams& params, CutResult& outCut)
{
    outCut.m_Clusters.clear();
    outCut.m_TriangleCount = 0;
    outCut.m_MinLODLevel = UINT32_MAX;
    outCut.m_MaxLODLevel = 0;

    if (clusters.IsEmpty())
        return;

    if (params.m_ForcedLODLevel >= 0)
    {
        const uint32_t forced = std::min(uint32_t(params.m_ForcedLODLevel), clusters.m_LODLevelCount - 1);
        for (uint32_t c = 0; c < uint32_t(clusters.m_Clusters.size()); ++c)
        {
            const Cluster& cluster = clusters.m_Clusters[c];
            if (cluster.m_LODLevel != forced)
                continue;
            outCut.m_Clusters.push_back(c);
            outCut.m_TriangleCount += cluster.GetTriangleCount();
            outCut.m_MinLODLevel = std::min(outCut.m_MinLODLevel, uint32_t(cluster.m_LODLevel));
            outCut.m_MaxLODLevel = std::max(outCut.m_MaxLODLevel, uint32_t(cluster.m_LODLevel));
        }
        if (outCut.m_MinLODLevel == UINT32_MAX)
            outCut.m_MinLODLevel = 0;
        return;
    }

    std::vector<uint8_t> tooCoarse(clusters.m_Groups.size(), 0);
    for (size_t g = 0; g < clusters.m_Groups.size(); ++g)
        tooCoarse[g] = GetGroupPixelError(clusters.m_Groups[g], params) >= params.m_PixelErrorThreshold ? 1 : 0;

    for (size_t g = 0; g < clusters.m_Groups.size(); ++g)
    {
        if (tooCoarse[g])
            continue;

        const Group& group = clusters.m_Groups[g];
        for (uint32_t i = 0; i < group.m_ClusterCount; ++i)
        {
            const uint32_t clusterIndex = group.m_ClusterOffset + i;
            const Cluster& cluster = clusters.m_Clusters[clusterIndex];

            // Skip a cluster whose replacement at a coarser level is detailed enough: that coarser level covers
            // this region already.
            const uint32_t generating = clusters.m_GeneratingGroups[clusterIndex];
            if (generating != kNoGroup && !tooCoarse[generating])
                continue;

            outCut.m_Clusters.push_back(clusterIndex);
            outCut.m_TriangleCount += cluster.GetTriangleCount();
            outCut.m_MinLODLevel = std::min(outCut.m_MinLODLevel, uint32_t(cluster.m_LODLevel));
            outCut.m_MaxLODLevel = std::max(outCut.m_MaxLODLevel, uint32_t(cluster.m_LODLevel));
        }
    }

    if (outCut.m_MinLODLevel == UINT32_MAX)
        outCut.m_MinLODLevel = 0;
}

bool ValidateDag(const PrimitiveClusters& clusters, std::string& outError)
{
    outError.clear();

    if (clusters.m_LODLevelCount == 0 || clusters.m_LODLevelCount > kMaxLODLevels)
    {
        outError = "level count out of range";
        return false;
    }
    if (clusters.m_GeneratingGroups.size() != clusters.m_Clusters.size())
    {
        outError = "generating-group array is not parallel to the cluster array";
        return false;
    }
    if (clusters.m_Nodes.size() != clusters.m_NodeBounds.size() || clusters.m_Nodes.empty())
    {
        outError = "node/bounds arrays are not parallel or empty";
        return false;
    }
    if (clusters.m_Nodes[0].IsLeaf() || clusters.m_Nodes[0].GetChildCount() != clusters.m_LODLevelCount)
    {
        outError = "root node does not have one child per LOD level";
        return false;
    }

    // Structure: nodes, groups, clusters.
    for (size_t n = 0; n < clusters.m_Nodes.size(); ++n)
    {
        const Node& node = clusters.m_Nodes[n];
        if (node.IsLeaf())
        {
            const uint32_t groupIndex = node.GetGroupIndex();
            if (groupIndex >= clusters.m_Groups.size())
            {
                outError = "leaf node references group " + std::to_string(groupIndex);
                return false;
            }
            if (node.GetGroupClusterCount() != clusters.m_Groups[groupIndex].m_ClusterCount)
            {
                outError = "leaf node cluster count disagrees with group " + std::to_string(groupIndex);
                return false;
            }
            continue;
        }

        const uint64_t childEnd = uint64_t(node.GetChildOffset()) + node.GetChildCount();
        if (childEnd > clusters.m_Nodes.size() || node.GetChildCount() > kMaxGroupChildren)
        {
            outError = "node " + std::to_string(n) + " has children outside the node array";
            return false;
        }
    }

    uint32_t expectedOffset = 0;
    for (size_t g = 0; g < clusters.m_Groups.size(); ++g)
    {
        const Group& group = clusters.m_Groups[g];
        if (group.m_ClusterCount == 0 || group.m_ClusterCount > kMaxClustersPerGroup)
        {
            outError = "group " + std::to_string(g) + " has an unusable cluster count";
            return false;
        }
        if (group.m_ClusterOffset != expectedOffset)
        {
            outError = "groups do not tile the cluster array at group " + std::to_string(g);
            return false;
        }
        if (group.m_LODLevel >= clusters.m_LODLevelCount)
        {
            outError = "group " + std::to_string(g) + " has an out-of-range level";
            return false;
        }
        expectedOffset += group.m_ClusterCount;
    }
    if (expectedOffset != clusters.m_Clusters.size())
    {
        outError = "groups do not tile the cluster array";
        return false;
    }

    for (size_t c = 0; c < clusters.m_Clusters.size(); ++c)
    {
        const Cluster& cluster = clusters.m_Clusters[c];
        const Group& group = clusters.m_Groups[GetClusterGroup(clusters, uint32_t(c))];

        if (cluster.m_LODLevel != group.m_LODLevel)
        {
            outError = "cluster " + std::to_string(c) + " has a different level than its group";
            return false;
        }
        if (uint64_t(cluster.m_VertexOffset) + cluster.GetVertexCount() > clusters.m_VertexIndices.size())
        {
            outError = "cluster " + std::to_string(c) + " reads past the vertex index array";
            return false;
        }
        if (uint64_t(cluster.m_TriangleOffset) + uint64_t(cluster.GetTriangleCount()) * 3 > clusters.m_Triangles.size())
        {
            outError = "cluster " + std::to_string(c) + " reads past the micro index array";
            return false;
        }
        for (uint32_t t = 0; t < cluster.GetTriangleCount() * 3; ++t)
        {
            if (clusters.m_Triangles[cluster.m_TriangleOffset + t] >= cluster.GetVertexCount())
            {
                outError = "cluster " + std::to_string(c) + " has a micro index outside its vertex range";
                return false;
            }
        }

        // V0.1: the error never decreases along the DAG. A planar region legitimately simplifies at zero cost, so
        // this is not strict; strictness is not needed for the cut to stay exclusive.
        const uint32_t generating = clusters.m_GeneratingGroups[c];
        if (generating == kNoGroup)
            continue;

        if (generating >= clusters.m_Groups.size())
        {
            outError = "cluster " + std::to_string(c) + " references generating group " + std::to_string(generating);
            return false;
        }
        const Group& replacement = clusters.m_Groups[generating];
        if (replacement.m_LODLevel != group.m_LODLevel + 1)
        {
            outError = "cluster " + std::to_string(c) + " is replaced by a group that is not exactly one level coarser";
            return false;
        }
        if (replacement.m_Metric.m_MaxQuadricError < group.m_Metric.m_MaxQuadricError)
        {
            outError = "cluster " + std::to_string(c) + " is replaced by a group with a smaller error";
            return false;
        }
    }

    for (uint32_t level = 1; level < clusters.m_LODLevelCount; ++level)
    {
        if (clusters.m_LODMinMaxError[level] < clusters.m_LODMinMaxError[level - 1])
        {
            outError = "level " + std::to_string(level) + " does not have a larger worst error than the level below";
            return false;
        }
    }
    if (clusters.m_LODMinMaxError[0] != 0.0f)
    {
        outError = "level 0 has a non-zero error";
        return false;
    }
    // Every group must be reachable from the root through a node of its own level.
    std::vector<uint8_t> groupSeen(clusters.m_Groups.size(), 0);
    std::vector<uint32_t> stack;
    std::vector<uint32_t> nodeLevel(clusters.m_Nodes.size(), UINT32_MAX);

    const Node& root = clusters.m_Nodes[0];
    for (uint32_t i = 0; i < root.GetChildCount(); ++i)
    {
        const uint32_t child = root.GetChildOffset() + i;
        nodeLevel[child] = i;
        stack.push_back(child);
    }
    if (root.GetChildCount() != clusters.m_LODLevelCount)
    {
        outError = "level roots do not match the level count";
        return false;
    }

    while (!stack.empty())
    {
        const uint32_t nodeIndex = stack.back();
        stack.pop_back();
        const Node& node = clusters.m_Nodes[nodeIndex];
        const uint32_t level = nodeLevel[nodeIndex];

        if (node.IsLeaf())
        {
            const uint32_t groupIndex = node.GetGroupIndex();
            if (clusters.m_Groups[groupIndex].m_LODLevel != level)
            {
                outError = "leaf node of level " + std::to_string(level) + " wraps a group of another level";
                return false;
            }
            if (groupSeen[groupIndex])
            {
                outError = "group " + std::to_string(groupIndex) + " is wrapped by more than one leaf";
                return false;
            }
            groupSeen[groupIndex] = 1;
            continue;
        }

        for (uint32_t i = 0; i < node.GetChildCount(); ++i)
        {
            const uint32_t child = node.GetChildOffset() + i;
            nodeLevel[child] = level;
            stack.push_back(child);
        }
    }

    for (size_t g = 0; g < groupSeen.size(); ++g)
    {
        if (!groupSeen[g])
        {
            outError = "group " + std::to_string(g) + " is not reachable from the root";
            return false;
        }
    }

    return true;
}

bool ValidateCut(const PrimitiveClusters& clusters, const float* positions, size_t positionStride,
                 const uint32_t* level0Indices, size_t indexCount, const CutResult& cut, std::string& outError)
{
    outError.clear();

    std::vector<uint64_t> positionHashes;
    auto positionHash = [&](uint32_t vertex)
    {
        if (positionHashes.size() <= vertex)
            positionHashes.resize(size_t(vertex) + 1, 0);
        if (positionHashes[vertex] == 0)
            positionHashes[vertex] = HashPosition(positions, positionStride, vertex);
        return positionHashes[vertex];
    };

    std::unordered_map<size_t, uint32_t> meshEdges;
    for (size_t t = 0; t + 2 < indexCount; t += 3)
    {
        size_t vertex[3];
        for (size_t i = 0; i < 3; ++i)
            vertex[i] = positionHash(level0Indices[t + i]);
        if (vertex[0] == vertex[1] || vertex[1] == vertex[2] || vertex[0] == vertex[2])
            continue;

        for (size_t e = 0; e < 3; ++e)
            meshEdges[MakeEdgeKey(vertex[e], vertex[(e + 1) % 3])]++;
    }

    std::vector<uint8_t> selectedGroup(clusters.m_Groups.size(), 0);
    std::unordered_map<size_t, uint32_t> cutEdges;

    for (uint32_t clusterIndex : cut.m_Clusters)
    {
        const Cluster& cluster = clusters.m_Clusters[clusterIndex];
        const uint32_t group = GetClusterGroup(clusters, clusterIndex);
        selectedGroup[group] = 1;

        for (uint32_t t = 0; t < cluster.GetTriangleCount(); ++t)
        {
            uint64_t vertex[3];
            for (uint32_t i = 0; i < 3; ++i)
            {
                const uint32_t micro = clusters.m_Triangles[cluster.m_TriangleOffset + t * 3 + i];
                vertex[i] = positionHash(clusters.m_VertexIndices[cluster.m_VertexOffset + micro]);
            }
            if (vertex[0] == vertex[1] || vertex[1] == vertex[2] || vertex[0] == vertex[2])
                continue;

            for (uint32_t e = 0; e < 3; ++e)
                cutEdges[MakeEdgeKey(vertex[e], vertex[(e + 1) % 3])]++;
        }
    }

    uint32_t unmatchedEdges = 0;
    uint32_t duplicateEdges = 0;
    for (const std::pair<const uint64_t, uint32_t>& entry : cutEdges)
    {
        if (entry.second == 2)
            continue;

        const auto meshEdge = meshEdges.find(entry.first);
        const uint32_t meshCount = meshEdge == meshEdges.end() ? 0u : meshEdge->second;

        if (entry.second == 1)
        {
            // An edge only one selected triangle uses has to exist in the original mesh. If it does not, a vertex
            // on that border was collapsed away: that is a crack. (Authored T-junctions fail the stricter
            // "exactly two" rule without being cracks, so the source is the reference.)
            if (meshCount == 0)
                unmatchedEdges++;
            continue;
        }

        // Three or more triangles on one edge is normal in authored assets (UV seams weld two pairs into four,
        // non-manifold borders, T-junctions), and decimation can fold a sliver onto one edge. Only a large number
        // of such edges means clusters covering the same surface, which is what this check exists for.
        if (entry.second > meshCount)
            duplicateEdges++;
    }

    if (unmatchedEdges > 0)
    {
        outError = std::to_string(unmatchedEdges) + " edge(s) of the cut are unmatched and are not border edges of the mesh";
        return false;
    }

    if (duplicateEdges > cutEdges.size() / 100)
    {
        outError = std::to_string(duplicateEdges) + " of " + std::to_string(cutEdges.size()) +
            " edges of the cut are covered more often than in the mesh";
        return false;
    }

    // V0.3: no selected cluster may have a selected replacement, i.e. two levels never cover one region.
    for (uint32_t clusterIndex : cut.m_Clusters)
    {
        const uint32_t generating = clusters.m_GeneratingGroups[clusterIndex];
        if (generating == kNoGroup)
            continue;
        if (selectedGroup[generating])
        {
            outError = "cluster " + std::to_string(clusterIndex) + " and the group that replaced it are both selected";
            return false;
        }
    }

    return true;
}

bool ValidateCutSweep(const PrimitiveClusters& clusters, const float* positions, size_t positionStride,
                      const uint32_t* level0Indices, size_t indexCount, const CutParams& params, std::string& outError)
{
    static const float kThresholds[] = { 0.05f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 16.0f, 64.0f, 256.0f, 1.0e6f };

    CutResult cut;

    // A single level must tile the mesh on its own, whatever level it is.
    for (uint32_t level = 0; level < clusters.m_LODLevelCount; ++level)
    {
        CutParams forced = params;
        forced.m_ForcedLODLevel = int32_t(level);
        EvaluateCut(clusters, forced, cut);

        std::string error;
        if (!ValidateCut(clusters, positions, positionStride, level0Indices, indexCount, cut, error))
        {
            outError = "forced level " + std::to_string(level) + ": " + error;
            return false;
        }
    }

    for (float threshold : kThresholds)
    {
        CutParams sweep = params;
        sweep.m_PixelErrorThreshold = threshold;
        EvaluateCut(clusters, sweep, cut);

        std::string error;
        if (!ValidateCut(clusters, positions, positionStride, level0Indices, indexCount, cut, error))
        {
            outError = "threshold " + std::to_string(threshold) + ": " + error;
            return false;
        }
    }

    return true;
}

const Config& GetActiveConfig()
{
    static const Config config;
    return config;
}

uint64_t GetCacheConfigHash(const Config& config)
{
    size_t hash = 0;
    hash = HashCombine(hash, 0x434C555354455231ull); // "CLUSTER1": layout tag, bump with the structs
    hash = HashCombine(hash, config.m_MaxTriangles);
    hash = HashCombine(hash, config.m_MaxVertices);
    hash = HashCombine(hash, config.m_GroupSize);
    hash = HashCombine(hash, size_t(config.m_DecimationFactor * 100000.0f));
    hash = HashCombine(hash, size_t(config.m_ErrorMergePrev * 100000.0f));
    hash = HashCombine(hash, size_t(config.m_NormalWeight * 100000.0f));
    hash = HashCombine(hash, size_t(config.m_UvWeight * 100000.0f));
    hash = HashCombine(hash, size_t(MESHOPTIMIZER_VERSION));
    return hash;
}

void LogStats(const PrimitiveClusters& clusters, const BuildStats& stats, std::string_view meshName)
{
    SDL_Log("[ClusterLod] %.*s: %u clusters, %u groups, %u levels, %.2f MB, %.1f ms",
        int(meshName.size()), meshName.data(), stats.m_ClusterCount, stats.m_GroupCount, stats.m_LODLevelCount,
        double(clusters.GetMemoryBytes()) / (1024.0 * 1024.0), stats.m_BuildMilliseconds);

    for (uint32_t level = 0; level < stats.m_LODLevelCount; ++level)
    {
        SDL_Log("[ClusterLod]   level %2u: %8u clusters, %8u groups, %9llu triangles, worst error %.6f",
            level, clusters.GetLevelClusterCount(level), clusters.GetLevelGroupCount(level),
            static_cast<unsigned long long>(stats.m_TriangleCountPerLevel[level]),
            double(stats.m_WorstErrorPerLevel[level]));
    }
}

void WriteToStream(std::ostream& os, const PrimitiveClusters& clusters)
{
    auto writeVector = [&os](const auto& values)
    {
        using Value = typename std::decay_t<decltype(values)>::value_type;
        const uint64_t count = static_cast<uint64_t>(values.size());
        os.write(reinterpret_cast<const char*>(&count), sizeof(count));
        if (count > 0)
            os.write(reinterpret_cast<const char*>(values.data()), count * sizeof(Value));
    };

    writeVector(clusters.m_Nodes);
    writeVector(clusters.m_NodeBounds);
    writeVector(clusters.m_Groups);
    writeVector(clusters.m_Clusters);
    writeVector(clusters.m_GeneratingGroups);
    writeVector(clusters.m_Triangles);
    writeVector(clusters.m_VertexIndices);
    os.write(reinterpret_cast<const char*>(&clusters.m_LODLevelCount), sizeof(clusters.m_LODLevelCount));
    os.write(reinterpret_cast<const char*>(clusters.m_LODMinSphere), sizeof(clusters.m_LODMinSphere));
    os.write(reinterpret_cast<const char*>(clusters.m_LODMinMaxError), sizeof(clusters.m_LODMinMaxError));
}

void ReadFromStream(std::istream& is, PrimitiveClusters& clusters)
{
    auto readVector = [&is](auto& values)
    {
        using Value = typename std::decay_t<decltype(values)>::value_type;
        uint64_t count = 0;
        is.read(reinterpret_cast<char*>(&count), sizeof(count));
        values.resize(static_cast<size_t>(count));
        if (count > 0)
            is.read(reinterpret_cast<char*>(values.data()), count * sizeof(Value));
    };

    readVector(clusters.m_Nodes);
    readVector(clusters.m_NodeBounds);
    readVector(clusters.m_Groups);
    readVector(clusters.m_Clusters);
    readVector(clusters.m_GeneratingGroups);
    readVector(clusters.m_Triangles);
    readVector(clusters.m_VertexIndices);
    is.read(reinterpret_cast<char*>(&clusters.m_LODLevelCount), sizeof(clusters.m_LODLevelCount));
    is.read(reinterpret_cast<char*>(clusters.m_LODMinSphere), sizeof(clusters.m_LODMinSphere));
    is.read(reinterpret_cast<char*>(clusters.m_LODMinMaxError), sizeof(clusters.m_LODMinMaxError));
}

} // namespace ClusterLod
