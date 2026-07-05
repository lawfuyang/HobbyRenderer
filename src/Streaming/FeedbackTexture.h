#pragma once

#include <rtxts-ttm/TiledTextureManager.h>

namespace nvfeedback
{
    // Forward declaration for circular dependency
    class FeedbackTextureSet;

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
        FeedbackTexture(
            const nvrhi::TextureDesc& desc,
            rtxts::TiledTextureManager* tiledTextureManager,
            uint32_t numReadbacks);
        ~FeedbackTexture();

        nvrhi::TextureHandle GetReservedTexture()                  { return m_ReservedTexture; }
        nvrhi::SamplerFeedbackTextureHandle GetSamplerFeedbackTexture() { return m_FeedbackTexture; }
        nvrhi::TextureHandle GetMinMipTexture()                    { return m_MinMipTexture; }
        bool IsTilePacked(uint32_t tileIndex);
        void GetTileInfo(uint32_t tileIndex, std::vector<FeedbackTextureTileInfo>& tiles);

        uint32_t GetNumTextureSets() const { return (uint32_t)m_TextureSets.size(); }
        class FeedbackTextureSet* GetTextureSet(uint32_t index) const;

        // Accessors used by FeedbackManager
        nvrhi::BufferHandle GetFeedbackResolveBuffer(uint32_t frameIndex) { return m_FeedbackResolveBuffers[frameIndex]; }
        uint32_t GetNumTiles() const                { return m_NumTiles; }
        const nvrhi::TileShape& GetTileShape() const       { return m_TileShape; }
        const nvrhi::PackedMipDesc& GetPackedMipInfo() const { return m_PackedMipDesc; }
        uint32_t GetTiledTextureId() const           { return m_TiledTextureId; }

        // Texture set membership management
        bool AddToTextureSet(FeedbackTextureSet* textureSet);
        bool RemoveFromTextureSet(FeedbackTextureSet* textureSet);
        void UpdateTextureSets();

        bool IsPrimaryTexture() const;
        const std::vector<FeedbackTextureSet*>& GetTextureSets() const        { return m_TextureSets; }
        const std::vector<FeedbackTextureSet*>& GetPrimaryTextureSets() const { return m_PrimaryTextureSets; }

    private:
        nvrhi::TextureHandle m_ReservedTexture;
        nvrhi::SamplerFeedbackTextureHandle m_FeedbackTexture;
        std::vector<nvrhi::BufferHandle> m_FeedbackResolveBuffers;
        nvrhi::TextureHandle m_MinMipTexture;

        uint32_t m_NumTiles = 0;
        nvrhi::PackedMipDesc m_PackedMipDesc{};
        nvrhi::TileShape m_TileShape{};

        uint32_t m_TiledTextureId = 0;

        std::vector<FeedbackTextureSet*> m_TextureSets;
        std::vector<FeedbackTextureSet*> m_PrimaryTextureSets;
    };

} // namespace nvfeedback
