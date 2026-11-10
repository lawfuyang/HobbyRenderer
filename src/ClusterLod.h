#pragma once

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

    // 20 B. Everything traversal needs about a group.
    struct TraversalMetric
    {
        float m_SphereX = 0.0f;
        float m_SphereY = 0.0f;
        float m_SphereZ = 0.0f;
        float m_SphereRadius = 0.0f;
        float m_MaxQuadricError = 0.0f;
    };
    static_assert(sizeof(TraversalMetric) == 20);

    // 32 B. Culling bounds: the shortest/longest edge drive cone and backface rejection on the GPU.
    struct Bounds
    {
        Vector3 m_Min = {};
        float m_ShortestEdge = 0.0f;
        Vector3 m_Max = {};
        float m_LongestEdge = 0.0f;
    };
    static_assert(sizeof(Bounds) == 32);

    // 4 B. Bit 0 is the leaf flag: interior nodes carry (childOffset:26, childCount-1:5), leaves carry
    // (groupIndex:23, groupClusterCount-1:8).
    struct Node
    {
        uint32_t m_Packed = 0;

        bool IsLeaf() const { return (m_Packed & 1u) != 0; }
        uint32_t GetChildOffset() const { return m_Packed >> 6; }
        uint32_t GetChildCount() const { return ((m_Packed >> 1) & 0x1Fu) + 1u; }
        uint32_t GetGroupIndex() const { return (m_Packed >> 1) & 0x7FFFFFu; }
        uint32_t GetGroupClusterCount() const { return ((m_Packed >> 24) & 0xFFu) + 1u; }

        static Node MakeChildren(uint32_t childOffset, uint32_t childCount);
        static Node MakeLeaf(uint32_t groupIndex, uint32_t clusterCount);
    };
    static_assert(sizeof(Node) == 4);

    // 12 B. A cluster's geometry is a cluster-local vertex range plus 8-bit micro indices into it. The offsets
    // are element offsets while the data lives on the CPU; the GPU blob (phase 1) stores byte offsets instead.
    struct Cluster
    {
        uint32_t m_TriangleCountMinusOne : 8;
        uint32_t m_VertexCountMinusOne   : 8;
        uint32_t m_LODLevel              : 8;
        uint32_t m_GroupChildIndex       : 8;
        uint32_t m_VertexOffset   = 0;  // into PrimitiveClusters::m_VertexIndices
        uint32_t m_TriangleOffset = 0;  // into PrimitiveClusters::m_Triangles, 3 micro indices per triangle

        uint32_t GetTriangleCount() const { return m_TriangleCountMinusOne + 1u; }
        uint32_t GetVertexCount() const { return m_VertexCountMinusOne + 1u; }
    };
    static_assert(sizeof(Cluster) == 12);

    // A group owns a contiguous run of clusters and is the unit of the cut decision.
    struct Group
    {
        uint32_t m_ClusterOffset = 0;  // into PrimitiveClusters::m_Clusters
        uint32_t m_ClusterCount = 0;
        uint32_t m_LODLevel = 0;
        TraversalMetric m_Metric;
        Bounds m_Bounds;
    };

    // Everything the offline builder produced for one primitive, in primitive-local vertex indices.
    struct PrimitiveClusters
    {
        std::vector<Node> m_Nodes;                 // node[0] is the root, its children are the level roots
        std::vector<Bounds> m_NodeBounds;          // parallel to m_Nodes
        std::vector<Group> m_Groups;
        std::vector<Cluster> m_Clusters;
        std::vector<uint32_t> m_GeneratingGroups;  // per cluster: the group that replaced it at the next level
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

    // Clusterisation parameters. Every field is hashed into the cooked-mesh config hash, so changing one
    // invalidates the cache instead of silently mixing formats.
    struct Config
    {
        uint32_t m_MaxTriangles = 128;
        uint32_t m_MaxVertices = 128;
        uint32_t m_GroupSize = 32;
        float m_DecimationFactor = 0.5f;
        float m_ErrorMergePrev = 1.5f;
        float m_NormalWeight = 1.0f;
        float m_UvWeight = 1.0f;
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
    };

    // The parameters this build clusterises with. They are part of the cooked-mesh cache hash, so a change here
    // invalidates existing caches instead of mixing two layouts.
    const Config& GetActiveConfig();

    bool Build(const BuildInput& input, const Config& config, PrimitiveClusters& outClusters, BuildStats& outStats);

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
