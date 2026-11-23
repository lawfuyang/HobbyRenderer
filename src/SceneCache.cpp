#include "SceneCache.h"
#include "SceneLoader.h"

namespace SceneCache
{

namespace
{
    // One group's placement in the blob section, in the order BuildGpuData flattens groups (mesh, primitive,
    // group). Sizes are computed from the cluster counts, so the table can be written before the data.
    struct GroupSource
    {
        const ClusterLod::PrimitiveClusters*       m_Clusters = nullptr;
        const ClusterLod::Group*                   m_Group    = nullptr;
        const std::vector<srrhi::VertexQuantized>* m_Vertices = nullptr;  // the scene's packed vertices
        uint32_t                                   m_VertexBase = 0;      // primitive's first scene-wide vertex
    };

    // The arena blob of one group: its clusters' packed triangle triples, then every cluster vertex's attributes
    // as six words (a VertexQuantized), padded to the 16-byte alignment a group address promises. Vertex indices
    // are cluster-local, so the attribute run is dense and no indirection is stored. Must stay in step with
    // ClusterLod::StreamGroup::GetBlobElements and BuildGpuData's group-local offsets.
    constexpr uint32_t kAttributeWords = sizeof(srrhi::VertexQuantized) / sizeof(uint32_t);
    static_assert(sizeof(srrhi::VertexQuantized) == 6 * sizeof(uint32_t));

    uint32_t GroupBlobByteSize(const GroupSource& source)
    {
        uint32_t words = 0;
        for (uint32_t i = 0; i < source.m_Group->m_ClusterCount; ++i)
        {
            const ClusterLod::Cluster& cluster = source.m_Clusters->m_Clusters[source.m_Group->m_ClusterOffset + i];
            words += cluster.GetTriangleCount() + kAttributeWords * cluster.GetVertexCount();
        }
        return (words * uint32_t(sizeof(uint32_t)) + 15u) & ~15u;
    }

