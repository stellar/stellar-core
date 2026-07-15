#pragma once

// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/util/finally.h"
#include "util/GlobalChecks.h"
#include "util/NonCopyable.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

namespace stellar
{

// Executes batches of CPU-bound tasks in parallel on a pool of worker threads.
//
// Use this to speed up trivially parallelizable compute-heavy workloads.
//
// The executor pins the worker threads to physical cores. It may only run
// a single batch of tasks at a time, and there should be only a single executor
// instance per app, or else the execution would have unexpectedly degraded
// performance due to oversubscription of physical cores.
class BatchExecutor : private NonMovableOrCopyable
{
  public:
    BatchExecutor();
    // Destroys the executor and joins all worker threads.
    // No batch should be in progress - batch execution is not supposed to be
    // async.
    ~BatchExecutor();

    // Executes every task in `tasks` in parallel and returns their results in
    // the same order.
    //
    // Every task will always be executed by a separate worker thread. Passing
    // more tasks than physical cores is allowed, but will result in performance
    // degradation.
    //
    // Only a single thread may call executeBatch at a time (enforced by an
    // assertion), but the executor may be reused for multiple batches scheduled
    // from arbitrary threads over its lifetime.
    //
    // Rethrows the first exception captured from a `runTask` invocation,
    // if any.
    template <typename T>
    std::vector<T> executeBatch(std::vector<std::function<T()>> tasks);

  private:
    // Runs `runTask(0)..runTask(numTasks-1)` across `numTasks` pinned workers
    // and blocks until all complete. Rethrows the first exception captured from
    // a `runTask` invocation, if any. Type-erased so the worker loop never
    // depends on the task result type.
    void runBatchImpl(size_t numTasks,
                      std::function<void(size_t)> const& runTask);
    // Task execution loop running on every worker thread.
    void workerLoop(size_t index, uint64_t initialGeneration);
    // Grows the pool to at least `count` workers, pinning each new one to a
    // physical core if possible.
    void ensureWorkers(size_t count);
    // Pins worker `index` to its designated physical core. No-op on non-Linux
    // platforms or when the CPU topology is unavailable.
    void pinWorker(size_t index);

    // Plain std::mutex (rather than the annotated wrapper) as it has to work
    // with std::condition_variable.
    std::mutex mMutex;
    std::condition_variable mCondition;
    std::vector<std::thread> mWorkers;
    // One logical CPU per physical core; empty if pinning is unavailable. Set
    // once in the constructor and immutable afterwards.
    std::vector<unsigned> mPinCpus;

    // Task that is being currently executed by the workers.
    std::function<void(size_t)> const* mRunTask{nullptr};
    size_t mWorkersRequested{0};
    size_t mWorkersDone{0};
    std::exception_ptr mFirstError;
    // Index of the task batch, workers wake when it changes.
    uint64_t mBatchId{0};
    bool mDestroying{false};
    // Marks that a batch is currently running, to prevent concurrent
    // executeBatch calls.
    std::atomic<bool> mBatchRunning{false};
};

template <typename T>
std::vector<T>
BatchExecutor::executeBatch(std::vector<std::function<T()>> tasks)
{
    // NB: The default-constructible requirement is not strictly necessary and
    // may be lifted if necessary, but that would require a more complex
    // implementation.
    static_assert(std::is_default_constructible_v<T>,
                  "BatchExecutor::executeBatch result type must be "
                  "default-constructible");
    static_assert(!std::is_same_v<T, bool>,
                  "BatchExecutor::executeBatch does not support bool results");

    // Only one executeBatch may be in progress at a time.
    releaseAssert(!mBatchRunning.exchange(true));
    auto resetRunning = gsl::finally([this]() { mBatchRunning.store(false); });

    std::vector<T> results(tasks.size());
    if (tasks.empty())
    {
        return results;
    }
    // Type-erase the typed tasks/results into a single index-based functor so
    // the worker loop is independent of T.
    std::function<void(size_t)> runTask = [&tasks, &results](size_t i) {
        results[i] = tasks[i]();
    };
    runBatchImpl(tasks.size(), runTask);
    return results;
}
}
