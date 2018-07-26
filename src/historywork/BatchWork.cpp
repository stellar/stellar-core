// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "BatchWork.h"
#include "catchup/CatchupManager.h"
#include "lib/util/format.h"
#include "main/Application.h"
#include "util/Logging.h"

namespace stellar
{

BatchWork::BatchWork(Application& app, WorkParent& parent, std::string name)
    : Work(app, parent, fmt::format("batch-work-" + name), RETRY_NEVER)
{
}

BatchWork::~BatchWork()
{
    clearChildren();
}

void
BatchWork::onReset()
{
    resetIter();
    clearChildren();
    mLastAssignedWork.reset();

    size_t nChildren = mApp.getConfig().MAX_CONCURRENT_SUBPROCESSES;

    bool isFirst = true;
    while (mChildren.size() < nChildren && hasNext())
    {
        handleNewWork(isFirst);
        isFirst = false;
    }
}

void
BatchWork::notify(std::string const& child)
{
    auto i = mChildren.find(child);
    if (i == mChildren.end())
    {
        CLOG(ERROR, "Work") << "BatchWork notified by unknown child " << child;
        return;
    }

    assert(i->second);
    for (auto childIt = mChildren.begin(); childIt != mChildren.end();)
    {
        if (childIt->second->getState() == WORK_SUCCESS)
        {
            CLOG(DEBUG, "History") << "Finished child work " << childIt->first;
            childIt = mChildren.erase(childIt);
            if (hasNext())
            {
                handleNewWork(false);
            }
        }
        else
        {
            ++childIt;
        }
    }

    mApp.getCatchupManager().logAndUpdateCatchupStatus(true);
    advance();
}

void
BatchWork::handleNewWork(bool isFirst)
{
    auto newWork = yieldMoreWork();

    if (!isFirst)
    {
        assert(mLastAssignedWork);
        mLastAssignedWork->registerDependent(newWork);

        // There is a possibility that previous work has already finished
        // (successfully), which means that registering a dependent will not
        // actually notify the dependent, since the work has completed.
        // In this case we manually trigger completion of the dependent work.
        if (mLastAssignedWork->getState() == Work::WORK_SUCCESS)
        {
            mLastAssignedWork->notifyCompleted();
        }
    }

    mLastAssignedWork = newWork;
}
}
