// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "work/BatchWork.h"
#include "catchup/CatchupManager.h"
#include "util/Logging.h"
#include <Tracy.hpp>
#include <fmt/format.h>

namespace stellar
{

BatchWork::BatchWork(Application& app, std::string name)
    : Work(app, name, BasicWork::RETRY_NEVER)
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
    ZoneScoped;
    if (anyChildRaiseFailure())
    {
        return State::WORK_FAILURE;
    }

    // Clean up completed children
    for (auto childIt = mBatch.begin(); childIt != mBatch.end();)
    {
        if (childIt->second->getState() == State::WORK_SUCCESS)
        {
            CLOG_DEBUG(Work, "Finished child work {}", childIt->first);
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
    ZoneScoped;
    if (isAborting())
    {
        throw std::runtime_error(getName() + " is being aborted!");
    }

    int nChildren = mApp.getConfig().MAX_CONCURRENT_SUBPROCESSES;
    while (mBatch.size() < nChildren && hasNext())
    {
        auto w = yieldMoreWork();
        addWork(nullptr, w);
        if (mBatch.find(w->getName()) != mBatch.end())
        {
            throw std::runtime_error(
                "BatchWork error: inserting duplicate work!");
        }
        mBatch.insert(std::make_pair(w->getName(), w));
    }
}
}
