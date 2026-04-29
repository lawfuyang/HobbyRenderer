#pragma once

// ============================================================================
// AsyncQueueBase
// ============================================================================
// Non-template base class for background-thread queues.
//
// The queue stores type-erased std::function<void()> tasks.  One or more
// worker threads pop and execute tasks concurrently in FIFO order.
// The worker count is fixed at construction time (default: 1).
//
// Public API:
//   AsyncQueueBase(workerCount)  — choose thread-pool size (default 1).
//   Start(logTag)                — launch worker threads (call before Enqueue*).
//   Stop(logTag)                 — drain queue, join all workers.
//   IsRunning()                  — true between Start() and Stop().
//   GetQueuedCount()             — items waiting in the queue (not yet picked up).
//   GetPendingCount()            — items queued + currently executing.
//   Flush()                      — block until GetPendingCount() reaches 0.
//
// Protected API (for derived classes):
//   EnqueueTask(task)  — push a callable; increments pending count.
//
// Thread safety:
//   EnqueueTask() / GetQueuedCount() / GetPendingCount() — any thread.
//   Start() / Stop() / Flush() — single owner thread only.
// ============================================================================
class AsyncQueueBase
{
public:
    // workerCount: number of background threads to spawn on Start().
    // Default is 1 (original single-thread behaviour).
    explicit AsyncQueueBase(uint32_t workerCount = 1);

    // Destructor: if workers were started but never stopped, join them here
    // to avoid std::terminate on joinable threads.
    virtual ~AsyncQueueBase();

    void Start(const char* logTag);
    void Stop(const char* logTag);

    bool     IsRunning()       const { return m_bRunning; }
    uint32_t GetQueuedCount()  const;
    uint32_t GetPendingCount() const;

    // Block until all queued + in-flight tasks complete. No-op if not running.
    void Flush();

    // ── Static registry ──────────────────────────────────────────────────────
    // Queues are registered automatically on Start() and removed on Stop().
    // Access is main-thread only (Start/Stop are owner-thread; UI reads here).
    struct RegistryEntry { const char* m_Name; AsyncQueueBase* m_Queue; };
    static const std::vector<RegistryEntry>& GetActiveQueues();

private:
    inline static std::vector<RegistryEntry> ms_ActiveQueues;

protected:
    using Task = std::function<void()>;

    // Push a task onto the queue. Increments pending count and wakes one worker.
    // Thread-safe; may be called from any thread after Start().
    void EnqueueTask(Task task);

private:
    void ThreadFunc();
    void JoinAll();

    const uint32_t          m_WorkerCount;
    std::vector<std::thread> m_Workers;

    mutable std::mutex      m_Mutex;
    std::condition_variable m_CV;
    std::condition_variable m_DrainCV;
    std::queue<Task>        m_Queue;
    uint32_t                m_PendingCount = 0;
    bool                    m_bRunning     = false;
    bool                    m_bStopping    = false;
};
