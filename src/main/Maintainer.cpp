// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Maintainer.h"
#include "main/Config.h"
#include "main/ExternalQueue.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/numeric.h"
#include <Tracy.hpp>
#include <fmt/format.h>

namespace stellar
{

Maintainer::Maintainer(Application& app) : mApp{app}, mTimer{mApp}
{
}

void
Maintainer::start()
{
    ZoneScoped;
    auto& c = mApp.getConfig();
    if (c.AUTOMATIC_MAINTENANCE_PERIOD.count() > 0 &&
        c.AUTOMATIC_MAINTENANCE_COUNT > 0)
    {
        // compare number of ledgers deleted per maintenance cycle with actual
        // number
        int64 ledgersPerMaintenancePeriod = bigDivide(
            c.AUTOMATIC_MAINTENANCE_PERIOD.count(), 1,
            c.getExpectedLedgerCloseTime().count(), Rounding::ROUND_UP);
        if (c.AUTOMATIC_MAINTENANCE_COUNT <= ledgersPerMaintenancePeriod)
        {
            LOG(WARNING) << fmt::format(
                "Maintenance may not be able to keep up: "
                "AUTOMATIC_MAINTENANCE_COUNT={} <= {}",
                c.AUTOMATIC_MAINTENANCE_COUNT, ledgersPerMaintenancePeriod);
        }
        scheduleMaintenance();
    }
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
    ZoneScoped;
    performMaintenance(mApp.getConfig().AUTOMATIC_MAINTENANCE_COUNT);
    scheduleMaintenance();
}

void
Maintainer::performMaintenance(uint32_t count)
{
    ZoneScoped;
    LOG(INFO) << "Performing maintenance";
    ExternalQueue ps{mApp};
    ps.deleteOldEntries(count);
}
}
