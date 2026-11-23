#pragma once

#include "Scene.h"

// GPU types generated from src/shaders/Cluster.sr (build_srrhi writes them into the source tree).
#include "shaders/srrhi/cpp/Cluster.h"

// Flattens the per-primitive cluster DAGs into the global arrays the traversal reads
// (docs/cluster-lod-implementation-plan.md §3.1). The GPU types come from src/shaders/Cluster.sr, so HLSL and
// C++ cannot drift; this file only decides how the per-primitive data is laid out in them.
namespace ClusterLod
{
    // One group's share of the streaming arena: its triangle words followed by its vertex attributes, 16-byte
    // aligned. The record offsets in m_Records are relative to this blob, so moving a group only rewrites its
    // address in the residency table (ClusterGroupState) and never the cluster data itself. The words live in the
    // cooked file (ClusterLod::GroupBlobRef), not in RAM; the two element fields are primitive-local cursors used
    // while BuildGpuData rebases the records, nothing reads them afterwards.
    struct StreamGroup
    {
        uint32_t m_TriangleElement = 0;   // primitive-local first triangle triple
        uint32_t m_TriangleCount = 0;
        uint32_t m_VertexElement = 0;     // primitive-local first vertex index
        uint32_t m_VertexCount = 0;
        uint32_t m_LODLevel = 0;          // level the group belongs to, for the per-level residency histogram
        // The coarsest level of its primitive: the set a bounded arena preloads, because it covers the primitive
        // at the fewest triangles.
        bool m_bCoarsestLevel = false;

        // Elements the blob occupies in the arena, padded to the 16-byte alignment a group address promises.
        // Every cluster vertex carries its own VertexQuantized (six words), which is what lets the arena be the
        // only copy of the cut's vertices.
        uint32_t GetBlobElements() const { return ((m_TriangleCount + 6u * m_VertexCount) + 3u) & ~3u; }
    };

    struct GpuData
    {
        std::vector<srrhi::ClusterNode> m_Nodes;
        std::vector<srrhi::ClusterBounds> m_NodeBounds;
        std::vector<srrhi::ClusterGroup> m_Groups;
        std::vector<uint32_t> m_GeneratingGroups;          // per cluster, global cluster order
        std::vector<srrhi::ClusterGeometry> m_Geometry;   // one per Scene::m_MeshData entry

        // One entry per LOD level, each geometry's levels contiguous from ClusterGeometry::m_LODLevelOffset: the
        // per-mesh LOD the ray-tracing BLAS pick uses for a discrete instance at that level
        // (docs/cluster-lod-implementation-plan.md phase 6).
        std::vector<uint32_t> m_LevelMeshLODs;

        // The draw side: one record per cluster, the micro indices of every cluster's triangles and the
        // scene-wide vertex index of every cluster vertex. The mesh shader reads these through ClusterRecord,
        // with the record's offsets relative to the group's StreamGroup blob. The words themselves live in the
        // cooked file: `GpuData` only carries the geometry, not a system-RAM copy of every blob.
        std::vector<srrhi::ClusterRecord> m_Records;
        std::vector<StreamGroup> m_StreamGroups;

        // Primitives the cluster path cannot draw, because they have no cluster data (fewer than three indices,
        // or a failed build). They disappear once the cluster base pass replaces the meshlet draw.
        uint32_t m_PrimitivesWithoutData = 0;

        uint64_t GetMemoryBytes() const;
    };

    // Fills outData from `meshes`. A primitive without cluster data leaves its ClusterGeometry at
    // m_LODLevelCount == 0, which the traversal treats as "nothing to draw". `meshData` is the cooked mesh array
    // (Scene::m_MeshData): its per-mesh LOD errors decide which BLAS a discrete cluster level maps to.
    void BuildGpuData(const std::vector<Scene::Mesh>& meshes, const std::vector<srrhi::MeshData>& meshData,
                      GpuData& outData);

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
