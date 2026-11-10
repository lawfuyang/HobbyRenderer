#include "ClusterLodScene.h"

// Aggregated view of the cluster DAGs a scene was cooked with. The cut checks (V0.2, V0.3) need the vertex
// positions, which are not kept on the CPU after upload: they run during the cook instead, where
// SceneLoader has them (see SceneLoader::ProcessMeshes, --cluster-stats).

void LogSceneClusterStats(const Scene& scene)
{
    uint64_t memoryBytes = 0;
    uint64_t trianglesPerLevel[ClusterLod::kMaxLODLevels] = {};
    uint32_t clustersPerLevel[ClusterLod::kMaxLODLevels] = {};
    uint32_t groupsPerLevel[ClusterLod::kMaxLODLevels] = {};
    float worstErrorPerLevel[ClusterLod::kMaxLODLevels] = {};
    uint32_t primitivesWithClusters = 0;
    uint32_t primitiveCount = 0;
    uint32_t maxLevelCount = 0;
    uint32_t dagFailures = 0;

    for (const Scene::Mesh& mesh : scene.m_Meshes)
    {
        for (size_t pi = 0; pi < mesh.m_Primitives.size(); ++pi)
        {
            primitiveCount++;
            if (pi >= mesh.m_Clusters.size())
                continue;

            const ClusterLod::PrimitiveClusters& clusters = mesh.m_Clusters[pi];
            if (clusters.IsEmpty())
                continue;

            primitivesWithClusters++;
            memoryBytes += clusters.GetMemoryBytes();
            maxLevelCount = std::max(maxLevelCount, clusters.m_LODLevelCount);

            for (const ClusterLod::Cluster& cluster : clusters.m_Clusters)
            {
                const uint32_t level = std::min(cluster.m_LODLevel, ClusterLod::kMaxLODLevels - 1);
                trianglesPerLevel[level] += cluster.GetTriangleCount();
                clustersPerLevel[level]++;
            }
            for (const ClusterLod::Group& group : clusters.m_Groups)
            {
                const uint32_t level = std::min(group.m_LODLevel, ClusterLod::kMaxLODLevels - 1);
                groupsPerLevel[level]++;
                worstErrorPerLevel[level] = std::max(worstErrorPerLevel[level], group.m_Metric.m_MaxQuadricError);
            }

            std::string error;
            if (!ClusterLod::ValidateDag(clusters, error))
            {
                dagFailures++;
                SDL_Log("[ClusterLod] DAG check FAILED (mesh primitive %zu): %s", pi, error.c_str());
            }
        }
    }

    SDL_Log("[ClusterLod] %zu meshes, %u of %u primitives with a cluster DAG: %u levels, %.2f MB",
        scene.m_Meshes.size(), primitivesWithClusters, primitiveCount, maxLevelCount,
        double(memoryBytes) / (1024.0 * 1024.0));

    for (uint32_t level = 0; level < maxLevelCount; ++level)
    {
        SDL_Log("[ClusterLod]   level %2u: %8u clusters, %8u groups, %10llu triangles, worst error %.6f",
            level, clustersPerLevel[level], groupsPerLevel[level],
            static_cast<unsigned long long>(trianglesPerLevel[level]), double(worstErrorPerLevel[level]));
    }

    SDL_Log("[ClusterLod] phase 0 DAG gate: %s (%u failures)", dagFailures == 0 ? "PASS" : "FAIL", dagFailures);
}
