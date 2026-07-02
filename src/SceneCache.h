#pragma once

#include "Scene.h"

namespace SceneCache
{
    // ─────────────────────────────────────────────────────────────────────────────
    // Cooked Mesh Binary Format (version 1)
    //
    // Offset  Size   Field
    // ------  ----   -----
    // 0       4      Magic: 0x59464C52 ("RLFY")
    // 4       4      Version: uint32_t
    // 8       var    Meshes:    [count:uint32_t][Mesh * count]
    //                          Mesh = [primCount:uint32_t][Primitive * primCount][m_Center:Vector3][m_Radius:float]
    //                          Primitive = [m_VertexOffset:uint32_t][m_VertexCount:uint32_t][m_MaterialIndex:int32_t][m_MeshDataIndex:uint32_t]
    // var     var    MeshData:  [count:uint64_t][srrhi::MeshData * count]
    // var     var    Meshlets:  [count:uint64_t][srrhi::Meshlet * count]
    // var     var    MletVerts: [count:uint64_t][uint32_t * count]
    // var     var    MletTris:  [count:uint64_t][uint32_t * count]
    // var     var    VerticesQ: [count:uint64_t][srrhi::VertexQuantized * count]
    // var     var    Indices:   [count:uint64_t][uint32_t * count]
    // ─────────────────────────────────────────────────────────────────────────────

    // Magic: "RLFY" = 0x59464C52
    constexpr uint32_t kCookedMeshMagic = 0x59464C52;

    // Bump when:
    // - meshoptimizer library is upgraded to a new major version
    // - ProcessMeshes algorithm changes (new passes, different quantization)
    // - VertexQuantized or Meshlet struct layout changes
    // - LOD generation parameters change
    constexpr uint32_t kCookedMeshVersion = 1;

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
        const std::vector<uint32_t>&              allIndices);

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
        std::vector<uint32_t>&               outIndices);

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
        std::vector<uint32_t>& outIndices);

} // namespace SceneCache
