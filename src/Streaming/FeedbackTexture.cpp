#include "FeedbackTexture.h"
#include "FeedbackTextureSet.h"
#include "../Renderer.h"

namespace nvfeedback
{
    FeedbackTexture::FeedbackTexture(
        const nvrhi::TextureDesc& desc,
        rtxts::TiledTextureManager* tiledTextureManager,
        uint32_t numReadbacks)
    {
        nvrhi::IDevice* device = g_Renderer.m_RHI->m_NvrhiDevice;

        // Create reserved (tiled/sparse) texture
        {
            nvrhi::TextureDesc textureDesc = desc;
            textureDesc.isTiled = true;
            textureDesc.initialState = nvrhi::ResourceStates::ShaderResource;
            textureDesc.keepInitialState = true;
            textureDesc.debugName = "Reserved Texture";
            m_ReservedTexture = device->createTexture(textureDesc);
        }

        SDL_assert(m_ReservedTexture && "Failed to create reserved texture");

        // Query tiling info
        m_NumTiles = 0;
        m_PackedMipDesc = {};
        m_TileShape = {};
        uint32_t mipLevels = desc.mipLevels;
        std::array<nvrhi::SubresourceTiling, 16> tilingsInfo{};
        device->getTextureTiling(m_ReservedTexture, &m_NumTiles, &m_PackedMipDesc, &m_TileShape, &mipLevels, tilingsInfo.data());

        SDL_assert(m_NumTiles > 0 && "getTextureTiling() returned zero tiles");

        // Register with TTM
        rtxts::TiledLevelDesc tiledLevelDescs[16]{};
        rtxts::TiledTextureDesc tiledTextureDesc{};
        tiledTextureDesc.textureWidth = desc.width;
        tiledTextureDesc.textureHeight = desc.height;
        tiledTextureDesc.tiledLevelDescs = tiledLevelDescs;
        tiledTextureDesc.regularMipLevelsNum = m_PackedMipDesc.numStandardMips;
        tiledTextureDesc.packedMipLevelsNum = m_PackedMipDesc.numPackedMips;
        tiledTextureDesc.packedTilesNum = m_PackedMipDesc.numTilesForPackedMips;
        tiledTextureDesc.tileWidth = m_TileShape.widthInTexels;
        tiledTextureDesc.tileHeight = m_TileShape.heightInTexels;

        for (uint32_t i = 0; i < tiledTextureDesc.regularMipLevelsNum; ++i)
        {
            tiledLevelDescs[i].widthInTiles = tilingsInfo[i].widthInTiles;
            tiledLevelDescs[i].heightInTiles = tilingsInfo[i].heightInTiles;
        }

        tiledTextureManager->AddTiledTexture(tiledTextureDesc, m_TiledTextureId);

        // Create sampler feedback texture (D3D12 only)
        rtxts::TextureDesc feedbackDesc = tiledTextureManager->GetTextureDesc(m_TiledTextureId, rtxts::eFeedbackTexture);

        nvrhi::SamplerFeedbackTextureDesc samplerFeedbackTextureDesc{};
        samplerFeedbackTextureDesc.samplerFeedbackFormat = nvrhi::SamplerFeedbackFormat::MinMipOpaque;
        samplerFeedbackTextureDesc.samplerFeedbackMipRegionX = feedbackDesc.textureOrMipRegionWidth;
        samplerFeedbackTextureDesc.samplerFeedbackMipRegionY = feedbackDesc.textureOrMipRegionHeight;
        samplerFeedbackTextureDesc.samplerFeedbackMipRegionZ = m_TileShape.depthInTexels;
        samplerFeedbackTextureDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        samplerFeedbackTextureDesc.keepInitialState = true;
        m_FeedbackTexture = device->createSamplerFeedbackTexture(m_ReservedTexture, samplerFeedbackTextureDesc);

        // Create readback (resolve) buffers — one per frame-in-flight
        m_FeedbackResolveBuffers.resize(numReadbacks);
        for (uint32_t i = 0; i < numReadbacks; i++)
        {
            uint32_t feedbackTilesX = (desc.width - 1) / feedbackDesc.textureOrMipRegionWidth + 1;
            uint32_t feedbackTilesY = (desc.height - 1) / feedbackDesc.textureOrMipRegionHeight + 1;

            nvrhi::BufferDesc bufferDesc{};
            bufferDesc.byteSize = feedbackTilesX * feedbackTilesY;
            bufferDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
            bufferDesc.initialState = nvrhi::ResourceStates::ResolveDest;
            bufferDesc.debugName = "Feedback Resolve Buffer";
            m_FeedbackResolveBuffers[i] = device->createBuffer(bufferDesc);
        }

        // Create MinMip texture (R32_FLOAT, dimensions = tile count)
        {
            rtxts::TextureDesc minMipDesc = tiledTextureManager->GetTextureDesc(m_TiledTextureId, rtxts::eMinMipTexture);

            nvrhi::TextureDesc textureDesc{};
            textureDesc.width = minMipDesc.textureOrMipRegionWidth;
            textureDesc.height = minMipDesc.textureOrMipRegionHeight;
            textureDesc.format = nvrhi::Format::R32_FLOAT;
            textureDesc.initialState = nvrhi::ResourceStates::ShaderResource;
            textureDesc.keepInitialState = true;
            textureDesc.debugName = "MinMip Texture";
            m_MinMipTexture = device->createTexture(textureDesc);
        }
    }