    void WriteZeroBytes(std::ostream& os, uint64_t count)
    {
        static const char kZeros[256] = {};
        while (count != 0)
        {
            const size_t chunk = size_t(std::min<uint64_t>(count, sizeof(kZeros)));
            os.write(kZeros, chunk);
            count -= chunk;
        }
    }
}

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
    const std::vector<uint32_t>&              allIndices,
    bool                                      bCookClusters,
    ClusterLod::BlobFile&                     outBlobs)
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
    WritePOD(os, ClusterLod::GetCacheConfigHash(ClusterLod::GetActiveConfig()));
    WritePOD(os, uint32_t(bCookClusters ? 1 : 0));

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

        const uint32_t clusterCount = static_cast<uint32_t>(mesh.m_Clusters.size());
        WritePOD(os, clusterCount);
        for (const ClusterLod::PrimitiveClusters& clusters : mesh.m_Clusters)
            ClusterLod::WriteToStream(os, clusters);
    }

    // POD arrays
    WriteVector(os, meshData);
    WriteVector(os, meshlets);
    WriteVector(os, meshletVertices);
    WriteVector(os, meshletTriangles);
    WriteVector(os, allVerticesQuantized);
    WriteVector(os, allIndices);

    // Group blobs: the exact bytes the arena holds, in BuildGpuData's group order. Writing them here (instead of
    // in the per-primitive records) is what lets a load skip them and stream from the mapping later.
    std::vector<GroupSource> groupSources;
    if (bCookClusters)
    {
        for (const Scene::Mesh& mesh : meshes)
        {
            SDL_assert(mesh.m_Clusters.size() == mesh.m_Primitives.size());
            if (mesh.m_Clusters.size() != mesh.m_Primitives.size())
                continue;

            for (size_t pi = 0; pi < mesh.m_Primitives.size(); ++pi)
            {
                const ClusterLod::PrimitiveClusters& clusters = mesh.m_Clusters[pi];
                if (clusters.IsEmpty())
                    continue;
                if (mesh.m_Primitives[pi].m_MeshDataIndex >= meshData.size())
                    continue;

                SDL_assert(clusters.m_Clusters.size() == clusters.m_GeneratingGroups.size());
                for (const ClusterLod::Group& group : clusters.m_Groups)
                    groupSources.push_back(GroupSource{ &clusters, &group, &allVerticesQuantized,
                        mesh.m_Primitives[pi].m_VertexOffset });
            }
        }
    }

    outBlobs.m_Groups.clear();
    outBlobs.m_Groups.reserve(groupSources.size());
    uint64_t blobBytes = 0;
    for (const GroupSource& source : groupSources)
    {
        ClusterLod::GroupBlobRef ref;
        ref.m_FileOffset = blobBytes;
        ref.m_ByteSize   = GroupBlobByteSize(source);
        blobBytes += ref.m_ByteSize;
        outBlobs.m_Groups.push_back(ref);
    }

    const uint64_t tableBytes   = sizeof(uint64_t) + uint64_t(outBlobs.m_Groups.size()) * sizeof(ClusterLod::GroupBlobRef);
    const uint64_t sectionStart = (uint64_t(os.tellp()) + tableBytes + 4095u) & ~uint64_t(4095);
    for (ClusterLod::GroupBlobRef& ref : outBlobs.m_Groups)
        ref.m_FileOffset += sectionStart;   // absolute from now on, which is what the reader hands out

    WritePOD(os, uint64_t(outBlobs.m_Groups.size()));
    for (const ClusterLod::GroupBlobRef& ref : outBlobs.m_Groups)
        WritePOD(os, ref);
    WriteZeroBytes(os, sectionStart - uint64_t(os.tellp()));

    // The blob words themselves. A group's triangles come first, then every cluster vertex's attributes - the
    // same VertexQuantized the scene's global vertex buffer holds, so the cluster path renders identical pixels
    // while only the cut's vertices ever reach VRAM.
    std::vector<uint32_t> words;
    auto packGroup = [&words](const GroupSource& source)
    {
        words.clear();

        for (uint32_t g = 0; g < source.m_Group->m_ClusterCount; ++g)
        {
            const ClusterLod::Cluster& cluster = source.m_Clusters->m_Clusters[source.m_Group->m_ClusterOffset + g];
            for (uint32_t t = 0; t < cluster.GetTriangleCount(); ++t)
            {
                const size_t at = cluster.m_TriangleOffset + size_t(t) * 3;
                words.push_back(uint32_t(source.m_Clusters->m_Triangles[at]) |
                    (uint32_t(source.m_Clusters->m_Triangles[at + 1]) << 8) |
                    (uint32_t(source.m_Clusters->m_Triangles[at + 2]) << 16));
            }
        }

        for (uint32_t g = 0; g < source.m_Group->m_ClusterCount; ++g)
        {
            const ClusterLod::Cluster& cluster = source.m_Clusters->m_Clusters[source.m_Group->m_ClusterOffset + g];
            for (uint32_t v = 0; v < cluster.GetVertexCount(); ++v)
            {
                const uint32_t vertex = source.m_VertexBase + source.m_Clusters->m_VertexIndices[cluster.m_VertexOffset + v];
                SDL_assert(vertex < source.m_Vertices->size());
                const srrhi::VertexQuantized& vq = (*source.m_Vertices)[vertex];
                const uint32_t* attributeWords = reinterpret_cast<const uint32_t*>(&vq);
                for (uint32_t w = 0; w < kAttributeWords; ++w)
                    words.push_back(attributeWords[w]);
            }
        }
    };

    for (size_t i = 0; i < groupSources.size(); ++i)
    {
        const ClusterLod::GroupBlobRef& ref = outBlobs.m_Groups[i];
        packGroup(groupSources[i]);
        SDL_assert(words.size() * sizeof(uint32_t) <= ref.m_ByteSize);
        os.write(reinterpret_cast<const char*>(words.data()), std::streamsize(words.size() * sizeof(uint32_t)));
        WriteZeroBytes(os, ref.m_ByteSize - words.size() * sizeof(uint32_t));
    }

    outBlobs.m_Path = cachePath;

    if (!os.good())
    {
        SDL_Log("[SceneCache] Write error while saving cache: %s", cachePath.string().c_str());
        return false;
    }

    os.close();

    // Read the section back and compare it to the packing. This is the check that catches an offset or ordering
    // mistake in the table - at load every such file looks valid. Skipped for very large sections, where the extra
    // read costs more than it is worth.
    if (blobBytes != 0 && blobBytes <= (256ull << 20))
    {
        std::ifstream verify(cachePath, std::ios::binary);
        std::vector<uint32_t> expected;
        std::vector<uint32_t> actual;
        for (size_t i = 0; i < groupSources.size() && verify.is_open(); ++i)
        {
            const ClusterLod::GroupBlobRef& ref = outBlobs.m_Groups[i];
            packGroup(groupSources[i]);
            expected.assign(ref.m_ByteSize / sizeof(uint32_t), 0u);
            std::copy(words.begin(), words.end(), expected.begin());
            actual.assign(expected.size(), 0u);

            verify.seekg(std::streamoff(ref.m_FileOffset));
            verify.read(reinterpret_cast<char*>(actual.data()), std::streamsize(actual.size() * sizeof(uint32_t)));
            if (!verify.good() || memcmp(actual.data(), expected.data(), actual.size() * sizeof(uint32_t)) != 0)
            {
                SDL_Log("[SceneCache] Blob verification failed for group %zu at offset %llu: %s", i,
                    static_cast<unsigned long long>(ref.m_FileOffset), cachePath.string().c_str());
                return false;
            }
        }
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
    std::vector<uint32_t>&               outIndices,
    bool                                 bCookClusters,
    ClusterLod::BlobFile&                outBlobs)
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

    // The clusterisation parameters are part of the cache identity: a version number cannot express them.
    uint64_t configHash = 0;
    ReadPOD(is, configHash);
    const uint64_t expectedConfigHash = ClusterLod::GetCacheConfigHash(ClusterLod::GetActiveConfig());
    if (configHash != expectedConfigHash)
    {
        SDL_Log("[SceneCache] Cache cluster config mismatch (file=%llx, expected=%llx): %s",
            static_cast<unsigned long long>(configHash), static_cast<unsigned long long>(expectedConfigHash),
            cachePath.string().c_str());
        return false;
    }

    uint32_t clustersCooked = 0;
    ReadPOD(is, clustersCooked);
    if ((clustersCooked != 0) != bCookClusters)
    {
        SDL_Log("[SceneCache] Cache was cooked with cluster LOD %s, this run wants %s: %s",
            clustersCooked ? "on" : "off", bCookClusters ? "on" : "off", cachePath.string().c_str());
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

        uint32_t clusterCount = 0;
        ReadPOD(is, clusterCount);
        mesh.m_Clusters.resize(clusterCount);
        for (ClusterLod::PrimitiveClusters& clusters : mesh.m_Clusters)
            ClusterLod::ReadFromStream(is, clusters);
    }

    // POD arrays
    ReadVector(is, outMeshData);
    ReadVector(is, outMeshlets);
    ReadVector(is, outMeshletVertices);
    ReadVector(is, outMeshletTriangles);
    ReadVector(is, outVerticesQuantized);
    ReadVector(is, outIndices);

    // The group blob table. The blob bytes after it are not read at all: the renderer maps the file and copies
    // each group straight into the arena.
    outBlobs.m_Groups.clear();
    uint64_t groupCount = 0;
    ReadPOD(is, groupCount);
    if (groupCount > (uint64_t(1) << 28))
    {
        SDL_Log("[SceneCache] Cache group blob table is implausible (%llu groups): %s",
            static_cast<unsigned long long>(groupCount), cachePath.string().c_str());
        return false;
    }
    outBlobs.m_Groups.resize(size_t(groupCount));
    for (ClusterLod::GroupBlobRef& ref : outBlobs.m_Groups)
        ReadPOD(is, ref);
    outBlobs.m_Path = cachePath;

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
    std::vector<uint32_t>& outIndices,
    bool bCookClusters,
    ClusterLod::BlobFile& outBlobs)
{
    outBlobs.Clear();
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
            outIndices,
            bCookClusters,
            outBlobs);
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
        scenePath.string(), scene, outVerticesQuantized, outIndices, bCookClusters);
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
        outIndices,
        bCookClusters,
        outBlobs))
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
