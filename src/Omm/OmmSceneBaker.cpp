#include "OmmSceneBaker.h"

#include "AlphaTextureReader.h"
#include "OmmBaker.h"
#include "Scene.h"
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

            std::vector<uint8_t> alpha;
            uint32_t alphaWidth = 0;
            uint32_t alphaHeight = 0;
            if (!ReadAlphaMip(scene.m_StreamingTextures[textureIndex], params.m_MipLevel, alpha, alphaWidth, alphaHeight))
            {
                ++skippedUnreadableTexture;
                continue;
            }

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

            std::vector<OmmLodData> lods(lodCount);
            bool bAnyMask = false;

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

                OmmBakeInput bakeInput;
                bakeInput.m_Indices = localIndices.data();
                bakeInput.m_IndexCount = indexCount;
                bakeInput.m_PackedUvs = packedUvs.data();
                bakeInput.m_VertexCount = primitive.m_VertexCount;
                bakeInput.m_Alpha = alpha.data();
                bakeInput.m_AlphaWidth = alphaWidth;
                bakeInput.m_AlphaHeight = alphaHeight;
                bakeInput.m_AlphaCutoff = material.m_GPU.m_AlphaCutoff;

                // The texel classification only feeds the debug view, so only LOD0 (which is what the path tracer
                // and nearby instances use) pays for it.
                const bool bBaked = BakeLod(bakeInput, params, lods[lod], lod == 0 ? &texelStats : nullptr);
                if (!bBaked)
                    continue;

                bAnyMask = true;
                ++bakedLods;
                totalMasks += lods[lod].m_Stats.m_MaskCount;
                totalDataBytes += lods[lod].m_Stats.m_DataBytes;
            }

            if (!bAnyMask)
                continue;

            primitive.m_Omm = std::move(lods);
            ++bakedPrimitives;
        }
    }

    if (texelStats.m_Width > 0)
    {
        const double texels = double(texelStats.m_Width) * double(texelStats.m_Height);
        SDL_Log("[OMM] Bake mip %u classification: %.1f%% opaque, %.1f%% transparent, %.1f%% mixed (%ux%u)",
                params.m_MipLevel, 100.0 * texelStats.m_OpaqueCount / texels,
                100.0 * texelStats.m_TransparentCount / texels, 100.0 * texelStats.m_MixedCount / texels,
                texelStats.m_Width, texelStats.m_Height);
    }

    SDL_Log("[OMM] Baked %zu of %zu masked primitives (%zu LODs, %llu masks, %.1f MiB mask data); "
            "%zu skipped for an unreadable texture",
            bakedPrimitives, maskedPrimitives, bakedLods, static_cast<unsigned long long>(totalMasks),
            double(totalDataBytes) / (1024.0 * 1024.0), skippedUnreadableTexture);

    return bakedPrimitives;
}
