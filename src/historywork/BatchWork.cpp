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
    addMoreWorkIfNeeded();
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

    for (auto childIt = mChildren.begin(); childIt != mChildren.end();)
    {
        if (childIt->second->getState() == WORK_SUCCESS)
        {
            CLOG(DEBUG, "History") << "Finished child work " << childIt->first;
            childIt = mChildren.erase(childIt);
        }
        else
        {
            ++childIt;
        }
    }

    addMoreWorkIfNeeded();
    mApp.getCatchupManager().logAndUpdateCatchupStatus(true);
    advance();
}

void
BatchWork::addMoreWorkIfNeeded()
{
    size_t nChildren = mApp.getConfig().MAX_CONCURRENT_SUBPROCESSES;
    while (mChildren.size() < nChildren && hasNext())
    {
        yieldMoreWork();
    }
}
}
