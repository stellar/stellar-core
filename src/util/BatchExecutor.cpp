// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/BatchExecutor.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"

#include <string>

#ifdef __linux__
#include <algorithm>
#include <fstream>
#include <map>
#include <pthread.h>
#include <sched.h>
#endif

namespace stellar
{

namespace
{
struct CpuPinning
{
    // Allowed logical CPUs in pinning-preference order.
    std::vector<unsigned> order;
    // Number of distinct physical cores found in the allowed logical CPUs.
    // First `physicalCoreCount` entries of `order` are guaranteed to be on
    // distinct physical cores by the pinning order assignment procedure.
    size_t physicalCoreCount{0};
};

// Returns every logical CPU this process is allowed to run on, ordered by
// pinning preference:
//   - One logical CPU per physical core in ascending order, but with the core
//     hosting logical CPU 0 shifted to be the last core in the order (as the OS
//     tends to concentrate housekeeping work on CPU 0)
//   - The remaining hyper-thread siblings follow, one tier of siblings at a
//     time in the same order as the physical core order above
//   - Within the CPU-0 core, CPU 0 itself is demoted behind its siblings, so it
//     ends up as the very last CPU we pin.
// Empty on failure or on non-Linux platforms.
CpuPinning
pinnedCpuOrder()
{
#ifdef __linux__
    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    if (sched_getaffinity(0, sizeof(affinity), &affinity) != 0)
    {
        // Affinity unavailable (e.g. more than CPU_SETSIZE logical CPUs); skip
        // pinning rather than guess.
        return {};
    }

    std::map<unsigned, std::vector<unsigned>> cpusByCore;
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
        cpusByCore[coreId].push_back(cpu);
    }

    // Order the cores with the CPU-0-hosting core last.
    std::vector<std::vector<unsigned> const*> coreOrder;
    coreOrder.reserve(cpusByCore.size());
    size_t maxSiblings = 0;
    for (auto const& [coreId, cpus] : cpusByCore)
    {
        if (coreId != 0)
        {
            coreOrder.push_back(&cpus);
        }
        maxSiblings = std::max(maxSiblings, cpus.size());
    }
    auto core0 = cpusByCore.find(0);
    if (core0 != cpusByCore.end())
    {
        // Demote CPU 0 behind its own hyper-thread siblings so it becomes the
        // very last CPU we pin.
        auto& core0Cpus = core0->second;
        releaseAssert(!core0Cpus.empty());
        std::rotate(core0Cpus.begin(), core0Cpus.begin() + 1, core0Cpus.end());
        coreOrder.push_back(&core0Cpus);
    }

    // Emit the CPUs tier by tier: the first sibling of every core, then the
    // second siblings, and so on.
    std::vector<unsigned> res;
    for (size_t rank = 0; rank < maxSiblings; ++rank)
    {
        for (auto const* cpus : coreOrder)
        {
            if (rank < cpus->size())
            {
                res.push_back((*cpus)[rank]);
            }
        }
    }
    return {std::move(res), cpusByCore.size()};
#else
    return {};
#endif
}
} // namespace

BatchExecutor::BatchExecutor()
{
    auto pinning = pinnedCpuOrder();
    mPinCpuOrder = std::move(pinning.order);
    mPhysicalCoreCount = pinning.physicalCoreCount;
    if (mPinCpuOrder.empty())
    {
        CLOG_WARNING(
            Perf,
            "CPU topology is unavailable, not pinning batch workers. This may "
            "reduce the performance of the node.");
    }
    else
    {
        for (size_t i = 0; i < mPinCpuOrder.size(); ++i)
        {
            CLOG_DEBUG(Perf, "Batch worker {} pins to CPU {}", i,
                       mPinCpuOrder[i]);
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
    if (mPinCpuOrder.empty())
    {
        return;
    }
    if (index >= mPinCpuOrder.size())
    {
        CLOG_WARNING(
            Perf,
            "Not enough logical CPU cores to pin batch executor worker {} - "
            "the network configuration demands more cores than available. "
            "This may significantly reduce the performance of the node.",
            index);
        return;
    }
    if (index >= mPhysicalCoreCount)
    {
        CLOG_WARNING(
            Perf,
            "Not enough physical CPU cores to pin batch executor "
            "worker {} - the network configuration demands more physical "
            "cores than available. Using hyper-thread sibling "
            "instead. This may reduce the performance of the node.",
            index);
    }
    unsigned cpu = mPinCpuOrder.at(index);
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
