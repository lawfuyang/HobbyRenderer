#include "SceneCache.h"
#include "SceneLoader.h"

namespace SceneCache
{

bool IsCacheValid(const std::filesystem::path& cachePath,
                  const std::filesystem::path& sourcePath)
{
    std::error_code ec;
    if (!std::filesystem::exists(cachePath, ec))
        return false;

    const auto cacheTime  = std::filesystem::last_write_time(cachePath, ec);
    if (ec) return false;
    const auto sourceTime = std::filesystem::last_write_time(sourcePath, ec);
    if (ec) return false;

    return cacheTime >= sourceTime;
}

bool SaveCookedMesh(
    const std::filesystem::path& cachePath,
    const std::vector<Scene::Mesh>&           meshes,
    const std::vector<srrhi::MeshData>&       meshData,
    const std::vector<srrhi::Meshlet>&        meshlets,
    const std::vector<uint32_t>&              meshletVertices,
    const std::vector<uint32_t>&              meshletTriangles,
    const std::vector<srrhi::VertexQuantized>& allVerticesQuantized,
    const std::vector<uint32_t>&              allIndices)
{
    std::ofstream os(cachePath, std::ios::binary | std::ios::trunc);
    if (!os.is_open())
    {
        SDL_Log("[SceneCache] Failed to open cache file for writing: %s", cachePath.string().c_str());
        return false;
    }

    // Header
    WritePOD(os, kCookedMeshMagic);
    WritePOD(os, kCookedMeshVersion);

    // Meshes — serialize field-by-field (Scene::Mesh is not trivially copyable)
    const uint32_t meshCount = static_cast<uint32_t>(meshes.size());
    WritePOD(os, meshCount);
    for (const Scene::Mesh& mesh : meshes)
    {
        const uint32_t primCount = static_cast<uint32_t>(mesh.m_Primitives.size());
        WritePOD(os, primCount);
        for (const Scene::Primitive& prim : mesh.m_Primitives)
        {
            WritePOD(os, prim.m_VertexOffset);
            WritePOD(os, prim.m_VertexCount);
            WritePOD(os, prim.m_MaterialIndex);
            WritePOD(os, prim.m_MeshDataIndex);
        }
        WritePOD(os, mesh.m_Center);
        WritePOD(os, mesh.m_Radius);
    }

    // POD arrays
    WriteVector(os, meshData);
    WriteVector(os, meshlets);
    WriteVector(os, meshletVertices);
    WriteVector(os, meshletTriangles);
    WriteVector(os, allVerticesQuantized);
    WriteVector(os, allIndices);

    if (!os.good())
    {
        SDL_Log("[SceneCache] Write error while saving cache: %s", cachePath.string().c_str());
        return false;
    }

    return true;
}

bool LoadCookedMesh(
    const std::filesystem::path& cachePath,
    std::vector<Scene::Mesh>&            outMeshes,
    std::vector<srrhi::MeshData>&        outMeshData,
    std::vector<srrhi::Meshlet>&         outMeshlets,
    std::vector<uint32_t>&               outMeshletVertices,
    std::vector<uint32_t>&               outMeshletTriangles,
    std::vector<srrhi::VertexQuantized>& outVerticesQuantized,
    std::vector<uint32_t>&               outIndices)
{
    std::ifstream is(cachePath, std::ios::binary);
    if (!is.is_open())
        return false;

    // Validate header
    uint32_t magic = 0;
    ReadPOD(is, magic);
    if (magic != kCookedMeshMagic)
    {
        SDL_Log("[SceneCache] Cache magic mismatch in: %s", cachePath.string().c_str());
        return false;
    }

    uint32_t version = 0;
    ReadPOD(is, version);
    if (version != kCookedMeshVersion)
    {
        SDL_Log("[SceneCache] Cache version mismatch (file=%u, expected=%u): %s",
            version, kCookedMeshVersion, cachePath.string().c_str());
        return false;
    }

    // Meshes
    uint32_t meshCount = 0;
    ReadPOD(is, meshCount);
    outMeshes.resize(meshCount);
    for (Scene::Mesh& mesh : outMeshes)
    {
        uint32_t primCount = 0;
        ReadPOD(is, primCount);
        mesh.m_Primitives.resize(primCount);
        for (Scene::Primitive& prim : mesh.m_Primitives)
        {
            ReadPOD(is, prim.m_VertexOffset);
            ReadPOD(is, prim.m_VertexCount);
            ReadPOD(is, prim.m_MaterialIndex);
            ReadPOD(is, prim.m_MeshDataIndex);
            // m_BLAS is GPU-allocated and not persisted; it will be rebuilt in BuildAccelerationStructures()
        }
        ReadPOD(is, mesh.m_Center);
        ReadPOD(is, mesh.m_Radius);
    }

    // POD arrays
    ReadVector(is, outMeshData);
    ReadVector(is, outMeshlets);
    ReadVector(is, outMeshletVertices);
    ReadVector(is, outMeshletTriangles);
    ReadVector(is, outVerticesQuantized);
    ReadVector(is, outIndices);

    if (!is.good() && !is.eof())
    {
        SDL_Log("[SceneCache] Read error while loading cache: %s", cachePath.string().c_str());
        return false;
    }

    return true;
}

bool LoadOrCookMeshData(
    const std::filesystem::path& scenePath,
    Scene& scene,
    std::vector<srrhi::VertexQuantized>& outVerticesQuantized,
    std::vector<uint32_t>& outIndices)
{
    // Compute cache path: <scene_stem>_mesh.bin alongside the glTF
    const std::filesystem::path cachePath = scenePath.parent_path() / (scenePath.stem().string() + "_mesh.bin");

    // 1. Try loading from cache
    if (IsCacheValid(cachePath, scenePath))
    {
        bool loaded = LoadCookedMesh(cachePath,
            scene.m_Meshes,
            scene.m_MeshData,
            scene.m_Meshlets,
            scene.m_MeshletVertices,
            scene.m_MeshletTriangles,
            outVerticesQuantized,
            outIndices);
        if (loaded)
        {
            SDL_Log("[SceneCache] Loaded cooked mesh from cache: %s", cachePath.string().c_str());

            // Recompute node bounding spheres now that mesh data exists.
            // ProcessNodesAndHierarchy was called earlier (before meshes were available),
            // so node bounding spheres were left uninitialized.
            for (int ni = 0; ni < (int)scene.m_Nodes.size(); ++ni)
            {
                scene.UpdateNodeBoundingSphere(ni);
            }

            return true;
        }
        SDL_Log("[SceneCache] Cache file exists but failed to load, falling back to glTF...");
    }

    // 2. Cache miss — process meshes from glTF
    bool success = SceneLoader::ProcessMeshesFromGLTF(
        scenePath.string(), scene, outVerticesQuantized, outIndices);
    if (!success)
    {
        SDL_Log("[SceneCache] Failed to process meshes from glTF");
        return false;
    }

    // Recompute node bounding spheres now that mesh data is available.
    for (int ni = 0; ni < (int)scene.m_Nodes.size(); ++ni)
    {
        scene.UpdateNodeBoundingSphere(ni);
    }

    // 3. Save the cooked mesh for next time
    if (SaveCookedMesh(cachePath,
        scene.m_Meshes,
        scene.m_MeshData,
        scene.m_Meshlets,
        scene.m_MeshletVertices,
        scene.m_MeshletTriangles,
        outVerticesQuantized,
        outIndices))
    {
        SDL_Log("[SceneCache] Saved cooked mesh to cache: %s", cachePath.string().c_str());
    }
    else
    {
        SDL_Log("[SceneCache] Warning: failed to save cooked mesh cache");
    }

    return true;
}

} // namespace SceneCache
