#pragma once

#include "../Utilities.h"
#include "srrhi/cpp/Common.h"

namespace nvfeedback
{
    // ─── TileRequest ─────────────────────────────────────────────────────────
    // Describes a single tile that needs its data loaded from disk and uploaded
    // to the GPU.  Submitted to AsyncTileIO::Submit() and completed via the
    // callback on the main thread during Flush().
    // ─────────────────────────────────────────────────────────────────────────

    struct TileRequest
    {
        // Source DDS data (kept alive by the StreamingTexture::m_SourceData shared_ptr)
        std::shared_ptr<MemoryMappedDataReader> m_SourceData;

        // Destination reserved texture
        nvrhi::TextureHandle m_ReservedTexture;

        // Tile geometry (filled from FeedbackTextureTileInfo)
        uint32_t m_MipLevel         = 0;
        uint32_t m_TileXInTexels    = 0;
        uint32_t m_TileYInTexels    = 0;
        uint32_t m_TileWidthInTexels  = 0;
        uint32_t m_TileHeightInTexels = 0;

        // Full texture dimensions (needed for mipWidth computation)
        uint32_t m_TextureWidth  = 0;
        uint32_t m_TextureHeight = 0;

        // Cached mip data offsets (fixed-size array; indexed by m_MipLevel).
        // Filled from StreamingTexture::m_MipDataOffsets at submit time.
        size_t m_MipOffsets[srrhi::CommonConsts::MAX_MIP_COUNT] = {};

        // Format info (filled from nvrhi::getFormatInfo)
        nvrhi::Format m_Format       = nvrhi::Format::UNKNOWN;
        uint32_t m_BytesPerBlock     = 0;
        uint32_t m_BlockSize         = 0;   // 4 for BC, 1 for uncompressed

        // Completion callback — called on the main thread in Flush().
        // Receives the active command list for recording writeTexture calls.
        // The callback receives the tile data pointer and its byte size.
        std::function<void(const TileRequest&, const void* tileData, size_t tileDataSize, nvrhi::ICommandList* cmd)> m_OnComplete;
    };

    // ─── AsyncTileIO ─────────────────────────────────────────────────────────
    // Thread-pool based async tile I/O.
    //
    // Usage pattern (per frame):
    //   1. Submit() one TileRequest per tile that needs loading.
    //   2. Worker threads extract tile data from the mmap'd DDS into per-thread
    //      scratch buffers.
    //   3. Flush() on the main thread drains all completed requests and fires
    //      their m_OnComplete callbacks (which call cmd->writeTexture).
    //
    // Thread safety:
    //   Submit() and Flush() must be called from the main thread only.
    //   Worker threads only read from the mmap'd DDS and write to their own
    //   scratch buffers — no shared mutable state.
    // ─────────────────────────────────────────────────────────────────────────

    class AsyncTileIO
    {
    public:
        AsyncTileIO();
        ~AsyncTileIO();

        // Submit a tile request for async processing.
        // Thread-safe: may be called from the main thread while workers run.
        void Submit(TileRequest request);

        // Drain all completed requests and fire their onComplete callbacks.
        // cmd is the active command list used to record writeTexture calls.
        // Must be called from the main thread.
        // Returns the number of completed requests processed.
        uint32_t Flush(nvrhi::ICommandList* cmd);

        // Block until all pending requests are complete.
        void WaitIdle();

        // Number of requests currently in flight (submitted but not yet flushed).
        uint32_t PendingCount() const { return m_PendingCount.load(std::memory_order_relaxed); }

        // Number of worker threads.
        uint32_t WorkerCount() const { return static_cast<uint32_t>(m_Workers.size()); }

    private:
        struct CompletedRequest
        {
            TileRequest m_Request;
            std::vector<uint8_t> m_TileData;  // extracted tile bytes
        };

        void WorkerLoop();

        // Pending queue (main thread → workers)
        std::queue<TileRequest>  m_PendingQueue;
        mutable std::mutex       m_PendingMutex;
        std::condition_variable  m_PendingCV;

        // Completed vector (workers → main thread, fully drained each frame)
        std::vector<CompletedRequest> m_CompletedQueue;
        mutable std::mutex            m_CompletedMutex;

        std::vector<std::thread> m_Workers;
        std::atomic<bool>        m_bShutdown{ false };
        std::atomic<uint32_t>    m_PendingCount{ 0 };
    };

} // namespace nvfeedback
