#pragma once

#include "Scene.h"

namespace SceneCache
{
    // ─────────────────────────────────────────────────────────────────────────────
    // Cooked Mesh Binary Format
    //
    // Offset  Size   Field
    // ------  ----   -----
    // 0       4      Magic: 0x59464C52 ("RLFY")
    // 4       4      Version: uint32_t
    // 8       8      Cluster config hash: uint64_t (ClusterLod::GetCacheConfigHash)
    // 16      var    Meshes:    [count:uint32_t][Mesh * count]
    //                          Mesh = [primCount:uint32_t][Primitive * primCount][m_Center:Vector3][m_Radius:float]
    //                                 [clusterCount:uint32_t][PrimitiveClusters * clusterCount]
    //                          Primitive = [m_VertexOffset:uint32_t][m_VertexCount:uint32_t][m_MaterialIndex:int32_t][m_MeshDataIndex:uint32_t]
    // var     var    MeshData:  [count:uint64_t][srrhi::MeshData * count]
    // var     var    Meshlets:  [count:uint64_t][srrhi::Meshlet * count]
    // var     var    MletVerts: [count:uint64_t][uint32_t * count]
    // var     var    MletTris:  [count:uint64_t][uint32_t * count]
    // var     var    VerticesQ: [count:uint64_t][srrhi::VertexQuantized * count]
    // var     var    Indices:   [count:uint64_t][uint32_t * count]
    // var     8      Group blobs: [count:uint64_t][ClusterLod::GroupBlobRef * count]
    // var     var    Blob data:  padded to the next 4 KiB; one 16-byte aligned blob per group, in the order
    //                            BuildGpuData flattens groups (mesh, primitive, group). Inserting the table here
    //                            keeps the per-primitive records small: a loaded scene streams the words from
    //                            this section instead of holding them in system RAM.
    // ─────────────────────────────────────────────────────────────────────────────

    // Magic: "RLFY" = 0x59464C52
    constexpr uint32_t kCookedMeshMagic = 0x59464C52;

    // Bump when:
    // - meshoptimizer library is upgraded to a new major version
    // - ProcessMeshes algorithm changes (new passes, different quantization)
    // - VertexQuantized or Meshlet struct layout changes
    // - LOD generation parameters change
    // 3: cluster LOD data written per primitive, plus the cluster config hash in the header.
    // 5: terminal (stuck) groups, regularised simplification and the meshoptimizer vertex dedup.
    // 8: cluster group blob words moved to their own section (a table plus 4 KiB-aligned blobs), so the runtime
    //    streams them from the file; the per-primitive index records no longer carry them.
    // 9: a group blob carries every cluster vertex's attributes after its triangles (six words each), so the
    //    cluster path no longer reads the scene-global vertex buffer.
    // 10: the cluster record carries the +-1 lod overlap table (one 8 b record per group plus its links).
    constexpr uint32_t kCookedMeshVersion = 10;

    // ── Streamed import: one part file per primitive ──────────────────────────
    //
    // A scene whose decoded glTF does not fit in RAM (zorah: 9.53 GB on disk, 41.69 GB decoded) cannot be cooked
    // by holding every primitive: the cook writes one part per primitive as soon as that primitive's clusters and
    // group blobs exist, and only the cluster metadata stays resident. A part is self-contained and final, so an
    // interrupted cook resumes by skipping the parts that are already there; AssembleCookedMesh then streams the
    // parts into the usual cooked-mesh layout, deleting each part as it is copied.
    //
    // Part file layout:
    //   0   4   Magic: 0x54524150 ("PART")
    //   4   4   Version: uint32_t
    //   8   ..  CookedPartHeader fields
    //   var     Cluster metadata: ClusterLod::WriteToStream(PrimitiveClusters)
    //   var     Group blob table: [count:uint32_t][ClusterLod::GroupBlobRef * count], offsets relative to the
    //           blob region below
    //   var     Blob region: the group blobs exactly as the arena holds them
    struct CookedPartHeader
    {
        uint32_t m_Magic = 0;
        uint32_t m_Version = 0;
        uint32_t m_MeshIndex = 0;
        uint32_t m_PrimitiveIndex = 0;
        // The triangle range this part was cooked from. A primitive above the split limit is several parts, and a
        // different split makes the parts of an earlier run describe different geometry: the reader compares the
        // range and re-imports the part when it does not match.
        uint32_t m_FirstTriangle = 0;
        uint32_t m_TriangleCount = 0;
        uint32_t m_VertexCount = 0;
        uint32_t m_IndexCount = 0;
        uint32_t m_LODCount = 0;
        uint32_t m_IndexCounts[srrhi::CommonConsts::MAX_LOD_COUNT] = {};
        float    m_Center[3] = {};
        float    m_Radius = 0.0f;
        uint64_t m_ClusterBytes = 0;   // metadata section, from the header to the blob table
        uint64_t m_BlobBytes = 0;      // blob region size
        uint32_t m_GroupCount = 0;
    };

    constexpr uint32_t kCookedPartMagic = 0x54524150;   // "PART"
    // 2: the cooked triangle range is recorded, so a changed --split-primitive-tris invalidates earlier parts.
    // 3: the part's cluster record carries the +-1 lod overlap table.
    constexpr uint32_t kCookedPartVersion = 3;

