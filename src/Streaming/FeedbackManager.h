#pragma once

#include "FeedbackTexture.h"
#include "FeedbackTextureSet.h"
#include "Utilities.h"

#include <rtxts-ttm/TiledTextureManager.h>

namespace nvfeedback
{
    // ─── Public structs ──────────────────────────────────────────────────────

    struct FeedbackManagerStats
    {
        uint64_t m_HeapAllocationInBytes = 0;
        uint32_t m_NumHeaps      = 0;
        uint32_t m_NumTTMHeaps   = 0;
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
        uint32_t m_TextureIdx = UINT32_MAX;
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

        void BeginFrame(nvrhi::ICommandList* commandList, FeedbackTextureCollection& results);

        void UpdateTileMappings(nvrhi::ICommandList* commandList, FeedbackTextureCollection& tilesReady);

        void ResolveFeedback(nvrhi::ICommandList* commandList);
        void EndFrame();

        const FeedbackManagerStats& GetStats() const;

        // Resolve an index (from FeedbackTextureUpdate::m_TextureIdx) to the actual FeedbackTexture pointer.
        // External code uses this after receiving a FeedbackTextureCollection from BeginFrame.
        FeedbackTexture* GetTextureByIndex(uint32_t idx) { return m_Textures.at(idx).get(); }

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

        FeedbackManagerStats m_StatsLastFrame{};

        double m_BeginFrameCPUTime = 0.0;
        double m_UpdateTileMappingsCPUTime = 0.0;
        double m_ResolveCPUTime = 0.0;

        std::unique_ptr<HeapAllocator>              m_HeapAllocator;
        std::unique_ptr<rtxts::TiledTextureManager> m_TiledTextureManager;
        std::unordered_set<uint32_t>                m_MinMipDirtyTextures;

        // Running cursor for packed-mip tile suballocation.
        // Packed mips fill tiles sequentially across dedicated heaps
        // (heapId = cursor / kHeapSizeInTiles, localTile = cursor % kHeapSizeInTiles).
        // These heaps are allocated via AllocateHeap but NOT registered with TTM
        // (no AddHeap), because TTM has no concept of externally-mapped tiles and
        // would otherwise allocate streaming tiles into packed-mip physical slots.
        uint32_t m_PackedMipTileCursor = 0;

        // Number of heaps registered with TiledTextureManager via AddHeap.
        // This is the count used in BeginFrame's heap management comparison
        // against GetNumDesiredHeaps(). It excludes packed-mip-only heaps.
        uint32_t m_NumTTMHeaps = 0;
    };

} // namespace nvfeedback
