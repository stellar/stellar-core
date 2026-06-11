#pragma once

// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/NonCopyable.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

namespace stellar
{

// A simple thread pool with persistent worker threads.
//
// This exists primarily for the parallel transaction apply path: spawning
// fresh threads for every parallel stage (e.g. via std::async) means every
// stage starts with cold per-thread allocator caches, which serializes the
// allocation-heavy apply work on the allocator's central locks. Keeping the
// workers alive across stages and ledgers preserves their allocator caches.
//
// The pool starts with no workers and grows on demand via ensureWorkerCount;
// it never shrinks. Growth is required because the demanded parallelism can
// change at runtime (e.g. the ledgerMaxDependentTxClusters network setting
// may be increased by a protocol upgrade).
//
// Note that the worker threads are not registered in the per-application
// thread type map (which must not be modified after the application is
// constructed, while this pool grows dynamically), so they must not call
// Application::threadIsType.
class ThreadPool : private NonMovableOrCopyable
{
  public:
    ThreadPool() = default;
    ~ThreadPool();

    // Ensures that at least `count` worker threads exist, spawning more if
    // necessary.
    void ensureWorkerCount(size_t count);

    // Pin every worker (current and future) to a distinct physical CPU core,
    // one logical CPU per core (wrapping if there are more workers than
    // cores). Without this the kernel's wake-up placement can leave two
    // workers parked on hyperthread siblings of one physical core for the
    // whole life of the pool, running both at ~60% throughput while another
    // core sits idle -- and a barrier over the workers (e.g. the parallel
    // tx-apply join) is bounded by exactly those degraded workers. No-op on
    // non-Linux platforms.
    void pinWorkersToDistinctPhysicalCores();

    size_t workerCount() const;

    // Stops accepting new tasks, runs all the already queued tasks to
    // completion, and joins all the worker threads. Called automatically by
    // the destructor.
    void shutdown();

    // Schedules `f` to run on a worker thread and returns a future for its
    // result. Exceptions thrown by `f` are captured in the future and
    // rethrown from its `get()`. The caller is responsible for ensuring at
    // least one worker exists via ensureWorkerCount.
    template <typename F>
    auto
    submit(F&& f) -> std::future<std::invoke_result_t<std::decay_t<F>>>
    {
        using R = std::invoke_result_t<std::decay_t<F>>;
        // packaged_task is move-only but std::function requires copyable
        // callables, so hold it via shared_ptr.
        auto task =
            std::make_shared<std::packaged_task<R()>>(std::forward<F>(f));
        auto future = task->get_future();
        enqueue([task]() { (*task)(); });
        return future;
    }

  private:
    void enqueue(std::function<void()> task);
    void workerLoop();
    // Pin worker `i` to its designated CPU; requires mMutex and pinning
    // enabled.
    void pinWorker(size_t i);

    // Plain std::mutex (rather than the annotated wrapper) as it has to work
    // with std::condition_variable.
    mutable std::mutex mMutex;
    std::condition_variable mCondition;
    std::deque<std::function<void()>> mTasks;
    std::vector<std::thread> mWorkers;
    // One logical CPU per physical core; non-empty iff pinning is enabled.
    std::vector<int> mPinCpus;
    bool mStopping{false};
};
}
