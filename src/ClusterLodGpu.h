#pragma once

#include "Scene.h"

// GPU types generated from src/shaders/Cluster.sr (build_srrhi writes them into the source tree).
#include "shaders/srrhi/cpp/Cluster.h"

// Flattens the per-primitive cluster DAGs into the global arrays the traversal reads
// (docs/cluster-lod-implementation-plan.md §3.1). The GPU types come from src/shaders/Cluster.sr, so HLSL and
// C++ cannot drift; this file only decides how the per-primitive data is laid out in them.
namespace ClusterLod
{
    struct GpuData
    {
        std::vector<srrhi::ClusterNode> m_Nodes;
        std::vector<srrhi::ClusterBounds> m_NodeBounds;
        std::vector<srrhi::ClusterGroup> m_Groups;
        std::vector<uint32_t> m_GeneratingGroups;          // per cluster, global cluster order
        std::vector<srrhi::ClusterGeometry> m_Geometry;   // one per Scene::m_MeshData entry

        uint64_t GetMemoryBytes() const;
    };

    // Fills outData from `meshes`. A primitive without cluster data leaves its ClusterGeometry at
    // m_LODLevelCount == 0, which the traversal treats as "nothing to draw".
    void BuildGpuData(const std::vector<Scene::Mesh>& meshes, size_t meshDataCount, GpuData& outData);

    // Counts of one primitive, used to size the traversal queues and the visible-cluster list.
    struct Counts
    {
        uint32_t m_NodeCount = 0;
        uint32_t m_GroupCount = 0;
        uint32_t m_ClusterCount = 0;
        uint32_t m_MaxTreeDepth = 0;
    };
    Counts GetCounts(const PrimitiveClusters& clusters);
}
