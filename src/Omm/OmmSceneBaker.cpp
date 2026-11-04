#include "OmmSceneBaker.h"

#include "AlphaTextureReader.h"
#include "OmmBaker.h"
#include "Scene.h"
#include "Renderer.h"
#include "Utilities.h"

namespace
{
    // Decodes the alpha of one streamed texture mip. Returns false when the texture is not readable from the
    // CPU, which is the normal case for anything that is not a streamed block-compressed texture.
    bool ReadAlphaMip(const Scene::StreamingTexture& texture, uint32_t mip, std::vector<uint8_t>& alpha,
                      uint32_t& width, uint32_t& height)
    {
        if (!texture.m_ReservedTexture || !texture.m_SourceData || !texture.m_SourceData->IsValid())
            return false;

        const nvrhi::TextureDesc& desc = texture.m_ReservedTexture->getDesc();
        width = std::max(1u, desc.width >> mip);
        height = std::max(1u, desc.height >> mip);

        OmmAlphaMip mipData;
        mipData.m_Data = static_cast<const uint8_t*>(texture.m_SourceData->GetDataAt(texture.m_MipDataOffsets[mip]));
        mipData.m_Width = width;
        mipData.m_Height = height;
        mipData.m_Size = GetOmmMipByteSize(desc.format, width, height);

        return DecodeAlphaMip(mipData, desc.format, alpha);
    }
}

