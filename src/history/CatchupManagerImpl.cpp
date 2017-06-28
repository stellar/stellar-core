// Copyright 2014-2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"
#include "history/CatchupManagerImpl.h"
#include "historywork/CatchupCompleteImmediateWork.h"
#include "historywork/CatchupCompleteWork.h"
#include "historywork/CatchupMinimalWork.h"
#include "historywork/CatchupRecentWork.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "util/Logging.h"
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
CatchupManagerImpl::catchupHistory(
    uint32_t initLedger, CatchupMode mode,
    std::function<void(asio::error_code const& ec, CatchupMode mode,
                       LedgerHeaderHistoryEntry const& lastClosed)>
        handler,
    bool manualCatchup)
{
    if (mCatchupWork)
    {
        throw std::runtime_error("Catchup already in progress");
    }

    mCatchupStart.Mark();

    // Avoid CATCHUP_RECENT if it's going to actually try to revert
    // us to an earlier state of the ledger than the LCL; in that case
    // we're close enough to the network to just run CATCHUP_COMPLETE.
    auto lcl = mApp.getLedgerManager().getLastClosedLedgerHeader();
    if (mode == CATCHUP_RECENT && (initLedger > lcl.header.ledgerSeq) &&
        (initLedger - lcl.header.ledgerSeq) <= mApp.getConfig().CATCHUP_RECENT)
    {
        mode = CatchupManager::CATCHUP_COMPLETE;
    }

    if (mode == CATCHUP_MINIMAL)
    {
        CLOG(INFO, "History") << "Starting CatchupMinimalWork";
        mCatchupWork = mApp.getWorkManager().addWork<CatchupMinimalWork>(
            initLedger, manualCatchup, handler);
    }
    else if (mode == CATCHUP_RECENT)
    {
        CLOG(INFO, "History") << "Starting CatchupRecentWork";
        mCatchupWork = mApp.getWorkManager().addWork<CatchupRecentWork>(
            initLedger, manualCatchup, handler);
    }
    else if (mode == CATCHUP_COMPLETE)
    {
        CLOG(INFO, "History") << "Starting CatchupCompleteWork";
        mCatchupWork = mApp.getWorkManager().addWork<CatchupCompleteWork>(
            initLedger, manualCatchup, handler);
    }
    else if (mode == CATCHUP_COMPLETE_IMMEDIATE)
    {
        CLOG(INFO, "History") << "Starting CatchupCompleteImmediateWork";
        mCatchupWork =
            mApp.getWorkManager().addWork<CatchupCompleteImmediateWork>(
                initLedger, manualCatchup, handler);
    }
    else
    {
        assert(false);
    }
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
}
