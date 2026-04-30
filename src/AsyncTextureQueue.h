#pragma once

#include "AsyncQueueBase.h"
#include "PendingInstanceUpdate.h"
#include "Scene.h"

// ─── AsyncTextureQueue ───────────────────────────────────────────────────────
// Loads texture files on a background thread (CPU only: no GPU work).
// The caller-provided callback is invoked on the background thread with a
// TextureUpdateCommand containing ready-to-upload CPU data.
// Scene::ApplyPendingUpdates() should collect these commands and perform
// the GPU texture creation/upload on the main thread.
class AsyncTextureQueue : public AsyncQueueBase
{
public:
    AsyncTextureQueue();

    using OnLoadedCallback = std::function<void(TextureUpdateCommand)>;

    // Enqueue a texture load.  Returns a PendingLoadID that can be passed to
    // CancelLoad() before the background task starts executing.
    // textureIndex: index into Scene::m_Textures for the slot to fill.
    PendingLoadID EnqueueLoad(std::string filePath,
                               uint32_t textureIndex,
                               Scene::Texture::SamplerType sampler,
                               OnLoadedCallback callback);

    // Request cancellation of a pending load.  If the task has already started
    // executing, the cancellation flag on the resulting command will be set to
    // true and the callback will still be invoked (so callers should check
    // TextureUpdateCommand::m_bCancelled).
    void CancelLoad(PendingLoadID id);

private:
    std::atomic<PendingLoadID> m_NextID{ 1 };
    std::mutex                 m_CancelMutex;
    std::unordered_set<PendingLoadID> m_CancelledIDs;
};
