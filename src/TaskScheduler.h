#pragma once

class TaskScheduler
{
public:
    TaskScheduler();
    ~TaskScheduler();

    void ParallelFor(uint32_t count, const std::function<void(uint32_t index, uint32_t threadIndex)>& func);
    void ScheduleTask(std::function<void()> func, bool bImmediateExecute = true);
    void ExecuteAllScheduledTasks();

    uint32_t GetThreadCount() const { return static_cast<uint32_t>(m_Workers.size()); }

private:
    void WorkerThread(uint32_t threadIndex);

    std::vector<std::thread> m_Workers;
    std::vector<std::function<void(uint32_t)>> m_Tasks;
    
    std::mutex m_QueueMutex;
    std::condition_variable m_Condition;
    std::atomic<bool> m_Stop{ false };

    std::atomic<uint32_t> m_RemainingTasks{ 0 };
    std::mutex m_CompletionMutex;
    std::condition_variable m_CompletionCondition;
};
