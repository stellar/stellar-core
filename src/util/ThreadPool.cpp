// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/ThreadPool.h"
#include "util/GlobalChecks.h"

namespace stellar
{

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
    }
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
