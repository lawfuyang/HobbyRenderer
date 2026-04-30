#include "pch.h"
#include "AsyncTextureQueue.h"
#include "TextureLoader.h"

AsyncTextureQueue::AsyncTextureQueue()
    : AsyncQueueBase(2) // 2 worker threads: texture loading is IO-heavy and benefits from some parallelism, but too many may cause disk thrashing.
{
}

PendingLoadID AsyncTextureQueue::EnqueueLoad(std::string filePath,
                                              uint32_t textureIndex,
                                              Scene::Texture::SamplerType /*sampler*/,
                                              OnLoadedCallback callback)
{
    PendingLoadID id = m_NextID.fetch_add(1, std::memory_order_relaxed);

    EnqueueTask([this, id, path = std::move(filePath), textureIndex, cb = std::move(callback)]() mutable
    {
        TextureUpdateCommand cmd;
        cmd.m_LoadID       = id;
        cmd.m_TextureIndex = textureIndex;

        {
            std::lock_guard<std::mutex> lk(m_CancelMutex);
            if (m_CancelledIDs.count(id))
            {
                m_CancelledIDs.erase(id);
                cmd.m_bCancelled = true;
                cb(std::move(cmd));
                return;
            }
        }

        if (!LoadTexture(path, cmd.m_Desc, cmd.m_Data))
        {
            SDL_Log("[AsyncTextureQueue] Failed to load texture: %s", path.c_str());
            cmd.m_bCancelled = true;
        }
        else
        {
            cmd.m_Desc.debugName = path;
        }

        cb(std::move(cmd));
    });

    return id;
}

void AsyncTextureQueue::CancelLoad(PendingLoadID id)
{
    std::lock_guard<std::mutex> lk(m_CancelMutex);
    m_CancelledIDs.insert(id);
}
