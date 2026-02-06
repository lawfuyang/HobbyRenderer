#include "pch.h"
#include "TaskScheduler.h"

TaskScheduler::TaskScheduler()
{
    //const uint32_t kNumThreads = std::thread::hardware_concurrency();
    const uint32_t kNumThreads = 8;
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

    {
        std::lock_guard<std::mutex> lock(m_QueueMutex);
        m_RemainingTasks.fetch_add(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            m_Tasks.push_back([i, &func, &remaining, &completionCondition, &completionMutex](uint32_t threadIndex)
                {
                    func(i, threadIndex);

                    // last task to finish signals completion
                    if (remaining.fetch_sub(1) == 1)
                    {
                        std::lock_guard<std::mutex> lock(completionMutex);
                        completionCondition.notify_all();
                    }
                });
        }
    }
    m_Condition.notify_all();

    std::unique_lock<std::mutex> lock(completionMutex);
    completionCondition.wait(lock, [&remaining]() { return remaining == 0; });
}

void TaskScheduler::ScheduleTask(std::function<void()> func)
{
    {
        std::lock_guard<std::mutex> lock(m_QueueMutex);
        m_RemainingTasks.fetch_add(1);
        m_Tasks.push_back([func](uint32_t)
            {
                func();
            });
    }
    m_Condition.notify_one();
}

void TaskScheduler::ExecuteAllScheduledTasks()
{
    std::unique_lock<std::mutex> lock(m_CompletionMutex);
    m_Condition.notify_all();
    m_CompletionCondition.wait(lock, [this]() { return m_RemainingTasks == 0; });
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

        if (m_RemainingTasks.fetch_sub(1) == 1)
        {
            std::lock_guard<std::mutex> lock(m_CompletionMutex);
            m_CompletionCondition.notify_all();
        }
    }
}
