#pragma once

class TaskScheduler
{
public:
    TaskScheduler();
    ~TaskScheduler();

    void ParallelFor(uint32_t count, const std::function<void(uint32_t index, uint32_t threadIndex)>& func);

    uint32_t GetThreadCount() const { return static_cast<uint32_t>(m_Workers.size()); }

private:
    void WorkerThread(uint32_t threadIndex);

    std::vector<std::thread> m_Workers;
    std::vector<std::function<void(uint32_t)>> m_Tasks;
    
    std::mutex m_QueueMutex;
    std::condition_variable m_Condition;
    std::atomic<bool> m_Stop{ false };
};
