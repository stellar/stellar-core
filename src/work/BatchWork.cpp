// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "work/BatchWork.h"
#include "util/Logging.h"

namespace stellar
{

BatchWork::BatchWork(Application& app, std::string name)
    : Work(app, name, RETRY_NEVER)
{
}

void
BatchWork::doReset()
{
    mBatch.clear();
    resetIter();
}

BasicWork::State
BatchWork::doWork()
{
    if (anyChildRaiseFailure())
    {
        return State::WORK_FAILURE;
    }

    // Clean up completed children
    for (auto childIt = mBatch.begin(); childIt != mBatch.end();)
    {
        if (childIt->second->getState() == State::WORK_SUCCESS)
        {
            CLOG(DEBUG, "Work") << "Finished child work " << childIt->first;
            childIt = mBatch.erase(childIt);
        }
        else
        {
            ++childIt;
        }
    }

    addMoreWorkIfNeeded();

    if (allChildrenSuccessful())
    {
        return State::WORK_SUCCESS;
    }

    if (!anyChildRunning())
    {
        return State::WORK_WAITING;
    }

    return State::WORK_RUNNING;
}

void
BatchWork::addMoreWorkIfNeeded()
{
    size_t nChildren = mApp.getConfig().MAX_CONCURRENT_SUBPROCESSES;
    while (mBatch.size() < nChildren && hasNext())
    {
        auto w = yieldMoreWork();
        if (mBatch.find(w->getName()) != mBatch.end())
        {
            throw std::runtime_error(
                "BatchWork error: inserting duplicate work!");
        }
        mBatch.insert(std::make_pair(w->getName(), w));
    }
}
}
