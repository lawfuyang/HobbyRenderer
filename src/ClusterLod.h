#pragma once

#include "shaders/srrhi/cpp/Cluster.h"

// Offline cluster-LOD representation of one primitive (docs/cluster-lod-implementation-plan.md §3).
//
// Level 0 is the original mesh, clusterised into fixed-size clusters. A group of clusters is decimated to
// roughly half its triangles and re-clusterised to build the next level, so a cluster's replacement at the next
// coarser level is always a whole group. Groups are formed by `meshopt_partitionClusters` at every level, so a
// level-(L+1) group can take clusters from several level-L groups: the structure is a DAG, not a tree.
//
// The cut rule this data is built for (a cluster is drawn when its own group is detailed enough and every group
// that replaced it is not) is only watertight when a coarser group is never nearer the camera than the groups it
// replaces, which is what the sphere inflation in the builder guarantees.

namespace ClusterLod
{
    constexpr uint32_t kMaxLODLevels        = 32;   // GeometryClusters has one entry per level
    constexpr uint32_t kMaxClustersPerGroup = 256;  // the cluster-count field of a Node is 8 bits wide
    constexpr uint32_t kMaxGroupChildren    = 32;   // the child-count field of a Node is 5 bits wide
    constexpr uint32_t kNoGroup             = 0xFFFFFFFFu;

    // ── The records the GPU reads are the shader's own ────────────────────────────────────────────
    //
    // src/shaders/Cluster.sr generates these through srrhi, so the CPU and the traversal cannot drift and there
    // is no layout to police with sizeof asserts. `Cluster` is the exception, see below.
    using TraversalMetric = srrhi::ClusterTraversalMetric;   // 20 B: sphere xyz + radius, then the quadric error
    using Bounds          = srrhi::ClusterBounds;            // 32 B: min/shortest edge, max/longest edge
    using Node            = srrhi::ClusterNode;              // 4 B packed tree node
    using Group           = srrhi::ClusterGroup;             // 64 B: cluster range, level, metric, bounds

    // The node packing the generated struct cannot carry: bit 0 is the leaf flag, an interior node holds
    // (childOffset:26, childCount-1:5) and a leaf holds (groupIndex:23, groupClusterCount-1:8).
    inline bool IsLeaf(const Node& node) { return (node.m_Packed & 1u) != 0; }
    inline uint32_t GetChildOffset(const Node& node) { return node.m_Packed >> 6; }
    inline uint32_t GetChildCount(const Node& node) { return ((node.m_Packed >> 1) & 0x1Fu) + 1u; }
    inline uint32_t GetGroupIndex(const Node& node) { return (node.m_Packed >> 1) & 0x7FFFFFu; }
    inline uint32_t GetGroupClusterCount(const Node& node) { return ((node.m_Packed >> 24) & 0xFFu) + 1u; }
    Node MakeChildNode(uint32_t childOffset, uint32_t childCount);
    Node MakeLeafNode(uint32_t groupIndex, uint32_t clusterCount);

    // 20 B. A cluster's geometry is a cluster-local vertex range plus 8-bit micro indices into it. The offsets
    // are element offsets while the data lives on the CPU; the GPU blob (phase 2) stores byte offsets instead,
    // and HLSL has no bitfields, so this record stays CPU-only.
    //
    // m_CenterRadius is `meshopt_computeClusterBounds`'s bounding sphere, packed as halves. The traversal needs
    // it because the group's sphere covers several clusters and is far too loose to reject the hidden ones.
    struct Cluster
    {
        uint32_t m_TriangleCountMinusOne : 8;
        uint32_t m_VertexCountMinusOne   : 8;
        uint32_t m_LODLevel              : 8;
        uint32_t m_GroupChildIndex       : 8;
        uint32_t m_VertexOffset   = 0;  // into PrimitiveClusters::m_VertexIndices
        uint32_t m_TriangleOffset = 0;  // into PrimitiveClusters::m_Triangles, 3 micro indices per triangle

        uint32_t m_CenterRadius[2] = {};

        uint32_t GetTriangleCount() const { return m_TriangleCountMinusOne + 1u; }
        uint32_t GetVertexCount() const { return m_VertexCountMinusOne + 1u; }
    };
    static_assert(sizeof(Cluster) == 20);

    // The groups one LOD level coarser and one level finer than a group, derived from the generating-group links.
    // Upstream keeps the same table (scene_cluster_lod.cpp, buildGeometryLodOverlap): the child-to-parent link
    // alone cannot answer "who replaced this group" without scanning every cluster, which is what stream-in
    // ordering, speculative prefetch and the unload gate all need.
    struct GroupOverlap
    {
        uint32_t m_Offset = 0;        // into PrimitiveClusters::m_OverlapGroups
        uint16_t m_CoarseCount = 0;   // links [m_Offset, m_Offset + m_CoarseCount): groups that replaced this one
        uint16_t m_FineCount = 0;     // links [m_Offset + m_CoarseCount, + m_FineCount): groups this one replaced
    };
    static_assert(sizeof(GroupOverlap) == 8);