    FeedbackTexture::~FeedbackTexture()
    {
        // Remove from all texture sets first
        std::vector<FeedbackTextureSet*> textureSets = m_TextureSets;
        for (FeedbackTextureSet* textureSet : textureSets)
        {
            RemoveFromTextureSet(textureSet);
        }
    }

    bool FeedbackTexture::IsTilePacked(uint32_t tileIndex)
    {
        return tileIndex >= m_PackedMipDesc.startTileIndexInOverallResource;
    }

    void FeedbackTexture::GetTileInfo(uint32_t tileIndex, std::vector<FeedbackTextureTileInfo>& tiles)
    {
        tiles.clear();

        const nvrhi::TextureDesc textureDesc = m_ReservedTexture->getDesc();
        bool bIsBlockCompressed = (textureDesc.format >= nvrhi::Format::BC1_UNORM &&
                                  textureDesc.format <= nvrhi::Format::BC7_UNORM_SRGB);

        if (IsTilePacked(tileIndex))
        {
            // Packed mips: return one entry per packed mip level
            for (uint32_t mip = m_PackedMipDesc.numStandardMips;
                 mip < uint32_t(m_PackedMipDesc.numStandardMips + m_PackedMipDesc.numPackedMips);
                 mip++)
            {
                uint32_t width  = std::max(textureDesc.width  >> mip, 1u);
                uint32_t height = std::max(textureDesc.height >> mip, 1u);

                if (bIsBlockCompressed)
                {
                    width  = ((width  + 3) / 4) * 4;
                    height = ((height + 3) / 4) * 4;
                }

                FeedbackTextureTileInfo tile;
                tile.m_XInTexels = 0;
                tile.m_YInTexels = 0;
                tile.m_Mip = mip;
                tile.m_WidthInTexels = width;
                tile.m_HeightInTexels = height;
                tiles.push_back(tile);
            }
        }
        else
        {
            // Standard tile
            const std::vector<rtxts::TileCoord>& tileCoords = g_Renderer.m_FeedbackManager->GetTiledTextureManager()->GetTileCoordinates(m_TiledTextureId);
            uint32_t tileX = tileCoords[tileIndex].x;
            uint32_t tileY = tileCoords[tileIndex].y;
            uint32_t mip   = tileCoords[tileIndex].mipLevel;

            uint32_t width  = m_TileShape.widthInTexels;
            uint32_t height = m_TileShape.heightInTexels;

            uint32_t subresourceWidth  = std::max(textureDesc.width  >> mip, 1u);
            uint32_t subresourceHeight = std::max(textureDesc.height >> mip, 1u);

            if (bIsBlockCompressed)
            {
                subresourceWidth  = ((subresourceWidth  + 3) / 4) * 4;
                subresourceHeight = ((subresourceHeight + 3) / 4) * 4;
            }

            uint32_t x = tileX * width;
            uint32_t y = tileY * height;

            // Clamp to subresource boundary
            if (x + width  > subresourceWidth)  width  = subresourceWidth  - x;
            if (y + height > subresourceHeight) height = subresourceHeight - y;

            FeedbackTextureTileInfo tile;
            tile.m_XInTexels = x;
            tile.m_YInTexels = y;
            tile.m_Mip = mip;
            tile.m_WidthInTexels = width;
            tile.m_HeightInTexels = height;
            tiles.push_back(tile);
        }
    }

    FeedbackTextureSet* FeedbackTexture::GetTextureSet(uint32_t index) const
    {
        return m_TextureSets[index];
    }

    bool FeedbackTexture::AddToTextureSet(FeedbackTextureSet* textureSet)
    {
        if (!textureSet) return false;
        auto it = std::find(m_TextureSets.begin(), m_TextureSets.end(), textureSet);
        if (it == m_TextureSets.end())
            m_TextureSets.push_back(textureSet);
        UpdateTextureSets();
        return true;
    }

    bool FeedbackTexture::RemoveFromTextureSet(FeedbackTextureSet* textureSet)
    {
        if (!textureSet) return false;
        auto it = std::find(m_TextureSets.begin(), m_TextureSets.end(), textureSet);
        if (it == m_TextureSets.end()) return false;
        m_TextureSets.erase(it);
        UpdateTextureSets();
        return true;
    }

    void FeedbackTexture::UpdateTextureSets()
    {
        m_PrimaryTextureSets.clear();
        for (FeedbackTextureSet* textureSet : m_TextureSets)
        {
            if (textureSet->GetPrimaryTexture() == this)
                m_PrimaryTextureSets.push_back(textureSet);
        }

        // Only include in ring buffer if not in any set, or if we are the primary
        bool bNeedsRingBuffer = m_TextureSets.empty() || IsPrimaryTexture();
        g_Renderer.m_FeedbackManager->UpdateTextureRingBufferState(this, bNeedsRingBuffer);
    }

    bool FeedbackTexture::IsPrimaryTexture() const
    {
        return !m_PrimaryTextureSets.empty();
    }

} // namespace nvfeedback
