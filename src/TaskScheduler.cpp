#include "pch.h"
#include "TaskScheduler.h"

TaskScheduler::TaskScheduler()
{
    const uint32_t kNumThreads = std::thread::hardware_concurrency();
    for (uint32_t i = 0; i < kNumThreads; ++i)
    {
        m_Workers.emplace_back(&TaskScheduler::WorkerThread, this, i);
    }
}

TaskScheduler::~TaskScheduler()
{
    {
        std::lock_guard<std::mutex> lock(m_QueueMutex);
        m_Stop = true;
    }
    m_Condition.notify_all();
    for (std::thread& worker : m_Workers)
    {
        worker.join();
    }
}

void TaskScheduler::ParallelFor(uint32_t count, const std::function<void(uint32_t index, uint32_t threadIndex)>& func)
{
    if (count == 0) return;

    std::atomic<uint32_t> remaining{ count };
    std::mutex completionMutex;
    std::condition_variable completionCondition;

    for (uint32_t i = 0; i < count; ++i)
    {
        std::lock_guard<std::mutex> lock(m_QueueMutex);
        m_Tasks.push_back([i, &func, &remaining, &completionCondition](uint32_t threadIndex)
            {
                func(i, threadIndex);

                // last task to finish signals completion
                if (remaining.fetch_sub(1) == 1)
                {
                    completionCondition.notify_all();
                }
            });
    }
    m_Condition.notify_all();

    std::unique_lock<std::mutex> lock(completionMutex);
    completionCondition.wait(lock, [&remaining]() { return remaining == 0; });
}

void TaskScheduler::WorkerThread(uint32_t threadIndex)
{
    while (true)
    {
        std::function<void(uint32_t)> task;
        {
            std::unique_lock<std::mutex> lock(m_QueueMutex);
            m_Condition.wait(lock, [this]() { return m_Stop || !m_Tasks.empty(); });
            if (m_Stop && m_Tasks.empty()) return;
            task = std::move(m_Tasks.back());
            m_Tasks.pop_back();
        }
        
        task(threadIndex); 
    }
}