    // Everything the offline builder produced for one primitive, in primitive-local vertex indices.
    struct PrimitiveClusters
    {
        std::vector<Node> m_Nodes;                 // node[0] is the root, its children are the level roots
        std::vector<Bounds> m_NodeBounds;          // parallel to m_Nodes
        std::vector<Group> m_Groups;               // group.m_ClusterOffset indexes m_Clusters
        std::vector<Cluster> m_Clusters;
        std::vector<uint32_t> m_GeneratingGroups;  // per cluster: the group that replaced it at the next level
        std::vector<GroupOverlap> m_Overlaps;      // parallel to m_Groups
        std::vector<uint32_t> m_OverlapGroups;     // both link lists, group indices local to this primitive
        // Cook-time only. The cooked cache stores the group blobs in GPU order instead, and a loaded scene
        // leaves these two empty: the bytes stream from the file (see GroupBlobRef below).
        std::vector<uint8_t> m_Triangles;          // micro indices, 3 per triangle
        std::vector<uint32_t> m_VertexIndices;     // cluster-local -> primitive-local vertex index
        TraversalMetric m_LODMinSphere[kMaxLODLevels] = {};
        float m_LODMinMaxError[kMaxLODLevels] = {};
        uint32_t m_LODLevelCount = 0;

        bool IsEmpty() const { return m_LODLevelCount == 0; }
        uint32_t GetClusterTriangleCount(uint32_t clusterIndex) const;
        uint32_t GetClusterVertexCount(uint32_t clusterIndex) const;
        uint32_t GetLevelClusterCount(uint32_t level) const;
        uint32_t GetLevelGroupCount(uint32_t level) const;
        uint64_t GetMemoryBytes() const;
        void Clear();
    };

    // Where one group's words live in the cooked mesh file. The bytes are the group's arena blob verbatim:
    // `GetBlobElements()` words, 16-byte aligned, the group's clusters' triangles followed by every cluster
    // vertex's attributes (a VertexQuantized, six words each). A load is therefore one copy out of the mapping -
    // no group data has to be materialised in system RAM.
    struct GroupBlobRef
    {
        uint64_t m_FileOffset = 0;   // byte offset into the cooked mesh file
        uint32_t m_ByteSize = 0;
        uint32_t m_Unused = 0;       // keeps the record 16 B so the table reads as one vector
    };

    // The cooked file a scene's group blobs are streamed from, and the table that locates them. Filled by
    // SceneCache::LoadOrCookMeshData and consumed by GeometryStreaming at scene load.
    struct BlobFile
    {
        std::filesystem::path m_Path;
        std::vector<GroupBlobRef> m_Groups;   // global group order, parallel to GpuData::m_StreamGroups

        void Clear()
        {
            m_Path.clear();
            m_Groups.clear();
        }
    };

    // Clusterisation parameters. Every field is hashed into the cooked-mesh config hash, so changing one
    // invalidates the cache instead of silently mixing formats.
    struct Config
    {
        // The unit size is what decides how well occlusion culling works: a cluster's bounding sphere is what the
        // HZB test sees, and a sphere over 128 triangles pokes past an occluder that a meshlet's does not. These
        // match the meshlet path's own limits (kMaxMeshletVertices / kMaxMeshletTriangles) so both paths cull at
        // the same granularity.
        uint32_t m_MaxTriangles = 96;
        uint32_t m_MaxVertices = 64;
        // Partition target in clusters; the reference renderer uses 16, and a smaller group also means a tighter
        // sphere for the group-level test.
        uint32_t m_GroupSize = 16;
        float m_DecimationFactor = 0.5f;
        float m_ErrorMergePrev = 1.0f;
        float m_NormalWeight = 1.0f;
        float m_UvWeight = 1.0f;
        // A group that keeps more than this fraction of its triangles is stuck: it stays terminal instead of
        // producing a coarser level that would only cost error without buying much of a reduction.
        float m_SimplifyThreshold = 0.85f;
        uint32_t m_MaxLODLevels = kMaxLODLevels;
    };

    // Positions/normals/UVs as the loader has them (interleaved, arbitrary stride).
    struct BuildInput
    {
        const float* m_Positions = nullptr;
        size_t m_PositionStride = 0;
        const float* m_Normals = nullptr;        // optional, 3 floats
        size_t m_NormalStride = 0;
        const float* m_Uvs = nullptr;            // optional, 2 floats
        size_t m_UvStride = 0;
        size_t m_VertexCount = 0;
        const uint32_t* m_Indices = nullptr;
        size_t m_IndexCount = 0;
    };

