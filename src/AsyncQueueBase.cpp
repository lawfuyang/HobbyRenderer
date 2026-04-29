#include "pch.h"
#include "AsyncQueueBase.h"

const std::vector<AsyncQueueBase::RegistryEntry>& AsyncQueueBase::GetActiveQueues()
{
    return ms_ActiveQueues;
}

AsyncQueueBase::AsyncQueueBase(uint32_t workerCount)
    : m_WorkerCount(workerCount > 0 ? workerCount : 1)
{
}

AsyncQueueBase::~AsyncQueueBase()
{
    // Safety net: if the owner forgot to call Stop() before destruction,
    // stop the workers here to avoid std::terminate on joinable threads.
    bool anyJoinable = false;
    for (std::thread& t : m_Workers)
        if (t.joinable()) { anyJoinable = true; break; }

    if (anyJoinable)
    {
        {
            std::lock_guard<std::mutex> lk(m_Mutex);
            m_bStopping = true;
        }
        m_CV.notify_all();
        JoinAll();
        m_bRunning = false;

        ms_ActiveQueues.erase(
            std::remove_if(ms_ActiveQueues.begin(), ms_ActiveQueues.end(),
                [this](const RegistryEntry& e) { return e.m_Queue == this; }),
            ms_ActiveQueues.end());
    }
}

void AsyncQueueBase::Start(const char* logTag)
{
    SDL_assert(!m_bRunning && "AsyncQueueBase::Start() called while already running");
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        m_bStopping = false;
        // Do NOT reset m_PendingCount here: items may have been enqueued before
        // Start() was called, and after a proper Stop() the count is already 0.
    }
    m_bRunning = true;
    m_Workers.reserve(m_WorkerCount);
    for (uint32_t i = 0; i < m_WorkerCount; ++i)
        m_Workers.emplace_back(&AsyncQueueBase::ThreadFunc, this);

    ms_ActiveQueues.push_back({ logTag, this });
    SDL_Log("[%s] Background thread pool started (%u worker%s)",
            logTag, m_WorkerCount, m_WorkerCount == 1 ? "" : "s");
}

void AsyncQueueBase::Stop(const char* logTag)
{
    bool anyJoinable = false;
    for (std::thread& t : m_Workers)
        if (t.joinable()) { anyJoinable = true; break; }

    if (!anyJoinable)
    {
        m_bRunning = false;
        return;
    }
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        m_bStopping = true;
    }
    m_CV.notify_all();
    JoinAll();
    m_bRunning = false;

    ms_ActiveQueues.erase(
        std::remove_if(ms_ActiveQueues.begin(), ms_ActiveQueues.end(),
            [this](const RegistryEntry& e) { return e.m_Queue == this; }),
        ms_ActiveQueues.end());

    SDL_Log("[%s] Background thread pool stopped", logTag);
}

void AsyncQueueBase::JoinAll()
{
    for (std::thread& t : m_Workers)
        if (t.joinable()) t.join();
    m_Workers.clear();
}

uint32_t AsyncQueueBase::GetQueuedCount() const
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    return static_cast<uint32_t>(m_Queue.size());
}

uint32_t AsyncQueueBase::GetPendingCount() const
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    return m_PendingCount;
}

void AsyncQueueBase::Flush()
{
    if (!m_bRunning)
        return;
    std::unique_lock<std::mutex> lk(m_Mutex);
    m_DrainCV.wait(lk, [this]() { return m_PendingCount == 0; });
}

void AsyncQueueBase::EnqueueTask(Task task)
{
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        ++m_PendingCount;
        m_Queue.push(std::move(task));
    }
    m_CV.notify_one();
}

void AsyncQueueBase::ThreadFunc()
{
    while (true)
    {
        Task task;
        {
            std::unique_lock<std::mutex> lock(m_Mutex);
            m_CV.wait(lock, [this]() { return !m_Queue.empty() || m_bStopping; });

            // Drain the queue completely before honouring a stop request.
            if (m_bStopping && m_Queue.empty())
                break;

            task = std::move(m_Queue.front());
            m_Queue.pop();
        }

        task();

        {
            std::lock_guard<std::mutex> lk(m_Mutex);
            SDL_assert(m_PendingCount > 0 && "AsyncQueueBase: pending count underflow");
            if (--m_PendingCount == 0)
                m_DrainCV.notify_all();
        }
    }
}
