// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Maintainer.h"
#include "main/Config.h"
#include "main/ExternalQueue.h"
#include "util/Logging.h"

namespace stellar
{

Maintainer::Maintainer(Application& app) : mApp{app}, mTimer{mApp}
{
}

void
Maintainer::start()
{
    if (mApp.getConfig().AUTOMATIC_MAINTENANCE_PERIOD.count() > 0 &&
        mApp.getConfig().AUTOMATIC_MAINTENANCE_COUNT > 0)
        scheduleMaintenance();
}

void
Maintainer::scheduleMaintenance()
{
    mTimer.expires_from_now(mApp.getConfig().AUTOMATIC_MAINTENANCE_PERIOD);
    mTimer.async_wait([this]() { tick(); }, VirtualTimer::onFailureNoop);
}

void
Maintainer::tick()
{
    performMaintenance(mApp.getConfig().AUTOMATIC_MAINTENANCE_COUNT);
    scheduleMaintenance();
}

void
Maintainer::performMaintenance(uint32_t count)
{
    LOG(INFO) << "Performing maintenance";
    ExternalQueue ps{mApp};
    ps.deleteOldEntries(count);
}
}
