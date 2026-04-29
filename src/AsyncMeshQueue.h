#pragma once

#include "AsyncQueueBase.h"
#include "PendingInstanceUpdate.h"

// ─── AsyncMeshQueue ──────────────────────────────────────────────────────────
// Loads and processes mesh data (vertex quantization, LOD generation, meshlet
// building) on a pool of background threads — CPU only, no GPU work.
//
// The caller-provided callback is invoked on a worker thread with a
// MeshUpdateCommand containing fully-processed, ready-to-upload data.
// Scene::ApplyPendingUpdates() should collect these commands and perform the
// GPU buffer re-creation on the main thread.
class AsyncMeshQueue : public AsyncQueueBase
{
public:
    // 4 worker threads: mesh processing (quantization, LOD, meshlets) is
    // CPU-heavy and embarrassingly parallel across primitives.
    AsyncMeshQueue() : AsyncQueueBase(4) {}

    using OnLoadedCallback = std::function<void(MeshUpdateCommand)>;

    // Enqueue a single glTF primitive to be loaded and processed asynchronously.
    // The info struct carries either a fast mmap path (binFilePath + accessor offsets)
    // or a fallback gltfPath for full re-parse when mmap is not possible.
    PendingLoadID EnqueueLoad(PendingAsyncMeshInfo info, OnLoadedCallback callback);

    // Request cancellation of a pending load (same semantics as AsyncTextureQueue).
    void CancelLoad(PendingLoadID id);

private:
    std::atomic<PendingLoadID> m_NextID{ 1 };
    std::mutex                 m_CancelMutex;
    std::unordered_set<PendingLoadID> m_CancelledIDs;
};
