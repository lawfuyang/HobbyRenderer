#pragma once

#include <taskflow/taskflow.hpp>

// The process-wide taskflow pool.
//
// The scene loader and the renderer both run their parallel loops here, so reaching a pool never means reaching the
// Renderer singleton.
//
// Work queued with ScheduleTask is fire-and-forget, so it has to be flushed with WaitForAllTasks before whatever
// it reads goes away. A batch whose stages depend on each other belongs in a tf::Taskflow instead: build it with
// emplace() and wire it with precede()/succeed(), then run it on GetTaskExecutor().

// The pool itself, for callers that build a task graph. Created on first use, destroyed by ShutdownTaskExecutor, so
// take it per use rather than storing it.
tf::Executor& GetTaskExecutor();

// Destroys the pool. Every worker exits its scheduling loop, and each one reports the exit to microprofile as it
// goes, so this has to run while the profiler is still up - before MicroProfileShutdown, which frees the per-thread
// logs that the exit path writes to. Nothing may run on the pool afterwards: a later call would recreate it, with
// new threads registering into a profiler that is already down.
void ShutdownTaskExecutor();

// Worker threads in the pool: one per logical core.
uint32_t GetTaskWorkerCount();

// Runs body(0..count-1) and returns once every call has finished. Safe to call from inside a pool task: the caller
// co-runs queued work instead of parking a worker, which is what keeps a nested loop (a build started from a
// build) from starving the pool.
void ParallelFor(uint32_t count, const std::function<void(uint32_t)>& body);

// Queues `func` on the pool. With `bImmediateExecute` false it is held until the next WaitForAllTasks, which is
// what lets the render graph record its passes only once the graph has been compiled.
void ScheduleTask(std::function<void()> func, bool bImmediateExecute = true);

// Blocks until every queued task has completed.
void WaitForAllTasks();