size_t BakeSceneOmm(Scene& scene, const std::vector<srrhi::VertexQuantized>& vertices,
                    const std::vector<uint32_t>& indices, const OmmBakeParams& params)
{
    SCOPED_TIMER("[OMM] BakeSceneOmm");

    SDL_assert(vertices.size() > 0);

    size_t bakedPrimitives = 0;
    size_t bakedLods = 0;
    size_t maskedPrimitives = 0;
    size_t skippedUnreadableTexture = 0;
    uint64_t totalMasks = 0;
    uint64_t totalDataBytes = 0;
    OmmDebugTexelStats texelStats;

    // Decoding a mip is the expensive part of the bake and many primitives share one material, so each texture
    // is decoded once rather than once per primitive.
    struct CachedAlpha
    {
        std::vector<uint8_t> m_Bytes;
        uint32_t m_Width = 0;
        uint32_t m_Height = 0;
        bool m_bValid = false;
    };
    std::unordered_map<int, CachedAlpha> alphaCache;

    // Each entry is one (primitive, LOD) bake. Gathering the inputs serially and baking in parallel keeps the
    // scene containers single-threaded while the expensive part spreads over the engine's worker threads.
    struct BakeJob
    {
        OmmLodData* m_Target = nullptr;
        const std::vector<uint8_t>* m_Alpha = nullptr;
        uint32_t m_AlphaWidth = 0;
        uint32_t m_AlphaHeight = 0;
        float m_AlphaCutoff = 0.5f;
        std::vector<uint32_t> m_PackedUvs;
        std::vector<uint32_t> m_LocalIndices;
        bool m_bClassify = false;
    };
    std::vector<BakeJob> jobs;
    // The classification fills one shared snapshot, so exactly one job may produce it: two jobs classifying at the
    // same time would reallocate the same texel array from two threads.
    bool bClassifyTaken = false;

    const uint64_t bakeStartMs = SDL_GetTicks();
    uint64_t decodeMs = 0;

    for (Scene::Mesh& mesh : scene.m_Meshes)
    {
        for (Scene::Primitive& primitive : mesh.m_Primitives)
        {
            primitive.m_Omm.clear();

            if (primitive.m_MaterialIndex < 0 || size_t(primitive.m_MaterialIndex) >= scene.m_Materials.size())
                continue;

            const Scene::Material& material = scene.m_Materials[primitive.m_MaterialIndex];
            if (material.m_GPU.m_AlphaMode != uint32_t(srrhi::CommonConsts::ALPHA_MODE_MASK))
                continue;

            ++maskedPrimitives;

            const int textureIndex = material.m_BaseColorTexture;
            if (textureIndex < 0 || size_t(textureIndex) >= scene.m_StreamingTextures.size())
            {
                ++skippedUnreadableTexture;
                continue;
            }

            std::unordered_map<int, CachedAlpha>::iterator cached = alphaCache.find(textureIndex);
            if (cached == alphaCache.end())
            {
                const uint64_t decodeStartMs = SDL_GetTicks();
                CachedAlpha entry;
                entry.m_bValid = ReadAlphaMip(scene.m_StreamingTextures[textureIndex], params.m_MipLevel, entry.m_Bytes,
                                              entry.m_Width, entry.m_Height);
                decodeMs += SDL_GetTicks() - decodeStartMs;
                cached = alphaCache.emplace(textureIndex, std::move(entry)).first;
            }

            if (!cached->second.m_bValid)
            {
                ++skippedUnreadableTexture;
                continue;
            }

            const std::vector<uint8_t>& alpha = cached->second.m_Bytes;
            const uint32_t alphaWidth = cached->second.m_Width;
            const uint32_t alphaHeight = cached->second.m_Height;

            if (primitive.m_MeshDataIndex >= scene.m_MeshData.size())
                continue;

            const srrhi::MeshData& meshData = scene.m_MeshData[primitive.m_MeshDataIndex];
            const uint32_t lodCount = std::min<uint32_t>(meshData.m_LODCount, srrhi::CommonConsts::MAX_LOD_COUNT);

            // The bake indexes its own vertex block, so the UVs are gathered once per primitive and the scene's
            // globally rebased index slices are rewritten relative to it.
            std::vector<uint32_t> packedUvs(primitive.m_VertexCount);
            for (uint32_t v = 0; v < primitive.m_VertexCount; ++v)
            {
                SDL_assert(size_t(primitive.m_VertexOffset) + v < vertices.size());
                packedUvs[v] = vertices[size_t(primitive.m_VertexOffset) + v].m_Uv;
            }

            primitive.m_Omm.assign(lodCount, OmmLodData{});

            for (uint32_t lod = 0; lod < lodCount; ++lod)
            {
                const uint32_t indexOffset = meshData.m_IndexOffsets[lod];
                const uint32_t indexCount = meshData.m_IndexCounts[lod];
                if (indexCount == 0 || size_t(indexOffset) + indexCount > indices.size())
                    continue;

                std::vector<uint32_t> localIndices(indexCount);
                for (uint32_t i = 0; i < indexCount; ++i)
                {
                    const uint32_t globalIndex = indices[size_t(indexOffset) + i];
                    SDL_assert(globalIndex >= primitive.m_VertexOffset);
                    localIndices[i] = globalIndex - primitive.m_VertexOffset;
                }

                BakeJob job;
                job.m_Target = &primitive.m_Omm[lod];
                job.m_Alpha = &alpha;
                job.m_AlphaWidth = alphaWidth;
                job.m_AlphaHeight = alphaHeight;
                job.m_AlphaCutoff = material.m_GPU.m_AlphaCutoff;
                job.m_PackedUvs = packedUvs;
                job.m_LocalIndices = std::move(localIndices);
                job.m_bClassify = lod == 0 && !bClassifyTaken;
                bClassifyTaken = bClassifyTaken || job.m_bClassify;
                jobs.push_back(std::move(job));
            }
        }
    }

    if (!jobs.empty())
    {
        auto runJob = [&](uint32_t jobIndex, uint32_t)
        {
            BakeJob& job = jobs[jobIndex];

            OmmBakeInput bakeInput;
            bakeInput.m_Indices = job.m_LocalIndices.data();
            bakeInput.m_IndexCount = job.m_LocalIndices.size();
            bakeInput.m_PackedUvs = job.m_PackedUvs.data();
            bakeInput.m_VertexCount = job.m_PackedUvs.size();
            bakeInput.m_Alpha = job.m_Alpha->data();
            bakeInput.m_AlphaWidth = job.m_AlphaWidth;
            bakeInput.m_AlphaHeight = job.m_AlphaHeight;
            bakeInput.m_AlphaCutoff = job.m_AlphaCutoff;

            // Exactly one job classifies its texels, so the shared stats below are written from one thread only.
            BakeLod(bakeInput, params, *job.m_Target, job.m_bClassify ? &texelStats : nullptr);
        };

        const uint32_t jobCount = static_cast<uint32_t>(jobs.size());
        SDL_Log("[OMM] Baking %u LOD jobs on %u worker threads", jobCount,
                g_Renderer.m_TaskScheduler->GetThreadCount());
        g_Renderer.m_TaskScheduler->ParallelFor(jobCount, runJob);
    }

    // A primitive counts as baked once any of its LODs produced masks; the rest keep an empty mask list.
    // The digest is of everything the bake produced, so two runs can be compared byte for byte, which is what the
    // cache in a later phase relies on.
    size_t digest = 0;
    for (Scene::Mesh& mesh : scene.m_Meshes)
    {
        for (Scene::Primitive& primitive : mesh.m_Primitives)
        {
            bool bAnyMask = false;
            for (OmmLodData& lod : primitive.m_Omm)
            {
                if (lod.m_Data.empty())
                    continue;

                bAnyMask = true;
                ++bakedLods;
                totalMasks += lod.m_Stats.m_MaskCount;
                totalDataBytes += lod.m_Stats.m_DataBytes;

                digest = HashBytes(digest, lod.m_Data.data(), lod.m_Data.size());
                if (lod.m_Stats.m_bSixteenBitIndices)
                    digest = HashBytes(digest, lod.m_Indices16.data(), lod.m_Indices16.size() * sizeof(uint16_t));
                else
                    digest = HashBytes(digest, lod.m_Indices.data(), lod.m_Indices.size() * sizeof(uint32_t));
            }

            if (bAnyMask)
                ++bakedPrimitives;
            else
                primitive.m_Omm.clear();
        }
    }

    if (texelStats.m_Width > 0)
    {
        const double texels = double(texelStats.m_Width) * double(texelStats.m_Height);
        SDL_Log("[OMM] Bake mip %u classification of the first baked primitive: %.1f%% opaque, %.1f%% transparent, "
                "%.1f%% mixed (%ux%u)",
                params.m_MipLevel, 100.0 * texelStats.m_OpaqueCount / texels,
                100.0 * texelStats.m_TransparentCount / texels, 100.0 * texelStats.m_MixedCount / texels,
                texelStats.m_Width, texelStats.m_Height);
    }

    SDL_Log("[OMM] Baked %zu of %zu masked primitives (%zu LODs, %llu masks, %.1f MiB mask data) in %.2f s "
            "(alpha decode %.2f s); %zu skipped for an unreadable texture; digest %016zx",
            bakedPrimitives, maskedPrimitives, bakedLods, static_cast<unsigned long long>(totalMasks),
            double(totalDataBytes) / (1024.0 * 1024.0), double(SDL_GetTicks() - bakeStartMs) / 1000.0,
            double(decodeMs) / 1000.0, skippedUnreadableTexture, digest);

    return bakedPrimitives;
}
