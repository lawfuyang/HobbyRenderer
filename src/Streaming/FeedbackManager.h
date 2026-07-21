#pragma once

#include "FeedbackTexture.h"
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

    static constexpr bool kStreamingDebugLog = false;
    static constexpr uint32_t kNumFramesInFlight = 3;
    static constexpr uint32_t kHeapSizeInTiles   = 256;
    static constexpr uint32_t kFeedbackTexturesToResolvePerFrame = 30;

    // Maximum number of tile indices submitted to AsyncTileIO per frame.
    // Matches the RTXTS reference's tilesPerFrame budget.  Excess tiles are deferred
    // to m_PendingTileRequests and submitted in subsequent frames, preventing large
    // single-frame upload batches (e.g. 1623 tiles = 104 MB in one frame).
    static constexpr uint32_t kMaxTilesPerFrame = 128;

    // Hysteresis timeout: a tile stays mapped for this many seconds after the
    // last time sampler feedback requested it.  TTM transitions Mapped→Standby
    // (not Free) when the timeout expires; Standby tiles are only freed when
    // the standby queue overflows (numExtraStandbyTiles).
    //
    // With 307 textures at 30/frame the ringbuffer cycle is ~10 frames (~167ms
    // at 60fps), so 1 second gives ~6 full cycles of margin before any tile
    // times out.  No per-frame re-submission of cached feedback is needed.
    static constexpr float kTileHysteresisSeconds = 1.0f;

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
        // allowHeapRelease: pass false while m_PendingTileRequests is non-empty.
        // Releasing empty heaps while tiles are still pending upload causes churn:
        // TTM has already allocated heap slots for those tiles; releasing the heap
        // then re-allocating it next frame produces the grow-shrink pattern.
        void BeginFrame(nvrhi::ICommandList* commandList, std::vector<FeedbackTextureUpdate>& results, bool allowHeapRelease = true);
        void UpdateTileMappings(nvrhi::ICommandList* commandList, std::vector<FeedbackTextureUpdate>& tilesReady);
        void ResolveFeedback(nvrhi::ICommandList* commandList);
        void EndFrame();

        const FeedbackManagerStats& GetStats() const;

        // Resolve an index (from FeedbackTextureUpdate::m_TextureIdx) to the actual FeedbackTexture pointer.
        // External code uses this after receiving results from BeginFrame.
        FeedbackTexture* GetTextureByIndex(uint32_t idx) { return m_Textures.at(idx).get(); }
        uint32_t GetNumTextures() const { return (uint32_t)m_Textures.size(); }

        // Internal helpers called by FeedbackTexture
        // Called to destroy a texture — cleans up ringbuffer/readback references and releases ownership.
        // Not called from ~FeedbackTexture (FeedbackManager owns the unique_ptrs); external code calls this
        // when a texture should be removed (e.g., scene unload / resource teardown).
        void UnregisterTexture(uint32_t textureIdx);

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

        // Number of heaps registered with TiledTextureManager via AddHeap.
        // Includes both packed-mip heaps (allocated in MapPackedMips) and
        // streaming heaps (allocated in BeginFrame Step 4).
        uint32_t m_NumTTMHeaps = 0;
    };

} // namespace nvfeedback
