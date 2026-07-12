#pragma once

#include <unordered_set>

#include "FeedbackTexture.h"
#include "FeedbackTextureSet.h"
#include <rtxts-ttm/TiledTextureManager.h>
#include "../Utilities.h"

// D3D12 tile size constant (64 KB)
#ifndef D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES
#define D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES 65536u
#endif

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

    struct FeedbackTextureUpdate
    {
        FeedbackTexture* m_Texture = nullptr;
        std::vector<uint32_t> m_TileIndices;
    };

    struct FeedbackTextureCollection
    {
        std::vector<FeedbackTextureUpdate> m_Textures;
    };

    static constexpr uint32_t kNumFramesInFlight = 3;
    static constexpr uint32_t kHeapSizeInTiles   = 256;

    // ─── HeapAllocator ───────────────────────────────────────────────────────
    //
    // Manages D3D12 heap + virtual buffer pairs used as tile pools for tiled
    // resources.  Heaps are released via deferred frame-bucket queues so that
    // GPU command lists still referencing the heap (up to kNumFramesInFlight
    // frames ago) are guaranteed to have finished before the heap is destroyed.
    // ─────────────────────────────────────────────────────────────────────────

    class HeapAllocator
    {
    public:
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

        uint32_t m_NumHeaps = 0;
        uint64_t m_TotalAllocatedBytes = 0;

        std::vector<nvrhi::HeapHandle>   m_HeapsToRelease[kNumFramesInFlight];
        std::vector<nvrhi::BufferHandle> m_BuffersToRelease[kNumFramesInFlight];
    };

    // ─── FeedbackManager ─────────────────────────────────────────────────────

    class FeedbackManager
    {
    public:
        FeedbackManager();

        FeedbackTexture* CreateTexture(const nvrhi::TextureDesc& desc);
        std::unique_ptr<FeedbackTextureSet> CreateTextureSet();

        void BeginFrame(nvrhi::ICommandList* commandList, FeedbackTextureCollection& results);

        void UpdateTileMappings(nvrhi::ICommandList* commandList, FeedbackTextureCollection& tilesReady);

        void ResolveFeedback(nvrhi::ICommandList* commandList);
        void EndFrame();

        const FeedbackManagerStats& GetStats() const;

        // Internal helpers called by FeedbackTexture
        // Called to destroy a texture — cleans up ringbuffer/readback references and releases ownership.
        // Not called from ~FeedbackTexture (FeedbackManager owns the unique_ptrs); external code calls this
        // when a texture should be removed (e.g., scene unload / resource teardown).
        void UnregisterTexture(uint32_t textureIdx);
        void UpdateTextureRingBufferState(uint32_t textureIdx, bool bIncludeInRingBuffer);

        // Map packed mips permanently (called once at scene load before uploading packed mip data).
        // Must be called after CreateTexture and before writing packed mip data via writeTexture.
        void MapPackedMips(uint32_t textureIdx);

        rtxts::TiledTextureManager* GetTiledTextureManager() { return m_TiledTextureManager.get(); }

    private:
        uint32_t m_FrameIndex = 0;

        std::vector<std::unique_ptr<FeedbackTexture>> m_Textures;
        std::vector<uint32_t>                         m_TexturesRingbuffer;
        uint32_t                                      m_RingbufferCursor = 0;
        std::vector<uint32_t>                         m_TexturesToReadback[kNumFramesInFlight];

        // Helpers to resolve index → pointer
        FeedbackTexture* GetTextureByIndex(uint32_t idx) { return m_Textures.at(idx).get(); }

        FeedbackManagerStats m_StatsLastFrame{};

        double m_BeginFrameCPUTime = 0.0;
        double m_UpdateTileMappingsCPUTime = 0.0;
        double m_ResolveCPUTime = 0.0;

        std::unique_ptr<HeapAllocator>              m_HeapAllocator;
        std::unique_ptr<rtxts::TiledTextureManager> m_TiledTextureManager;
        std::unordered_set<uint32_t>                m_MinMipDirtyTextures;
    };

} // namespace nvfeedback
