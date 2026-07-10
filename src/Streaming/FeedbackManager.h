#pragma once

#include <deque>
#include <map>

#include "FeedbackTexture.h"
#include "FeedbackTextureSet.h"
#include <rtxts-ttm/TiledTextureManager.h>
#include "../Utilities.h"

namespace nvfeedback
{
    // ─── Public structs ──────────────────────────────────────────────────────

    struct FeedbackManagerStats
    {
        uint64_t m_HeapAllocationInBytes = 0;
        uint32_t m_HeapTilesFree = 0;
        uint32_t m_TilesTotal = 0;
        uint32_t m_TilesAllocated = 0;
        uint32_t m_TilesStandby = 0;

        double m_CpuTimeBeginFrame = 0.0;
        double m_CpuTimeUpdateTileMappings = 0.0;
        double m_CpuTimeResolve = 0.0;
    };

    struct FeedbackUpdateConfig
    {
        uint32_t m_FrameIndex = 0;
        uint32_t m_MaxTexturesToUpdate = 8;
        float m_TileTimeoutSeconds = 0.0f;
        bool m_bDefragmentHeaps = true;
        bool m_bTrimStandbyTiles = true;
        bool m_bReleaseEmptyHeaps = true;
        uint32_t m_NumExtraStandbyTiles = 0;
    };

    struct FeedbackTextureUpdate
    {
        FeedbackTexture* m_Texture = nullptr;
        std::vector<uint32_t> m_TileIndices;
    };

    struct FeedbackTextureCollection
    {
        std::vector<FeedbackTextureUpdate> m_Textures;
    };

    struct FeedbackManagerDesc
    {
        uint32_t m_NumFramesInFlight = 2;
        uint32_t m_HeapSizeInTiles = 256;
    };

    // ─── HeapAllocator ───────────────────────────────────────────────────────
    //
    // Manages D3D12 heap + virtual buffer pairs used as tile pools for tiled
    // resources.  Heaps are released via deferred frame-bucket queues so that
    // GPU command lists still referencing the heap (up to NumFramesInFlight
    // frames ago) are guaranteed to have finished before the heap is destroyed.
    // ─────────────────────────────────────────────────────────────────────────

    class HeapAllocator
    {
    public:
        HeapAllocator(uint64_t heapSizeInBytes, uint32_t framesInFlight);

        uint32_t AllocateHeap();
        void ReleaseHeap(uint32_t heapId, uint32_t frameIndex);
        void DrainReleaseQueue(uint32_t frameIndex);

        nvrhi::HeapHandle   GetHeapHandle(uint32_t heapId)   { return m_Heaps[heapId]; }
        nvrhi::BufferHandle GetBufferHandle(uint32_t heapId) { return m_Buffers[heapId]; }

        uint64_t GetTotalAllocatedBytes() const { return m_TotalAllocatedBytes; }
        uint32_t GetNumHeaps() const { return m_NumHeaps; }

    private:
        std::vector<nvrhi::HeapHandle>   m_Heaps;
        std::vector<nvrhi::BufferHandle> m_Buffers;
        std::vector<uint32_t>            m_FreeHeapIds;

        uint64_t m_HeapSizeInBytes;
        uint32_t m_NumHeaps;
        uint64_t m_TotalAllocatedBytes;
        uint32_t m_FramesInFlight;

        // Deferred release: handles are queued into frame-bucket [frameIndex % FramesInFlight]
        // and only truly destroyed when DrainReleaseQueue is called for the same bucket
        // N frames later (guaranteeing the GPU is done with them).
        std::map<uint32_t, std::vector<nvrhi::HeapHandle>>   m_HeapsToRelease;
        std::map<uint32_t, std::vector<nvrhi::BufferHandle>> m_BuffersToRelease;
    };

    // ─── FeedbackManager ─────────────────────────────────────────────────────

    class FeedbackManager
    {
    public:
        FeedbackManager(const FeedbackManagerDesc& desc);

        FeedbackTexture* CreateTexture(const nvrhi::TextureDesc& desc);
        std::unique_ptr<FeedbackTextureSet> CreateTextureSet();

        void BeginFrame(nvrhi::ICommandList* commandList,
                        const FeedbackUpdateConfig& config,
                        FeedbackTextureCollection* results);

        void UpdateTileMappings(nvrhi::ICommandList* commandList,
                                FeedbackTextureCollection* tilesReady);

        void ResolveFeedback(nvrhi::ICommandList* commandList);
        void EndFrame();

        const FeedbackManagerStats& GetStats() const;

        // Internal helpers called by FeedbackTexture
        void UnregisterTexture(FeedbackTexture* pTex);
        void UpdateTextureRingBufferState(FeedbackTexture* pTex, bool bIncludeInRingBuffer);

        rtxts::TiledTextureManager* GetTiledTextureManager() { return m_TiledTextureManager.get(); }

    private:
        FeedbackManagerDesc m_Desc;
        FeedbackUpdateConfig m_UpdateConfigThisFrame;

        uint32_t m_NumFramesInFlight;
        uint32_t m_FrameIndex;

        std::vector<std::unique_ptr<FeedbackTexture>> m_Textures;
        std::deque<FeedbackTexture*>               m_TexturesRingbuffer;
        std::vector<std::vector<FeedbackTexture*>> m_TexturesToReadback;

        FeedbackManagerStats m_StatsLastFrame{};

        double m_BeginFrameCPUTime = 0.0;
        double m_UpdateTileMappingsCPUTime = 0.0;
        double m_ResolveCPUTime = 0.0;

        std::unique_ptr<HeapAllocator>              m_HeapAllocator;
        std::unique_ptr<rtxts::TiledTextureManager> m_TiledTextureManager;
        std::unordered_set<FeedbackTexture*>        m_MinMipDirtyTextures;
    };

} // namespace nvfeedback
