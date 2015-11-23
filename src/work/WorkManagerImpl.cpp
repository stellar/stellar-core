// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "work/Work.h"
#include "work/WorkParent.h"
#include "work/WorkManager.h"
#include "work/WorkManagerImpl.h"

#include "lib/util/format.h"
#include "util/Logging.h"
#include "util/make_unique.h"

#include "medida/meter.h"
#include "medida/metrics_registry.h"

namespace stellar
{

WorkManager::WorkManager(Application& app)
    : WorkParent(app)
{
}

WorkManagerImpl::WorkManagerImpl(Application& app)
    : WorkManager(app)
{
}

void
WorkManagerImpl::notify(std::string const& child)
{
    auto i = mChildren.find(child);
    if (i == mChildren.end())
    {
        CLOG(WARNING, "Work") << "WorkManager notified by unknown child "
                              << child;
        return;
    }

    if (i->second->getState() == Work::WORK_SUCCESS)
    {
        CLOG(INFO, "Work") << "WorkManager got SUCCESS from " << child;
        mApp.getMetrics().NewMeter({"work", "root", "success"}, "unit").Mark();
        mChildren.erase(child);
    }

    if (i->second->getState() == Work::WORK_FAILURE_RAISE)
    {
        CLOG(WARNING, "Work") << "WorkManager got FAILURE_RAISE from " << child;
        mApp.getMetrics().NewMeter({"work", "root", "failure"}, "unit").Mark();
        mChildren.erase(child);
    }
    advanceChildren();
}

std::unique_ptr<WorkManager>
WorkManager::create(Application& app)
{
    return make_unique<WorkManagerImpl>(app);
}

}