    // `<cachePath>.parts/<mesh>_<primitive>.part`
    std::filesystem::path GetPartPath(const std::filesystem::path& cachePath, uint32_t meshIndex, uint32_t primitiveIndex);

    // Writes one part: the cluster metadata plus every group's blob, packed from `vertices` (the part's own packed
    // vertex block, which is why the blob can carry attributes without the scene-wide array). Returns false and
    // leaves no file behind on failure.
    bool WriteCookedPart(const std::filesystem::path& partPath, uint32_t meshIndex, uint32_t primitiveIndex,
                         uint32_t firstTriangle, uint32_t triangleCount,
                         const ClusterLod::PrimitiveClusters& clusters,
                         const std::vector<srrhi::VertexQuantized>& vertices,
                         const srrhi::MeshData& meshData, const float center[3], float radius);

    // Header only: enough to rebuild the scene records of an already-imported primitive without reading its
    // geometry back.
    bool ReadCookedPartHeader(const std::filesystem::path& partPath, CookedPartHeader& header);

    // Header plus cluster metadata, for the assemble pass and for resuming.
    bool ReadCookedPart(const std::filesystem::path& partPath, CookedPartHeader& header,
                        ClusterLod::PrimitiveClusters& clusters);

    // Assembles the parts into a cooked mesh with no legacy vertex/index/meshlet arrays (the cluster path and the
    // group blobs are all a streamed scene has), streaming each part's blob region into the file. Parts are
    // deleted as they are copied, so peak disk is the cache plus one part.
    bool AssembleCookedMesh(const std::filesystem::path& cachePath, const std::vector<Scene::Mesh>& meshes,
                            const std::vector<srrhi::MeshData>& meshData, const std::filesystem::path& partsDir,
                            ClusterLod::BlobFile& outBlobs);

    // ── Binary I/O helpers ────────────────────────────────────────────────────

    template<typename T>
    inline void WritePOD(std::ostream& os, const T& value)
    {
        os.write(reinterpret_cast<const char*>(&value), sizeof(T));
    }

    template<typename T>
    inline void ReadPOD(std::istream& is, T& value)
    {
        is.read(reinterpret_cast<char*>(&value), sizeof(T));
    }

    template<typename T>
    inline void WriteVector(std::ostream& os, const std::vector<T>& vec)
    {
        const uint64_t count = static_cast<uint64_t>(vec.size());
        WritePOD(os, count);
        if (count > 0)
            os.write(reinterpret_cast<const char*>(vec.data()), count * sizeof(T));
    }

    template<typename T>
    inline void ReadVector(std::istream& is, std::vector<T>& vec)
    {
        uint64_t count = 0;
        ReadPOD(is, count);
        vec.resize(static_cast<size_t>(count));
        if (count > 0)
            is.read(reinterpret_cast<char*>(vec.data()), count * sizeof(T));
    }

    // ── Cooked mesh save/load ─────────────────────────────────────────────────

    // Save mesh-processed data to a binary file.
    // Returns true on success.
    bool SaveCookedMesh(
        const std::filesystem::path& cachePath,
        const std::vector<Scene::Mesh>&           meshes,
        const std::vector<srrhi::MeshData>&       meshData,
        const std::vector<srrhi::Meshlet>&        meshlets,
        const std::vector<uint32_t>&              meshletVertices,
        const std::vector<uint32_t>&              meshletTriangles,
        const std::vector<srrhi::VertexQuantized>& allVerticesQuantized,
        const std::vector<uint32_t>&              allIndices,
        bool                                      bCookClusters,
        ClusterLod::BlobFile&                     outBlobs);

    // Load mesh-processed data from a binary file.
    // Returns true on success; false if file is missing, corrupt, or wrong version.
    bool LoadCookedMesh(
        const std::filesystem::path& cachePath,
        std::vector<Scene::Mesh>&            outMeshes,
        std::vector<srrhi::MeshData>&        outMeshData,
        std::vector<srrhi::Meshlet>&         outMeshlets,
        std::vector<uint32_t>&               outMeshletVertices,
        std::vector<uint32_t>&               outMeshletTriangles,
        std::vector<srrhi::VertexQuantized>& outVerticesQuantized,
        std::vector<uint32_t>&               outIndices,
        bool                                 bCookClusters,
        ClusterLod::BlobFile&                outBlobs);

    // Check whether the cached file exists and is newer than the source file.
    bool IsCacheValid(const std::filesystem::path& cachePath,
                      const std::filesystem::path& sourcePath);

    // Try to load cooked mesh data from cache. If cache is valid, populate out parameters.
    // If cache is missing/outdated, process meshes from glTF and save the cache.
    // Returns true if mesh data was successfully loaded (from cache or from glTF).
    bool LoadOrCookMeshData(
        const std::filesystem::path& scenePath,
        Scene& scene,
        std::vector<srrhi::VertexQuantized>& outVerticesQuantized,
        std::vector<uint32_t>& outIndices,
        bool bCookClusters,
        ClusterLod::BlobFile& outBlobs);

} // namespace SceneCache
