#pragma once

#include <rtxts-ttm/TiledTextureManager.h>

namespace nvfeedback
{
    struct FeedbackTextureTileInfo
    {
        uint32_t m_Mip;
        uint32_t m_XInTexels;
        uint32_t m_YInTexels;
        uint32_t m_WidthInTexels;
        uint32_t m_HeightInTexels;

        bool operator==(const FeedbackTextureTileInfo& b) const
        {
            return m_Mip == b.m_Mip &&
                m_XInTexels == b.m_XInTexels &&
                m_YInTexels == b.m_YInTexels &&
                m_WidthInTexels == b.m_WidthInTexels &&
                m_HeightInTexels == b.m_HeightInTexels;
        }
    };

    // Tiled texture with sampler feedback
    class FeedbackTexture
    {
    public:
        FeedbackTexture(const nvrhi::TextureDesc& desc, rtxts::TiledTextureManager* tiledTextureManager);

        nvrhi::TextureHandle GetReservedTexture()                       { return m_ReservedTexture; }
        nvrhi::SamplerFeedbackTextureHandle GetSamplerFeedbackTexture() { return m_FeedbackTexture; }
        nvrhi::TextureHandle GetMinMipTexture()                         { return m_MinMipTexture; }
        bool IsTilePacked(uint32_t tileIndex) { return tileIndex >= m_PackedMipDesc.startTileIndexInOverallResource; }
        void GetTileInfo(uint32_t tileIndex, std::vector<FeedbackTextureTileInfo>& tiles);

        // Accessors used by FeedbackManager
        nvrhi::BufferHandle GetFeedbackResolveBuffer(uint32_t frameIndex) { return m_FeedbackResolveBuffers[frameIndex]; }
        uint32_t GetNumTiles() const                     { return m_NumTiles; }
        const nvrhi::TileShape& GetTileShape() const     { return m_TileShape; }
        const nvrhi::PackedMipDesc& GetPackedMipInfo() const { return m_PackedMipDesc; }
        uint32_t GetTiledTextureId() const               { return m_TiledTextureId; }

        // CPU-side cache of the last successfully resolved feedback data for this texture.
        // FeedbackManager re-submits this to TTM every frame for textures that are not in
        // the current readback batch, keeping lastRequestedTime fresh and preventing tiles
        // from timing out between ringbuffer cycles.  Empty until the first readback.
        std::vector<uint8_t> m_CachedFeedbackData;

        // User-defined index for O(1) lookup into external data structures (e.g., Scene::m_StreamingTextures)
        int  GetUserIndex() const              { return m_UserIndex; }
        void SetUserIndex(int index)           { m_UserIndex = index; }

        // Index within FeedbackManager::m_Textures — set by FeedbackManager::CreateTexture
        uint32_t GetManagerIndex() const       { return m_ManagerIndex; }
        void     SetManagerIndex(uint32_t idx) { m_ManagerIndex = idx; }

    private:
        nvrhi::TextureHandle m_ReservedTexture;
        nvrhi::SamplerFeedbackTextureHandle m_FeedbackTexture;
        std::vector<nvrhi::BufferHandle> m_FeedbackResolveBuffers;
        nvrhi::TextureHandle m_MinMipTexture;

        uint32_t m_NumTiles = 0;
        nvrhi::PackedMipDesc m_PackedMipDesc{};
        nvrhi::TileShape m_TileShape{};

        uint32_t m_TiledTextureId = 0;
        int m_UserIndex = -1;
        uint32_t m_ManagerIndex = UINT32_MAX;
    };

} // namespace nvfeedback
