// Copyright 2014-2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"
#include "catchup/CatchupManagerImpl.h"
#include "catchup/CatchupConfiguration.h"
#include "catchup/CatchupWork.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "util/Logging.h"
#include "util/StatusManager.h"
#include "util/format.h"
#include "util/make_unique.h"
#include "work/WorkManager.h"

namespace stellar
{

std::unique_ptr<CatchupManager>
CatchupManager::create(Application& app)
{
    return make_unique<CatchupManagerImpl>(app);
}

CatchupManagerImpl::CatchupManagerImpl(Application& app)
    : mApp(app)
    , mCatchupWork(nullptr)
    , mCatchupStart(
          app.getMetrics().NewMeter({"history", "catchup", "start"}, "event"))
    , mCatchupSuccess(
          app.getMetrics().NewMeter({"history", "catchup", "success"}, "event"))
    , mCatchupFailure(
          app.getMetrics().NewMeter({"history", "catchup", "failure"}, "event"))
{
}

CatchupManagerImpl::~CatchupManagerImpl()
{
}

void
CatchupManagerImpl::historyCaughtup()
{
    mCatchupWork.reset();
}

void
CatchupManagerImpl::catchupHistory(CatchupConfiguration catchupConfiguration,
                                   bool manualCatchup,
                                   CatchupWork::ProgressHandler handler)
{
    if (mCatchupWork)
    {
        throw std::runtime_error("Catchup already in progress");
    }

    mCatchupStart.Mark();

    mCatchupWork = mApp.getWorkManager().addWork<CatchupWork>(
        catchupConfiguration, manualCatchup, handler, Work::RETRY_NEVER);
    mApp.getWorkManager().advanceChildren();
}

std::string
CatchupManagerImpl::getStatus() const
{
    return mCatchupWork ? mCatchupWork->getStatus() : std::string{};
}

uint64_t
CatchupManagerImpl::getCatchupStartCount() const
{
    return mCatchupStart.count();
}

uint64_t
CatchupManagerImpl::getCatchupSuccessCount() const
{
    return mCatchupSuccess.count();
}

uint64_t
CatchupManagerImpl::getCatchupFailureCount() const
{
    return mCatchupFailure.count();
}

void
CatchupManagerImpl::logAndUpdateCatchupStatus(bool contiguous)
{
    auto catchupStatus = getStatus();

    if (!catchupStatus.empty())
    {
        auto contiguousString =
            contiguous ? "" : " (discontiguous; will fail and restart)";
        auto state =
            fmt::format("Catching up{}: {}", contiguousString, catchupStatus);
        auto existing = mApp.getStatusManager().getStatusMessage(
            StatusCategory::HISTORY_CATCHUP);
        if (existing != state)
        {
            CLOG(INFO, "History") << state;
            mApp.getStatusManager().setStatusMessage(
                StatusCategory::HISTORY_CATCHUP, state);
        }
    }
    else
    {
        mApp.getStatusManager().removeStatusMessage(
            StatusCategory::HISTORY_CATCHUP);
    }
}
}
