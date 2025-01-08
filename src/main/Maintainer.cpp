// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Maintainer.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "main/Config.h"
#include "util/GlobalChecks.h"
#include "util/LogSlowExecution.h"
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
        int64 ledgersPerMaintenancePeriod = bigDivideOrThrow(
            c.AUTOMATIC_MAINTENANCE_PERIOD.count(), 1,
            c.getExpectedLedgerCloseTime().count(), Rounding::ROUND_UP);
        if (c.AUTOMATIC_MAINTENANCE_COUNT <= ledgersPerMaintenancePeriod)
        {
            LOG_WARNING(
                DEFAULT_LOG, "{}",
                fmt::format(
                    FMT_STRING("Maintenance may not be able to keep up: "
                               "AUTOMATIC_MAINTENANCE_COUNT={:d} <= {:d}"),
                    c.AUTOMATIC_MAINTENANCE_COUNT,
                    ledgersPerMaintenancePeriod));
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
    LOG_INFO(DEFAULT_LOG, "Performing maintenance");
    auto logSlow = LogSlowExecution(
        "Performing maintenance", LogSlowExecution::Mode::AUTOMATIC_RAII,
        "performance issue: check database or perform a large manual "
        "maintenance followed by database maintenance. Maintenance took",
        std::chrono::seconds{2});

    // Calculate the minimum of the LCL and/or any queued checkpoint.
    uint32_t lcl = mApp.getLedgerManager().getLastClosedLedgerNum();
    uint32_t ql = HistoryManager::getMinLedgerQueuedToPublish(mApp.getConfig());
    uint32_t qmin = ql == 0 ? lcl : std::min(ql, lcl);

    // Next calculate, given qmin, the first ledger it'd be _safe to
    // delete_ while still keeping everything required to publish.
    // So if qmin is (for example) 0x7f = 127, then we want to keep 64
    // ledgers before that, and therefore can erase 0x3f = 63 and less.
    uint32_t freq = HistoryManager::getCheckpointFrequency(mApp.getConfig());
    uint32_t lmin = qmin >= freq ? qmin - freq : 0;

    CLOG_INFO(History, "Trimming history <= ledger {}", lmin);

    mApp.getLedgerManager().deleteOldEntries(mApp.getDatabase(), lmin, count);
}
}
