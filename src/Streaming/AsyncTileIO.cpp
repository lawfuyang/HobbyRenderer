#include "AsyncTileIO.h"

namespace nvfeedback
{
    // ─── Helpers ─────────────────────────────────────────────────────────────

    // Extract a tile from a linear DDS mip into dst.
    static void ExtractTile(
        const uint8_t* srcBase,
        uint32_t mipWidth,
        uint32_t tileXInTexels, uint32_t tileYInTexels,
        uint32_t tileWidthInTexels, uint32_t tileHeightInTexels,
        uint32_t bytesPerBlock, uint32_t blockSize,
        uint8_t* dst)
    {
        const uint32_t blocksPerRow = (mipWidth + blockSize - 1) / blockSize;
        const uint32_t mipHeightInBlocks = blocksPerRow; // square-ish, but safe upper bound: use blocksPerRow
        const uint32_t tileBlocksW  = (tileWidthInTexels  + blockSize - 1) / blockSize;
        const uint32_t tileBlocksH  = (tileHeightInTexels + blockSize - 1) / blockSize;
        const uint32_t srcBlockX    = tileXInTexels / blockSize;
        const uint32_t srcBlockY    = tileYInTexels / blockSize;
        const uint32_t rowPitchSrc  = blocksPerRow * bytesPerBlock;
        const uint32_t rowPitchTile = tileBlocksW  * bytesPerBlock;

        SDL_assert(tileBlocksW > 0 && tileBlocksH > 0);
        SDL_assert(srcBlockX + tileBlocksW <= blocksPerRow);

        for (uint32_t row = 0; row < tileBlocksH; row++)
        {
            const uint32_t readOffset = (srcBlockY + row) * rowPitchSrc + srcBlockX * bytesPerBlock;
            memcpy(dst + row * rowPitchTile, srcBase + readOffset, rowPitchTile);
        }
    }

    // ─── AsyncTileIO ─────────────────────────────────────────────────────────

    AsyncTileIO::AsyncTileIO()
    {
        // Default: half of hardware threads, at least 1, at most 4
        const uint32_t hw = std::thread::hardware_concurrency();
        const uint32_t numWorkers = std::max(1u, std::min(4u, hw / 2));

        m_Workers.reserve(numWorkers);

        for (uint32_t i = 0; i < numWorkers; i++)
        {
            m_Workers.emplace_back([this]() { WorkerLoop(); });
        }
    }

    AsyncTileIO::~AsyncTileIO()
    {
        // Signal shutdown and wake all workers
        {
            std::lock_guard<std::mutex> lock(m_PendingMutex);
            m_bShutdown.store(true, std::memory_order_release);
        }
        m_PendingCV.notify_all();

        for (std::thread& t : m_Workers)
        {
            if (t.joinable())
                t.join();
        }
    }

    void AsyncTileIO::Submit(TileRequest request)
    {
        m_PendingCount.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(m_PendingMutex);
            m_PendingQueue.push(std::move(request));
        }
        m_PendingCV.notify_one();
    }

    uint32_t AsyncTileIO::Flush(nvrhi::ICommandList* cmd)
    {
        uint32_t processed = 0;

        // Drain all completed requests on the main thread
        std::vector<CompletedRequest> local;
        {
            std::lock_guard<std::mutex> lock(m_CompletedMutex);
            std::swap(local, m_CompletedQueue);
        }

        if (!cmd)
        {
            return 0;
        }

        for (CompletedRequest& cr : local)
        {
            SDL_assert(cr.m_Request.m_OnComplete);
            cr.m_Request.m_OnComplete(cr.m_Request, cr.m_TileData.data(), cr.m_TileData.size(), cmd);
            processed++;
        }

        return processed;
    }

    void AsyncTileIO::WaitIdle()
    {
        // Spin until all submitted requests have been completed
        while (m_PendingCount.load(std::memory_order_acquire) > 0)
        {
            std::this_thread::yield();
        }
    }

    void AsyncTileIO::WorkerLoop()
    {
        std::vector<uint8_t> scratch;

        while (true)
        {
            TileRequest req;

            // Wait for work
            {
                std::unique_lock<std::mutex> lock(m_PendingMutex);
                m_PendingCV.wait(lock, [this]() {
                    return m_bShutdown.load(std::memory_order_acquire) || !m_PendingQueue.empty();
                });

                if (m_bShutdown.load(std::memory_order_acquire) && m_PendingQueue.empty())
                    return;

                req = std::move(m_PendingQueue.front());
                m_PendingQueue.pop();
            }

            // ── Extract tile data from mmap'd DDS ──
            const uint8_t* ddsBase = static_cast<const uint8_t*>(req.m_SourceData->GetData());

            // Use the cached per-mip file offsets (parsed once from the DDS header at load time)
            size_t mipOffset = req.m_MipOffsets[req.m_MipLevel];
            SDL_assert(req.m_MipLevel < srrhi::CommonConsts::MAX_MIP_COUNT);

            uint32_t mipWidth = std::max(req.m_TextureWidth >> req.m_MipLevel, 1u);

            uint32_t tileBlocksW = (req.m_TileWidthInTexels  + req.m_BlockSize - 1) / req.m_BlockSize;
            uint32_t tileBlocksH = (req.m_TileHeightInTexels + req.m_BlockSize - 1) / req.m_BlockSize;
            size_t tileDataSize  = static_cast<size_t>(tileBlocksW) * tileBlocksH * req.m_BytesPerBlock;

            // SDL_Log("[Streaming] Extracting tile: mip=%u tex=(%u,%u) size=(%u,%u) blocks=(%u,%u) dataSize=%zu offset=%zu",
            //         req.m_MipLevel, req.m_TileXInTexels, req.m_TileYInTexels,
            //         req.m_TileWidthInTexels, req.m_TileHeightInTexels,
            //         tileBlocksW, tileBlocksH, tileDataSize, mipOffset);

            // Resize scratch buffer (only grows, never shrinks)
            if (scratch.size() < tileDataSize)
                scratch.resize(tileDataSize);

            ExtractTile(
                ddsBase + mipOffset,
                mipWidth,
                req.m_TileXInTexels, req.m_TileYInTexels,
                req.m_TileWidthInTexels, req.m_TileHeightInTexels,
                req.m_BytesPerBlock, req.m_BlockSize,
                scratch.data());

            // ── Push to completed queue ──
            CompletedRequest cr;
            cr.m_Request = std::move(req);
            cr.m_TileData.assign(scratch.data(), scratch.data() + tileDataSize);

            {
                std::lock_guard<std::mutex> lock(m_CompletedMutex);
                m_CompletedQueue.push_back(std::move(cr));
            }

            m_PendingCount.fetch_sub(1, std::memory_order_release);
        }
    }

} // namespace nvfeedback
