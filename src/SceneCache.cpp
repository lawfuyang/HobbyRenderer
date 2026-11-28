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

    struct GroupCounts
    {
        uint32_t m_Triangles = 0;
        uint32_t m_Vertices = 0;
    };

    GroupCounts CountGroup(const GroupSource& source)
    {
        GroupCounts counts;
        for (uint32_t i = 0; i < source.m_Group->m_ClusterCount; ++i)
        {
            const ClusterLod::Cluster& cluster = source.m_Clusters->m_Clusters[source.m_Group->m_ClusterOffset + i];
            counts.m_Triangles += cluster.GetTriangleCount();
            counts.m_Vertices += cluster.GetVertexCount();
        }
        return counts;
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
    const std::vector<srrhi::VertexQuantized>& allVerticesQuantized,
    const std::vector<uint32_t>&              allIndices,
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
    WriteVector(os, allVerticesQuantized);
    WriteVector(os, allIndices);

    // Group blobs: the exact bytes the arena holds, in BuildGpuData's group order. Writing them here (instead of
    // in the per-primitive records) is what lets a load skip them and stream from the mapping later.
    std::vector<GroupSource> groupSources;
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

    // The blob stored for a group is its arena blob with the vertex run meshopt-encoded, so its size is only
    // known once it has been encoded: the table comes from one pass over the groups and the data from a second.
    std::vector<uint32_t> words;
    std::vector<uint8_t> stored;
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

    outBlobs.m_Groups.clear();
    outBlobs.m_Groups.reserve(groupSources.size());
    uint64_t blobBytes = 0;
    uint64_t rawBytes  = 0;
    for (const GroupSource& source : groupSources)
    {
        const GroupCounts counts = CountGroup(source);
        packGroup(source);
        if (!ClusterLod::EncodeGroupBlob(words.data(), counts.m_Triangles, counts.m_Vertices, stored))
        {
            SDL_Log("[SceneCache] Failed to encode a group blob while saving cache: %s", cachePath.string().c_str());
            return false;
        }

        ClusterLod::GroupBlobRef ref;
        ref.m_FileOffset  = blobBytes;
        ref.m_ByteSize    = uint32_t(stored.size());
        ref.m_RawByteSize = ClusterLod::GroupBlobRawBytes(counts.m_Triangles, counts.m_Vertices);
        blobBytes += ref.m_ByteSize;
        rawBytes += ref.m_RawByteSize;
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

    for (size_t i = 0; i < groupSources.size(); ++i)
    {
        const GroupCounts counts = CountGroup(groupSources[i]);
        packGroup(groupSources[i]);
        if (!ClusterLod::EncodeGroupBlob(words.data(), counts.m_Triangles, counts.m_Vertices, stored))
        {
            SDL_Log("[SceneCache] Failed to encode a group blob while saving cache: %s", cachePath.string().c_str());
            return false;
        }
        SDL_assert(stored.size() == outBlobs.m_Groups[i].m_ByteSize);
        os.write(reinterpret_cast<const char*>(stored.data()), std::streamsize(stored.size()));
    }

    outBlobs.m_Path = cachePath;

    if (!os.good())
    {
        SDL_Log("[SceneCache] Write error while saving cache: %s", cachePath.string().c_str());
        return false;
    }

    os.close();

    if (rawBytes != 0)
    {
        SDL_Log("[SceneCache] Group blobs: %.1f MB on disk from %.1f MB of arena data (%.2fx)",
            double(blobBytes) / (1 << 20), double(rawBytes) / (1 << 20), double(rawBytes) / double(blobBytes));
    }

    // Read the section back, decode it and compare it to the packing. This is the check that catches an offset
    // or ordering mistake in the table - at load every such file looks valid. Skipped for very large sections,
    // where the extra read costs more than it is worth.
    if (blobBytes != 0 && blobBytes <= (256ull << 20))
    {
        std::ifstream verify(cachePath, std::ios::binary);
        std::vector<uint32_t> expected;
        std::vector<uint8_t> loaded;
        std::vector<uint8_t> raw;
        for (size_t i = 0; i < groupSources.size() && verify.is_open(); ++i)
        {
            const ClusterLod::GroupBlobRef& ref = outBlobs.m_Groups[i];
            const GroupCounts counts = CountGroup(groupSources[i]);
            packGroup(groupSources[i]);
            expected.assign(ref.m_RawByteSize / sizeof(uint32_t), 0u);
            std::copy(words.begin(), words.end(), expected.begin());

            loaded.assign(ref.m_ByteSize, 0);
            verify.seekg(std::streamoff(ref.m_FileOffset));
            verify.read(reinterpret_cast<char*>(loaded.data()), std::streamsize(loaded.size()));

            raw.assign(ref.m_RawByteSize, 0);
            if (!verify.good() ||
                !ClusterLod::DecodeGroupBlob(loaded.data(), ref.m_ByteSize, counts.m_Triangles, counts.m_Vertices,
                    raw.data()) ||
                memcmp(raw.data(), expected.data(), expected.size() * sizeof(uint32_t)) != 0)
            {
                SDL_Log("[SceneCache] Blob verification failed for group %zu at offset %llu: %s", i,
                    static_cast<unsigned long long>(ref.m_FileOffset), cachePath.string().c_str());
                return false;
            }
        }
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Streamed import: part files
// ─────────────────────────────────────────────────────────────────────────────

std::filesystem::path GetPartPath(const std::filesystem::path& cachePath, uint32_t meshIndex, uint32_t primitiveIndex)
{
    std::filesystem::path dir = cachePath;
    dir += ".parts";
    char name[64];
    std::snprintf(name, sizeof(name), "%06u_%04u.part", meshIndex, primitiveIndex);
    return dir / name;
}

bool WriteCookedPart(const std::filesystem::path& partPath, uint32_t meshIndex, uint32_t primitiveIndex,
                     uint32_t firstTriangle, uint32_t triangleCount,
                     const ClusterLod::PrimitiveClusters& clusters,
                     const std::vector<srrhi::VertexQuantized>& vertices,
                     const srrhi::MeshData& meshData, const float center[3], float radius)
{
    std::error_code ec;
    std::filesystem::create_directories(partPath.parent_path(), ec);

    // Written under a temporary name and renamed, so a part file's existence means "complete". That is the whole
    // resume protocol: a torn part is simply absent and gets redone.
    std::filesystem::path tempPath = partPath;
    tempPath += ".tmp";

    {
        std::ofstream os(tempPath, std::ios::binary | std::ios::trunc);
        if (!os.is_open())
        {
            SDL_Log("[SceneCache] Failed to open part file for writing: %s", tempPath.string().c_str());
            return false;
        }

        CookedPartHeader header;
        header.m_Magic = kCookedPartMagic;
        header.m_Version = kCookedPartVersion;
        header.m_MeshIndex = meshIndex;
        header.m_PrimitiveIndex = primitiveIndex;
        header.m_FirstTriangle = firstTriangle;
        header.m_TriangleCount = triangleCount;
        header.m_VertexCount = uint32_t(vertices.size());
        header.m_IndexCount = meshData.m_IndexCounts[0];
        header.m_LODCount = meshData.m_LODCount;
        for (uint32_t lod = 0; lod < srrhi::CommonConsts::MAX_LOD_COUNT; ++lod)
            header.m_IndexCounts[lod] = meshData.m_IndexCounts[lod];
        header.m_Center[0] = center[0];
        header.m_Center[1] = center[1];
        header.m_Center[2] = center[2];
        header.m_Radius = radius;
        header.m_GroupCount = uint32_t(clusters.m_Groups.size());
        WritePOD(os, header);

        const std::streampos metadataStart = os.tellp();
        ClusterLod::WriteToStream(os, clusters);
        header.m_ClusterBytes = uint64_t(os.tellp() - metadataStart);

        // The blob table, with offsets relative to the blob region that follows it. Each group's blob is the
        // arena layout - its triangles, then six words of VertexQuantized per cluster vertex - with the vertex
        // run meshopt-encoded, so a size is only known once the group has been encoded: one pass sizes the
        // table, the second writes the data.
        std::vector<uint32_t> words;
        std::vector<uint8_t> stored;
        std::vector<GroupCounts> groupCounts(clusters.m_Groups.size());
        auto packGroup = [&clusters, &vertices, &words](const ClusterLod::Group& group)
        {
            words.clear();
            for (uint32_t i = 0; i < group.m_ClusterCount; ++i)
            {
                const ClusterLod::Cluster& cluster = clusters.m_Clusters[group.m_ClusterOffset + i];
                for (uint32_t t = 0; t < cluster.GetTriangleCount(); ++t)
                {
                    const size_t at = cluster.m_TriangleOffset + size_t(t) * 3;
                    words.push_back(uint32_t(clusters.m_Triangles[at]) | (uint32_t(clusters.m_Triangles[at + 1]) << 8) |
                        (uint32_t(clusters.m_Triangles[at + 2]) << 16));
                }
            }
            for (uint32_t i = 0; i < group.m_ClusterCount; ++i)
            {
                const ClusterLod::Cluster& cluster = clusters.m_Clusters[group.m_ClusterOffset + i];
                for (uint32_t v = 0; v < cluster.GetVertexCount(); ++v)
                {
                    const uint32_t vertex = clusters.m_VertexIndices[cluster.m_VertexOffset + v];
                    SDL_assert(vertex < vertices.size());
                    const srrhi::VertexQuantized& vq = vertices[vertex];
                    const uint32_t* attributeWords = reinterpret_cast<const uint32_t*>(&vq);
                    for (uint32_t w = 0; w < 6u; ++w)
                        words.push_back(attributeWords[w]);
                }
            }
        };

        std::vector<ClusterLod::GroupBlobRef> refs(clusters.m_Groups.size());
        uint64_t blobBytes = 0;
        for (size_t g = 0; g < clusters.m_Groups.size(); ++g)
        {
            const ClusterLod::Group& group = clusters.m_Groups[g];
            for (uint32_t i = 0; i < group.m_ClusterCount; ++i)
            {
                const ClusterLod::Cluster& cluster = clusters.m_Clusters[group.m_ClusterOffset + i];
                groupCounts[g].m_Triangles += cluster.GetTriangleCount();
                groupCounts[g].m_Vertices += cluster.GetVertexCount();
            }

            packGroup(group);
            if (!ClusterLod::EncodeGroupBlob(words.data(), groupCounts[g].m_Triangles, groupCounts[g].m_Vertices,
                    stored))
            {
                SDL_Log("[SceneCache] Failed to encode a group blob while saving part: %s", tempPath.string().c_str());
                os.close();
                std::filesystem::remove(tempPath, ec);
                return false;
            }

            refs[g].m_FileOffset  = blobBytes;
            refs[g].m_ByteSize    = uint32_t(stored.size());
            refs[g].m_RawByteSize = ClusterLod::GroupBlobRawBytes(groupCounts[g].m_Triangles,
                groupCounts[g].m_Vertices);
            blobBytes += refs[g].m_ByteSize;
        }
        header.m_BlobBytes = blobBytes;

        WritePOD(os, uint32_t(refs.size()));
        for (const ClusterLod::GroupBlobRef& ref : refs)
            WritePOD(os, ref);

        for (size_t g = 0; g < clusters.m_Groups.size(); ++g)
        {
            packGroup(clusters.m_Groups[g]);
            if (!ClusterLod::EncodeGroupBlob(words.data(), groupCounts[g].m_Triangles, groupCounts[g].m_Vertices,
                    stored))
            {
                SDL_Log("[SceneCache] Failed to encode a group blob while saving part: %s", tempPath.string().c_str());
                os.close();
                std::filesystem::remove(tempPath, ec);
                return false;
            }
            SDL_assert(stored.size() == refs[g].m_ByteSize);
            os.write(reinterpret_cast<const char*>(stored.data()), std::streamsize(stored.size()));
        }

        if (!os.good())
        {
            SDL_Log("[SceneCache] Write error while saving part: %s", tempPath.string().c_str());
            os.close();
            std::filesystem::remove(tempPath, ec);
            return false;
        }

        // The header is only final once the sizes are known.
        os.seekp(0);
        WritePOD(os, header);
        os.close();
    }

    std::filesystem::rename(tempPath, partPath, ec);
    if (ec)
    {
        SDL_Log("[SceneCache] Failed to publish part %s: %s", partPath.string().c_str(), ec.message().c_str());
        std::filesystem::remove(tempPath, ec);
        return false;
    }
    return true;
}

bool ReadCookedPartHeader(const std::filesystem::path& partPath, CookedPartHeader& header)
{
    std::ifstream is(partPath, std::ios::binary);
    if (!is.is_open())
        return false;
    ReadPOD(is, header);
    if (!is.good() || header.m_Magic != kCookedPartMagic || header.m_Version != kCookedPartVersion)
        return false;

    // The resume marker is the file's existence, so a part that is the wrong size (a write that was cut short,
    // or a part from an older layout) must be rejected and redone rather than trusted.
    const uint64_t expected = sizeof(CookedPartHeader) + header.m_ClusterBytes + sizeof(uint32_t) +
        uint64_t(header.m_GroupCount) * sizeof(ClusterLod::GroupBlobRef) + header.m_BlobBytes;
    std::error_code ec;
    const auto actual = std::filesystem::file_size(partPath, ec);
    if (ec || uint64_t(actual) != expected)
    {
        SDL_Log("[SceneCache] Part %s is %llu bytes, its header promises %llu: re-importing it",
            partPath.filename().string().c_str(), static_cast<unsigned long long>(ec ? 0 : actual),
            static_cast<unsigned long long>(expected));
        return false;
    }
    return true;
}

bool ReadCookedPart(const std::filesystem::path& partPath, CookedPartHeader& header,
                    ClusterLod::PrimitiveClusters& clusters)
{
    // Validated through the header reader, so a part whose size does not match its header is rejected here too -
    // that is what makes the resume check and the assemble pass agree on which parts are usable.
    if (!ReadCookedPartHeader(partPath, header))
        return false;

    std::ifstream is(partPath, std::ios::binary);
    if (!is.is_open())
        return false;
    ReadPOD(is, header);
    ClusterLod::ReadFromStream(is, clusters);
    return is.good();
}

bool AssembleCookedMesh(const std::filesystem::path& cachePath, const std::vector<Scene::Mesh>& meshes,
                        const std::vector<srrhi::MeshData>& meshData, const std::filesystem::path& partsDir,
                        ClusterLod::BlobFile& outBlobs)
{
    // Blob order must match BuildGpuData's group order: mesh, primitive, group.
    std::vector<std::filesystem::path> parts;
    std::vector<CookedPartHeader> headers;
    uint64_t blobBytes = 0;
    for (const Scene::Mesh& mesh : meshes)
    {
        for (size_t pi = 0; pi < mesh.m_Primitives.size(); ++pi)
        {
            if (mesh.m_Clusters.empty() || mesh.m_Clusters[pi].IsEmpty())
                continue;
            const uint32_t meshIndex = uint32_t(&mesh - meshes.data());
            const std::filesystem::path partPath = GetPartPath(cachePath, meshIndex, uint32_t(pi));
            CookedPartHeader header;
            if (!ReadCookedPartHeader(partPath, header))
            {
                SDL_Log("[SceneCache] Missing part for mesh %u primitive %zu: %s", meshIndex, pi, partPath.string().c_str());
                return false;
            }
            parts.push_back(partPath);
            headers.push_back(header);
            blobBytes += header.m_BlobBytes;
        }
    }

    // Assembled under a temporary name: a failure must not leave a file that a later run would happily "load".
    std::filesystem::path tempCache = cachePath;
    tempCache += ".tmp";
    std::ofstream os(tempCache, std::ios::binary | std::ios::trunc);
    if (!os.is_open())
    {
        SDL_Log("[SceneCache] Failed to open cache file for writing: %s", tempCache.string().c_str());
        return false;
    }

    WritePOD(os, kCookedMeshMagic);
    WritePOD(os, kCookedMeshVersion);
    WritePOD(os, ClusterLod::GetCacheConfigHash(ClusterLod::GetActiveConfig()));
    WritePOD(os, uint32_t(1));   // clusters cooked

    const uint32_t meshCount = uint32_t(meshes.size());
    WritePOD(os, meshCount);
    for (const Scene::Mesh& mesh : meshes)
    {
        const uint32_t primCount = uint32_t(mesh.m_Primitives.size());
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

        const uint32_t clusterCount = uint32_t(mesh.m_Clusters.size());
        WritePOD(os, clusterCount);
        for (const ClusterLod::PrimitiveClusters& clusters : mesh.m_Clusters)
            ClusterLod::WriteToStream(os, clusters);
    }

    // The vertex/index arrays are absent from a streamed scene; m_MeshData is not - the cluster path and the node
    // bounds read it - so it is written as usual.
    WriteVector(os, meshData);
    WriteVector(os, std::vector<srrhi::VertexQuantized>{});
    WriteVector(os, std::vector<uint32_t>{});

    outBlobs.m_Groups.clear();
    uint64_t offset = 0;
    for (const CookedPartHeader& header : headers)
        for (uint32_t g = 0; g < header.m_GroupCount; ++g)
            outBlobs.m_Groups.push_back(ClusterLod::GroupBlobRef{});

    // Per-part blob tables carry their own sizes; re-read them to fill the global table.
    size_t globalGroup = 0;
    for (size_t p = 0; p < parts.size(); ++p)
    {
        std::ifstream part(parts[p], std::ios::binary);
        CookedPartHeader header{};
        ReadPOD(part, header);
        part.seekg(std::streamoff(sizeof(CookedPartHeader) + int64_t(header.m_ClusterBytes)));
        uint32_t groupCount = 0;
        ReadPOD(part, groupCount);
        SDL_assert(groupCount == header.m_GroupCount);
        for (uint32_t g = 0; g < groupCount; ++g)
        {
            ClusterLod::GroupBlobRef ref{};
            ReadPOD(part, ref);
            ref.m_FileOffset += offset;
            outBlobs.m_Groups[globalGroup++] = ref;
        }
        offset += header.m_BlobBytes;
    }

    const uint64_t tableBytes = sizeof(uint64_t) + uint64_t(outBlobs.m_Groups.size()) * sizeof(ClusterLod::GroupBlobRef);
    const uint64_t sectionStart = (uint64_t(os.tellp()) + tableBytes + 4095u) & ~uint64_t(4095);
    for (ClusterLod::GroupBlobRef& ref : outBlobs.m_Groups)
        ref.m_FileOffset += sectionStart;

    WritePOD(os, uint64_t(outBlobs.m_Groups.size()));
    for (const ClusterLod::GroupBlobRef& ref : outBlobs.m_Groups)
        WritePOD(os, ref);
    WriteZeroBytes(os, sectionStart - uint64_t(os.tellp()));

    // Stream each part's blob region in and delete the part as it goes.
    std::vector<char> buffer;
    for (size_t p = 0; p < parts.size(); ++p)
    {
        const CookedPartHeader& header = headers[p];
        std::ifstream part(parts[p], std::ios::binary);
        part.seekg(std::streamoff(sizeof(CookedPartHeader) + int64_t(header.m_ClusterBytes) + sizeof(uint32_t) +
            int64_t(header.m_GroupCount) * int64_t(sizeof(ClusterLod::GroupBlobRef))));
        buffer.resize(size_t(std::min<uint64_t>(header.m_BlobBytes, 32ull << 20)));
        uint64_t remaining = header.m_BlobBytes;
        while (remaining != 0)
        {
            const size_t chunk = size_t(std::min<uint64_t>(remaining, buffer.size()));
            part.read(buffer.data(), std::streamsize(chunk));
            if (!part.good())
            {
                SDL_Log("[SceneCache] Failed to read part blob data: %s", parts[p].string().c_str());
                return false;
            }
            os.write(buffer.data(), std::streamsize(chunk));
            remaining -= chunk;
        }
        part.close();

        std::error_code ec;
        std::filesystem::remove(parts[p], ec);
    }

    outBlobs.m_Path = cachePath;
    if (!os.good())
    {
        SDL_Log("[SceneCache] Write error while assembling the cooked mesh: %s", tempCache.string().c_str());
        os.close();
        std::error_code removeError;
        std::filesystem::remove(tempCache, removeError);
        return false;
    }
    os.close();

    std::error_code ec;
    std::filesystem::rename(tempCache, cachePath, ec);
    if (ec)
    {
        SDL_Log("[SceneCache] Failed to publish the assembled cache %s: %s", cachePath.string().c_str(),
            ec.message().c_str());
        return false;
    }
    std::filesystem::remove(partsDir, ec);
    SDL_Log("[SceneCache] Assembled cooked mesh from %zu parts: %.2f MB of blobs", parts.size(),
        double(blobBytes) / (1024.0 * 1024.0));
    return true;
}

bool LoadCookedMesh(
    const std::filesystem::path& cachePath,
    std::vector<Scene::Mesh>&            outMeshes,
    std::vector<srrhi::MeshData>&        outMeshData,
    std::vector<srrhi::VertexQuantized>& outVerticesQuantized,
    std::vector<uint32_t>&               outIndices,
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

            outVerticesQuantized,
            outIndices,
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

    // 2. Cache miss — process meshes from glTF. A streamed import needs somewhere for its part files: the cooked
    // cache's own path plus ".parts".
    SceneLoader::SetStreamImportPartsBase(cachePath);
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

    // 3. Save the cooked mesh for next time. A streamed import leaves per-primitive parts behind instead of the
    // scene's geometry, so the assembler streams those in and writes no legacy arrays.
    std::filesystem::path partsDir = cachePath;
    partsDir += ".parts";
    bool saved = false;
    if (Config::Get().m_bStreamImport && std::filesystem::exists(partsDir))
    {
        saved = AssembleCookedMesh(cachePath, scene.m_Meshes, scene.m_MeshData, partsDir, outBlobs);
    }
    else
    {
        saved = SaveCookedMesh(cachePath,
            scene.m_Meshes,
            scene.m_MeshData,
            outVerticesQuantized,
            outIndices,
            outBlobs);
    }
    if (saved)
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
