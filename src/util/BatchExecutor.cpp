// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/BatchExecutor.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"

#include <string>

#ifdef __linux__
#include <fstream>
#include <pthread.h>
#include <sched.h>
#include <unordered_set>
#endif

namespace stellar
{

namespace
{
// Returns one logical CPU id per physical core, restricted to the CPUs this
// process is actually allowed to run on. Empty on failure or on non-Linux
// platforms.
std::vector<unsigned>
physicalCoreRepresentatives()
{
    std::vector<unsigned> res;
#ifdef __linux__
    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    if (sched_getaffinity(0, sizeof(affinity), &affinity) != 0)
    {
        // Affinity unavailable (e.g. more than CPU_SETSIZE logical CPUs); skip
        // pinning rather than guess.
        return {};
    }

    // Walk the allowed CPUs in ascending order and keep the first (hence
    // smallest) allowed sibling of each physical core. thread_siblings_list
    // begins with the smallest sibling id, which is a stable per-core
    // identifier independent of the affinity mask, so it lets us collapse the
    // logical CPUs of one core down to a single representative.
    std::unordered_set<unsigned> seenCores;
    for (unsigned cpu = 0; cpu < CPU_SETSIZE; ++cpu)
    {
        if (!CPU_ISSET(cpu, &affinity))
        {
            continue;
        }
        std::ifstream in("/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
                         "/topology/thread_siblings_list");
        // The list is formatted like "0,16" or "0-1"; the first (smallest)
        // entry is the leading integer either way and identifies the core.
        unsigned coreId;
        if (!(in >> coreId))
        {
            // Topology is unreadable for a CPU we are allowed to use, so skip
            // pinning rather than guess.
            return {};
        }
        if (seenCores.insert(coreId).second)
        {
            res.push_back(cpu);
        }
    }
#endif
    return res;
}
} // namespace

BatchExecutor::BatchExecutor()
{
    mPinCpus = physicalCoreRepresentatives();
    if (mPinCpus.empty())
    {
        CLOG_WARNING(
            Perf,
            "CPU topology is unavailable, not pinning batch workers. This may "
            "reduce the performance of the node.");
    }
    else
    {
        for (size_t i = 0; i < mPinCpus.size(); ++i)
        {
            CLOG_DEBUG(Perf, "Batch worker physical core {} maps to CPU {}", i,
                       mPinCpus[i]);
        }
    }
}

BatchExecutor::~BatchExecutor()
{
    {
        std::lock_guard<std::mutex> guard(mMutex);
        mDestroying = true;
    }
    mCondition.notify_all();
    for (auto& worker : mWorkers)
    {
        worker.join();
    }
}

void
BatchExecutor::ensureWorkers(size_t count)
{
    while (mWorkers.size() < count)
    {
        size_t index = mWorkers.size();
        uint64_t batchId = mBatchId;
        mWorkers.emplace_back(
            [this, index, batchId]() { workerLoop(index, batchId); });
        pinWorker(index);
    }
}

void
BatchExecutor::pinWorker(size_t index)
{
#ifdef __linux__
    if (mPinCpus.empty())
    {
        return;
    }
    if (index >= mPinCpus.size())
    {
        CLOG_WARNING(
            Perf,
            "Not enough physical cores to pin batch executor worker {} - the "
            "network configuration demands more physical cores than available. "
            "This may reduce the performance of the node.",
            index);
    }
    unsigned cpu = mPinCpus.at(index % mPinCpus.size());
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    int rc = pthread_setaffinity_np(mWorkers.at(index).native_handle(),
                                    sizeof(set), &set);
    if (rc != 0)
    {
        CLOG_WARNING(Perf, "Failed to pin batch worker {} to CPU {}: {}", index,
                     cpu, rc);
    }
#else
    (void)index;
#endif
}

void
BatchExecutor::runBatchImpl(size_t numTasks,
                            std::function<void(size_t)> const& runTask)
{

    std::exception_ptr firstError;
    {
        std::unique_lock<std::mutex> lock(mMutex);
        ensureWorkers(numTasks);

        // Publish the batch and wake the participating workers.
        mRunTask = &runTask;
        mWorkersRequested = numTasks;
        mWorkersDone = 0;
        mFirstError = nullptr;
        ++mBatchId;
        mCondition.notify_all();

        // Block until every participating worker has finished its task.
        mCondition.wait(lock,
                        [this]() { return mWorkersDone == mWorkersRequested; });

        firstError = mFirstError;
        // Drop references to caller-owned state now that the batch is done.
        mRunTask = nullptr;
        mFirstError = nullptr;
    }
    if (firstError)
    {
        std::rethrow_exception(firstError);
    }
}

void
BatchExecutor::workerLoop(size_t workerIndex, uint64_t initBatchId)
{
    uint64_t lastBatchId = initBatchId;
    while (true)
    {
        std::function<void(size_t)> const* runTask = nullptr;
        {
            std::unique_lock<std::mutex> lock(mMutex);
            mCondition.wait(lock, [this, lastBatchId]() {
                return mDestroying || mBatchId != lastBatchId;
            });
            if (mDestroying)
            {
                return;
            }
            lastBatchId = mBatchId;
            if (workerIndex >= mWorkersRequested)
            {
                continue;
            }
            runTask = mRunTask;
        }

        // Run the task outside the lock so tasks execute in parallel.
        std::exception_ptr error;
        try
        {
            (*runTask)(workerIndex);
        }
        catch (...)
        {
            error = std::current_exception();
        }

        {
            std::lock_guard<std::mutex> guard(mMutex);
            if (error && !mFirstError)
            {
                mFirstError = error;
            }
            if (++mWorkersDone == mWorkersRequested)
            {
                mCondition.notify_all();
            }
        }
    }
}
}
