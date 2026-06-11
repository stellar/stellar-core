// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/ThreadPool.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"

#ifdef __linux__
#include <fstream>
#include <pthread.h>
#include <sched.h>
#endif

namespace stellar
{

namespace
{
// One logical CPU per physical core: every CPU whose id is the smallest in
// its thread-siblings set. Empty on failure or on non-Linux platforms.
std::vector<int>
physicalCoreRepresentatives()
{
    std::vector<int> res;
#ifdef __linux__
    unsigned n = std::thread::hardware_concurrency();
    for (unsigned cpu = 0; cpu < n; ++cpu)
    {
        std::ifstream in("/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
                         "/topology/thread_siblings_list");
        std::string s;
        if (!(in >> s))
        {
            return {};
        }
        // The list is formatted like "0,16" or "0-1"; the first (smallest)
        // entry is the leading integer either way.
        if (atoi(s.c_str()) == static_cast<int>(cpu))
        {
            res.push_back(static_cast<int>(cpu));
        }
    }
#endif
    return res;
}
}

ThreadPool::~ThreadPool()
{
    shutdown();
}

void
ThreadPool::ensureWorkerCount(size_t count)
{
    std::lock_guard<std::mutex> guard(mMutex);
    releaseAssert(!mStopping);
    while (mWorkers.size() < count)
    {
        mWorkers.emplace_back([this]() { workerLoop(); });
        if (!mPinCpus.empty())
        {
            pinWorker(mWorkers.size() - 1);
        }
    }
}

void
ThreadPool::pinWorkersToDistinctPhysicalCores()
{
    std::lock_guard<std::mutex> guard(mMutex);
    mPinCpus = physicalCoreRepresentatives();
    if (mPinCpus.empty())
    {
        CLOG_DEBUG(Perf, "CPU topology unavailable, not pinning pool workers");
        return;
    }
    for (size_t i = 0; i < mWorkers.size(); ++i)
    {
        pinWorker(i);
    }
}

void
ThreadPool::pinWorker(size_t i)
{
#ifdef __linux__
    int cpu = mPinCpus.at(i % mPinCpus.size());
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    int rc = pthread_setaffinity_np(mWorkers.at(i).native_handle(),
                                    sizeof(set), &set);
    if (rc != 0)
    {
        CLOG_WARNING(Perf, "failed to pin pool worker {} to cpu {}: {}", i,
                     cpu, rc);
    }
#endif
}

size_t
ThreadPool::workerCount() const
{
    std::lock_guard<std::mutex> guard(mMutex);
    return mWorkers.size();
}

void
ThreadPool::shutdown()
{
    {
        std::lock_guard<std::mutex> guard(mMutex);
        if (mStopping)
        {
            return;
        }
        mStopping = true;
    }
    mCondition.notify_all();
    for (auto& worker : mWorkers)
    {
        worker.join();
    }
    mWorkers.clear();
}

void
ThreadPool::enqueue(std::function<void()> task)
{
    {
        std::lock_guard<std::mutex> guard(mMutex);
        releaseAssert(!mStopping);
        mTasks.emplace_back(std::move(task));
    }
    mCondition.notify_one();
}

void
ThreadPool::workerLoop()
{
    while (true)
    {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mMutex);
            mCondition.wait(
                lock, [this]() { return mStopping || !mTasks.empty(); });
            if (mTasks.empty())
            {
                releaseAssert(mStopping);
                return;
            }
            task = std::move(mTasks.front());
            mTasks.pop_front();
        }
        task();
    }
}
}
