#include "pch.h"

#include "TaskExecutor.h"
#include "Utilities.h"

namespace
{
    std::mutex gs_ExecutorMutex;
    std::shared_ptr<tf::Executor> gs_Executor;

    std::mutex gs_DeferredMutex;
    std::vector<std::function<void()>> gs_DeferredTasks;

    // microprofile gives a thread a row of its own only after that thread has called MicroProfileOnThreadCreate, and
    // silently drops every scope recorded without one. Taskflow's workers are plain std::threads it creates itself,
    // so without this hook all the work that runs on the pool - every ParallelFor item, every deferred render pass -
    // is missing from a capture. The prologue runs on the worker thread as it enters its scheduling loop, the
    // epilogue as it leaves, which is also where the row is handed back.
    class MicroProfileWorkerInterface : public tf::WorkerInterface
    {
    public:
        void scheduler_prologue(tf::Worker& worker) override
        {
            // The pool is never rebuilt, so the worker id is this thread's creation index for the whole run.
            char name[32];
            std::snprintf(name, sizeof(name), "Taskflow W%zu", worker.id());
            MicroProfileOnThreadCreate(name);
        }

        void scheduler_epilogue(tf::Worker& /*worker*/, std::exception_ptr /*failure*/) override
        {
            MicroProfileOnThreadExit();
        }
    };
}

tf::Executor& GetTaskExecutor()
{
    // One pool for the whole run: the load and the frame loop share these threads, which also means one profiler row
    // per worker instead of a retired set from each worker count the run went through.
    std::lock_guard<std::mutex> lock(gs_ExecutorMutex);
    if (!gs_Executor)
    {
        gs_Executor = std::make_shared<tf::Executor>(std::max<size_t>(1, std::thread::hardware_concurrency()),
                                                     std::make_shared<MicroProfileWorkerInterface>());
    }
    return *gs_Executor;
}

void ShutdownTaskExecutor()
{
    // Moved out and released after the lock, so a worker that is already leaving the pool while the destructor waits
    // for it can still take the lock.
    std::shared_ptr<tf::Executor> executor;
    {
        std::lock_guard<std::mutex> lock(gs_ExecutorMutex);
        executor = std::move(gs_Executor);
    }
}

uint32_t GetTaskWorkerCount()
{
    return uint32_t(GetTaskExecutor().num_workers());
}

void ParallelFor(uint32_t count, const std::function<void(uint32_t)>& body)
{
    if (count == 0)
        return;
    if (count == 1)
    {
        body(0);
        return;
    }

    tf::Executor& executor = GetTaskExecutor();

    // The first failure is rethrown on the calling thread: a task body that throws cannot report it any other way,
    // and swallowing it would leave a half-built result behind.
    std::exception_ptr failure;
    std::mutex failureMutex;
    const auto runItem = [&body, &failure, &failureMutex](uint32_t index)
    {
        try
        {
            body(index);
        }
        catch (...)
        {
            std::lock_guard<std::mutex> lock(failureMutex);
            if (!failure)
                failure = std::current_exception();
        }
    };

    if (executor.this_worker_id() >= 0)
    {
        tf::TaskGroup group = executor.task_group();
        for (uint32_t i = 0; i < count; ++i)
            group.silent_async([&runItem, i]() { runItem(i); });
        group.corun();
    }
    else
    {
        // A thread that is not a pool worker may simply block, so it waits for the count to reach zero while idle
        // workers pick the items up.
        std::atomic<uint32_t> remaining{ count };
        std::mutex mutex;
        std::condition_variable condition;
        for (uint32_t i = 0; i < count; ++i)
        {
            executor.silent_async([&runItem, &remaining, &mutex, &condition, i]()
            {
                runItem(i);
                if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    condition.notify_all();
                }
            });
        }

        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&remaining]() { return remaining.load(std::memory_order_acquire) == 0; });
    }

    if (failure)
        std::rethrow_exception(failure);
}

void ScheduleTask(std::function<void()> func, bool bImmediateExecute)
{
    if (!bImmediateExecute)
    {
        std::lock_guard<std::mutex> lock(gs_DeferredMutex);
        gs_DeferredTasks.push_back(std::move(func));
        return;
    }

    GetTaskExecutor().silent_async(std::move(func));
}

void WaitForAllTasks()
{
    PROFILE_FUNCTION();

    // Deferred tasks go to the pool: the render graph queues one per pass and records them concurrently, which it
    // supports because its active-pass index is thread-local (RenderGraph::t_ActivePassIndex) - each pass reads the
    // state it declared during Setup, whichever thread records it. The deferral is about *when* they run (after the
    // graph compiles), not about serialising them; running them here instead would leave every pass on the calling
    // thread and hide the whole frame from the profiler's per-thread rows.
    std::vector<std::function<void()>> deferred;
    {
        std::lock_guard<std::mutex> lock(gs_DeferredMutex);
        deferred.swap(gs_DeferredTasks);
    }

    tf::Executor& executor = GetTaskExecutor();
    for (std::function<void()>& task : deferred)
        executor.silent_async(std::move(task));

    executor.wait_for_all();
}
