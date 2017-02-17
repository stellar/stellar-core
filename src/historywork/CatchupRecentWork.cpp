// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/CatchupRecentWork.h"
#include "historywork/CatchupCompleteWork.h"
#include "historywork/CatchupMinimalWork.h"
#include "lib/util/format.h"
#include "main/Application.h"
#include "main/Config.h"
#include "util/Logging.h"

namespace stellar
{

CatchupRecentWork::CatchupRecentWork(Application& app, WorkParent& parent,
                                     uint32_t initLedger, bool manualCatchup,
                                     handler endHandler)
    : Work(app, parent, fmt::format("catchup-recent-{:d}-from-{:08x}",
                                    app.getConfig().CATCHUP_RECENT, initLedger))
    , mInitLedger(initLedger)
    , mManualCatchup(manualCatchup)
    , mEndHandler(endHandler)
{
}

std::string
CatchupRecentWork::getStatus() const
{
    if (mState == WORK_PENDING)
    {
        if (mCatchupCompleteWork)
        {
            return mCatchupCompleteWork->getStatus();
        }
        if (mCatchupMinimalWork)
        {
            return mCatchupMinimalWork->getStatus();
        }
    }
    return Work::getStatus();
}

void
CatchupRecentWork::onReset()
{
    clearChildren();
    mCatchupMinimalWork.reset();
    mCatchupCompleteWork.reset();
    mFirstVerified = LedgerHeaderHistoryEntry();
    mLastApplied = LedgerHeaderHistoryEntry();
}

CatchupRecentWork::handler
CatchupRecentWork::writeFirstVerified()
{
    std::weak_ptr<CatchupRecentWork> weak =
        std::static_pointer_cast<CatchupRecentWork>(shared_from_this());
    return [weak](asio::error_code const& ec, CatchupManager::CatchupMode mode,
                  LedgerHeaderHistoryEntry const& ledger) {
        auto self = weak.lock();
        if (!self)
        {
            return;
        }
        self->mFirstVerified = ledger;
    };
}

CatchupRecentWork::handler
CatchupRecentWork::writeLastApplied()
{
    std::weak_ptr<CatchupRecentWork> weak =
        std::static_pointer_cast<CatchupRecentWork>(shared_from_this());
    return [weak](asio::error_code const& ec, CatchupManager::CatchupMode mode,
                  LedgerHeaderHistoryEntry const& ledger) {
        auto self = weak.lock();
        if (!self)
        {
            return;
        }
        self->mLastApplied = ledger;
    };
}

Work::State
CatchupRecentWork::onSuccess()
{
    if (!mCatchupMinimalWork)
    {
        CLOG(INFO, "History")
            << "CATCHUP_RECENT starting inner CATCHUP_MINIMAL";
        mCatchupMinimalWork = addWork<CatchupMinimalWork>(
            mInitLedger, mManualCatchup, writeFirstVerified());
        return WORK_PENDING;
    }

    if (!mCatchupCompleteWork)
    {
        CLOG(INFO, "History")
            << "CATCHUP_RECENT finished inner CATCHUP_MINIMAL";
        assert(mCatchupMinimalWork &&
               mCatchupMinimalWork->getState() == WORK_SUCCESS);
        // We make an initial callback in mode CATCHUP_RECENT, to drive the
        // CATCHUP_MINIMAL LCL we just got through to the LM, and prepare
        // it for the upcoming CATCHUP_COMPLETE replay.
        asio::error_code ec;
        mEndHandler(ec, CatchupManager::CATCHUP_RECENT, mFirstVerified);

        CLOG(INFO, "History")
            << "CATCHUP_RECENT starting inner CATCHUP_COMPLETE";
        // Now make a CATCHUP_COMPLETE inner worker, for replay.
        mCatchupCompleteWork = addWork<CatchupCompleteWork>(
            mInitLedger, mManualCatchup, writeLastApplied());

        // Transfer the download dir used by the minimal catchup to the
        // complete catchup, to avoid re-downloading the ledger history.
        auto minimal =
            std::static_pointer_cast<CatchupMinimalWork>(mCatchupMinimalWork);
        auto complete =
            std::static_pointer_cast<CatchupCompleteWork>(mCatchupCompleteWork);
        complete->takeDownloadDir(*minimal);

        return WORK_PENDING;
    }

    assert(mCatchupMinimalWork &&
           mCatchupMinimalWork->getState() == WORK_SUCCESS);
    assert(mCatchupCompleteWork &&
           mCatchupCompleteWork->getState() == WORK_SUCCESS);

    CLOG(INFO, "History") << "CATCHUP_RECENT finished inner CATCHUP_COMPLETE";
    // The second callback we make is CATCHUP_COMPLETE
    mApp.getCatchupManager().historyCaughtup();
    asio::error_code ec;
    mEndHandler(ec, CatchupManager::CATCHUP_COMPLETE, mLastApplied);
    return WORK_SUCCESS;
}

void
CatchupRecentWork::onFailureRaise()
{
    mApp.getCatchupManager().historyCaughtup();
    asio::error_code ec = std::make_error_code(std::errc::timed_out);
    mEndHandler(ec, CatchupManager::CATCHUP_RECENT, mFirstVerified);
}
}
