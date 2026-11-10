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

uint32_t CountReferenceCutClusters(const Scene& scene, const Scene::BucketInfo& bucket, const ClusterLod::CutParams& params)
{
    // The traversal indexes its primitive through the instance's MeshDataIndex, so the reference has to do the same.
    std::unordered_map<uint32_t, const ClusterLod::PrimitiveClusters*> clustersByMeshData;
    for (const Scene::Mesh& mesh : scene.m_Meshes)
    {
        for (size_t pi = 0; pi < mesh.m_Primitives.size() && pi < mesh.m_Clusters.size(); ++pi)
        {
            clustersByMeshData[mesh.m_Primitives[pi].m_MeshDataIndex] = &mesh.m_Clusters[pi];
        }
    }

    // ClusterLod::EvaluateCut's rule, but with the group sphere taken to world space and the error scaled by the
    // instance's uniform scale. That is what the traversal compares; EvaluateCut works on untransformed primitive
    // data and is only valid for the identity instance.
    const auto groupPixelError = [&params](const ClusterLod::Group& group, const Matrix& world, float worldScale)
    {
        const DirectX::XMVECTOR local = DirectX::XMVectorSet(
            group.m_Metric.m_Sphere.x, group.m_Metric.m_Sphere.y, group.m_Metric.m_Sphere.z, 0.0f);
        const DirectX::XMVECTOR center = DirectX::XMVector3Transform(local, DirectX::XMLoadFloat4x4(&world));

        const float dx = DirectX::XMVectorGetX(center) - params.m_ViewPosition.x;
        const float dy = DirectX::XMVectorGetY(center) - params.m_ViewPosition.y;
        const float dz = DirectX::XMVectorGetZ(center) - params.m_ViewPosition.z;
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz) - group.m_Metric.m_Sphere.w * worldScale;
        const float nearest = std::max(distance, 1e-4f);

        return group.m_Metric.m_MaxQuadricError * worldScale * params.m_FocalPixels / nearest;
    };

    std::vector<uint8_t> tooCoarse;
    uint32_t total = 0;

    for (uint32_t i = 0; i < bucket.m_Count; ++i)
    {
        const srrhi::PerInstanceData& instance = scene.m_InstanceData[bucket.m_BaseIndex + i];

        const auto it = clustersByMeshData.find(instance.m_MeshDataIndex);
        if (it == clustersByMeshData.end() || it->second->IsEmpty())
            continue;

        const ClusterLod::PrimitiveClusters& clusters = *it->second;
        const Matrix& world = instance.m_World;

        const float worldScale = std::max({ std::sqrt(world._11 * world._11 + world._12 * world._12 + world._13 * world._13),
            std::sqrt(world._21 * world._21 + world._22 * world._22 + world._23 * world._23),
            std::sqrt(world._31 * world._31 + world._32 * world._32 + world._33 * world._33) });

        const bool bForcedLevel = params.m_ForcedLODLevel >= 0;
        const uint32_t forcedLevel = bForcedLevel
            ? std::min(uint32_t(params.m_ForcedLODLevel), clusters.m_LODLevelCount - 1)
            : 0u;

        tooCoarse.assign(clusters.m_Groups.size(), 0);
        for (size_t g = 0; g < clusters.m_Groups.size(); ++g)
        {
            tooCoarse[g] = groupPixelError(clusters.m_Groups[g], world, worldScale) >= params.m_PixelErrorThreshold ? 1 : 0;
        }

        for (size_t g = 0; g < clusters.m_Groups.size(); ++g)
        {
            const ClusterLod::Group& group = clusters.m_Groups[g];

            if (bForcedLevel)
            {
                if (group.m_LODLevel != forcedLevel)
                    continue;
            }
            else if (tooCoarse[g])
            {
                continue;
            }

            for (uint32_t c = 0; c < group.m_ClusterCount; ++c)
            {
                const uint32_t clusterIndex = group.m_ClusterOffset + c;

                if (!bForcedLevel)
                {
                    const uint32_t generating = clusters.m_GeneratingGroups[clusterIndex];
                    if (generating != ClusterLod::kNoGroup && !tooCoarse[generating])
                        continue;
                }

                total++;
            }
        }
    }

    return total;
}