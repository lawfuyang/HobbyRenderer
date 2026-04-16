
#include "TaskScheduler.h"

TaskScheduler::TaskScheduler()
{
    SetThreadCount(kRuntimeThreadCount);
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
        if (worker.joinable())
        {
            worker.join();
        }
    }
}

void TaskScheduler::SetThreadCount(uint32_t count)
{
    std::vector<std::thread> threadsToJoin;
    {
        std::lock_guard<std::mutex> lock(m_QueueMutex);
        
        if (count > m_Workers.size())
        {
            uint32_t startIdx = static_cast<uint32_t>(m_Workers.size());
            m_TargetThreadCount = count;
            for (uint32_t i = startIdx; i < count; ++i)
            {
                m_Workers.emplace_back(&TaskScheduler::WorkerThread, this, i);
            }
        }
        else if (count < m_Workers.size())
        {
            m_TargetThreadCount = count;
            m_Condition.notify_all();
            
            for (uint32_t i = count; i < m_Workers.size(); ++i)
            {
                threadsToJoin.push_back(std::move(m_Workers[i]));
            }
            m_Workers.resize(count);
        }
        else
        {
            m_TargetThreadCount = count;
        }
    }

    for (std::thread& t : threadsToJoin)
    {
        if (t.joinable())
        {
            t.join();
        }
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
    m_CompletionCondition.notify_all();

    std::unique_lock<std::mutex> lock(completionMutex);
    completionCondition.wait(lock, [&remaining]() { return remaining == 0; });
}

void TaskScheduler::ScheduleTask(std::function<void()> func, bool bImmediateExecute)
{
    std::lock_guard<std::mutex> lock(m_QueueMutex);
    if (bImmediateExecute)
    {
        m_RemainingTasks.fetch_add(1);
        m_Tasks.push_back([func](uint32_t)
            {
                func();
            });

        m_Condition.notify_one();
    }
    else
    {
        m_DeferredTasks.push_back([func]() { func(); });
    }
}

void TaskScheduler::ExecuteAllScheduledTasks()
{
    PROFILE_FUNCTION();

    m_Condition.notify_all();

    for (std::function<void()>& deferredTask : m_DeferredTasks)
    {
        ScheduleTask(std::move(deferredTask));
    }
    m_DeferredTasks.clear();

    while (m_RemainingTasks > 0)
    {
        std::function<void(uint32_t)> task;
        {
            std::lock_guard<std::mutex> lock(m_QueueMutex);
            if (!m_Tasks.empty())
            {
                task = std::move(m_Tasks.back());
                m_Tasks.pop_back();
            }
        }

        if (task)
        {
            task(GetThreadCount());

            if (m_RemainingTasks.fetch_sub(1) == 1)
            {
                std::lock_guard<std::mutex> lock(m_CompletionMutex);
                m_CompletionCondition.notify_all();
            }
        }
        else
        {
            std::unique_lock<std::mutex> lock(m_CompletionMutex);
            if (m_RemainingTasks > 0)
            {
                m_CompletionCondition.wait(lock, [this]() { return m_RemainingTasks == 0 || !m_Tasks.empty(); });
            }
        }
    }
}

void TaskScheduler::WorkerThread(uint32_t threadIndex)
{
    while (true)
    {
        std::function<void(uint32_t)> task;
        {
            std::unique_lock<std::mutex> lock(m_QueueMutex);
            m_Condition.wait(lock, [this, threadIndex]() { return m_Stop || !m_Tasks.empty() || threadIndex >= m_TargetThreadCount; });
            
            if (m_Stop && m_Tasks.empty()) return;

            if (threadIndex >= m_TargetThreadCount) return;

            if (m_Tasks.empty()) continue;

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