    struct BuildStats
    {
        uint32_t m_ClusterCount = 0;
        uint32_t m_GroupCount = 0;
        uint32_t m_LODLevelCount = 0;
        uint64_t m_TriangleCountPerLevel[kMaxLODLevels] = {};
        float m_WorstErrorPerLevel[kMaxLODLevels] = {};
        double m_BuildMilliseconds = 0.0;
        // Where the build's time went, in milliseconds. The dedup, the level-0 clusterisation and the per-level
        // partition run on one thread; everything else already runs through the caller's parallel-for hook.
        double m_DedupMilliseconds = 0.0;
        double m_ClusterizeMilliseconds = 0.0;      // level 0, the whole primitive at once
        double m_AssignGroupsMilliseconds = 0.0;    // sum over levels, includes meshopt_partitionClusters
        double m_LocksMilliseconds = 0.0;           // sum over levels
        double m_SimplifyMilliseconds = 0.0;        // sum over levels, per-group decimation
        double m_NextLevelMilliseconds = 0.0;       // sum over levels, per-group clusterisation
        double m_EmitMilliseconds = 0.0;            // bounds, cluster and group records, level linking
        double m_FinalizeMilliseconds = 0.0;        // node trees and the level metrics after the last level
    };

    // The parameters this build clusterises with. They are part of the cooked-mesh cache hash, so a change here
    // invalidates existing caches instead of mixing two layouts.
    const Config& GetActiveConfig();

    // Optional parallelism hook for Build: runs `body(0..count-1)` concurrently; every index writes only its own
    // output, and the shared inputs are read-only. An empty hook runs the build serially. The caller must not
    // invoke Build from inside another parallel loop: the builder calls the hook on its own thread.
    using ParallelForFn = std::function<void(uint32_t count, const std::function<void(uint32_t)>& body)>;

    bool Build(const BuildInput& input, const Config& config, PrimitiveClusters& outClusters, BuildStats& outStats,
               const ParallelForFn& parallelFor = {});

    // ── Runtime side (CPU reference; the GPU path must match it cluster for cluster) ──────────────────

    struct CutParams
    {
        Vector3 m_ViewPosition = {};
        float m_WorldScale = 1.0f;
        float m_FocalPixels = 1000.0f;         // viewportHeight / (2 * tan(fovY / 2))
        float m_PixelErrorThreshold = 1.0f;
        int32_t m_ForcedLODLevel = -1;         // >= 0 selects one level wholesale (validation only)
    };

    struct CutResult
    {
        std::vector<uint32_t> m_Clusters;
        uint32_t m_TriangleCount = 0;
        uint32_t m_MinLODLevel = 0;
        uint32_t m_MaxLODLevel = 0;
    };

    // Error of a group, in pixels, at its nearest point.
    float GetGroupPixelError(const Group& group, const CutParams& params);

    void EvaluateCut(const PrimitiveClusters& clusters, const CutParams& params, CutResult& outCut);

    // Checks the structure and the metric chain (V0.1, V0.3 structural half). Returns false and fills
    // outError with the first problem found.
    bool ValidateDag(const PrimitiveClusters& clusters, std::string& outError);

    // Edge-multiset check of one cut (V0.2, V0.3): every edge of a selected cluster is either shared with exactly
    // one other selected cluster or is a border edge of the original mesh, and no region is covered twice. Edges
    // are compared in position space, because vertices split by a UV seam sit on top of each other and share no
    // index while still being the same point of the surface.
    bool ValidateCut(const PrimitiveClusters& clusters, const float* positions, size_t positionStride,
                     const uint32_t* level0Indices, size_t indexCount, const CutResult& cut, std::string& outError);

    // Sweeps EvaluateCut over a range of thresholds and over every forced level, validating each cut.
    bool ValidateCutSweep(const PrimitiveClusters& clusters, const float* positions, size_t positionStride,
                          const uint32_t* level0Indices, size_t indexCount, const CutParams& params,
                          std::string& outError);

    // Hash of everything that changes the cooked cluster data.
    uint64_t GetCacheConfigHash(const Config& config);

    // Identity of a vertex *position*. Two vertices split by a UV seam share no index but are the same point of
    // the surface, and the simplifier may keep either of them, so welding (locks, cut validation) works on this
    // hash and not on the vertex index.
    size_t HashPosition(const float* positions, size_t stride, uint32_t vertex);

    // Cache serialisation of one primitive's cluster data. The container format (magic, version, config hash)
    // belongs to SceneCache; this is the payload.
    void WriteToStream(std::ostream& os, const PrimitiveClusters& clusters);
    void ReadFromStream(std::istream& is, PrimitiveClusters& clusters);

    void LogStats(const PrimitiveClusters& clusters, const BuildStats& stats, std::string_view meshName);
}
