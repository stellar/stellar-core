// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/CatchupRecentWork.h"
#include "catchup/CatchupCompleteWork.h"
#include "catchup/CatchupMinimalWork.h"
#include "lib/util/format.h"
#include "main/Application.h"
#include "main/Config.h"
#include "util/Logging.h"

namespace stellar
{

CatchupRecentWork::CatchupRecentWork(
    Application& app, WorkParent& parent, uint32_t initLedger,
    bool manualCatchup, CatchupWork::ProgressHandler progressHandler)
    : Work(app, parent, fmt::format("catchup-recent-{:d}-from-{:08x}",
                                    app.getConfig().CATCHUP_RECENT, initLedger))
    , mInitLedger(initLedger)
    , mManualCatchup(manualCatchup)
    , mProgressHandler(progressHandler)
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
}

Work::State
CatchupRecentWork::onSuccess()
{
    if (!mCatchupMinimalWork)
    {
        CLOG(INFO, "History")
            << "CATCHUP_RECENT starting inner CATCHUP_MINIMAL";
        mCatchupMinimalWork = addWork<CatchupMinimalWork>(
            mInitLedger, mManualCatchup,
            [](asio::error_code const&, CatchupWork::ProgressState,
               LedgerHeaderHistoryEntry const&) {});
        return WORK_PENDING;
    }

    if (!mCatchupCompleteWork)
    {
        CLOG(INFO, "History")
            << "CATCHUP_RECENT finished inner CATCHUP_MINIMAL";
        assert(mCatchupMinimalWork &&
               mCatchupMinimalWork->getState() == WORK_SUCCESS);
        mProgressHandler({}, CatchupWork::ProgressState::APPLIED_BUCKETS,
                         mCatchupMinimalWork->getFirstVerified());

        CLOG(INFO, "History")
            << "CATCHUP_RECENT starting inner CATCHUP_COMPLETE";
        // Now make a CATCHUP_COMPLETE inner worker, for replay.
        mCatchupCompleteWork = addWork<CatchupCompleteWork>(
            mInitLedger, mManualCatchup,
            [](asio::error_code const&, CatchupWork::ProgressState,
               LedgerHeaderHistoryEntry const&) {});

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
    mProgressHandler({}, CatchupWork::ProgressState::APPLIED_TRANSACTIONS,
                     mCatchupCompleteWork->getLastApplied());
    mProgressHandler({}, CatchupWork::ProgressState::FINISHED,
                     mCatchupCompleteWork->getLastApplied());
    return WORK_SUCCESS;
}

void
CatchupRecentWork::onFailureRaise()
{
    asio::error_code ec = std::make_error_code(std::errc::timed_out);
    mProgressHandler({}, CatchupWork::ProgressState::FINISHED,
                     mCatchupMinimalWork
                         ? mCatchupMinimalWork->getFirstVerified()
                         : LedgerHeaderHistoryEntry{});
}
}
