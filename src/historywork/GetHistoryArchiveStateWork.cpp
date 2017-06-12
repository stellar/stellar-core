// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/GetHistoryArchiveStateWork.h"
#include "history/HistoryArchive.h"
#include "historywork/ApplyLedgerChainWork.h"
#include "historywork/GetRemoteFileWork.h"
#include "ledger/LedgerManager.h"
#include "lib/util/format.h"
#include "main/Application.h"
#include "util/Logging.h"

namespace stellar
{

GetHistoryArchiveStateWork::GetHistoryArchiveStateWork(
    Application& app, WorkParent& parent, HistoryArchiveState& state,
    uint32_t seq, VirtualClock::duration const& initialDelay,
    std::shared_ptr<HistoryArchive const> archive, size_t maxRetries)
    : Work(app, parent, "get-history-archive-state", maxRetries)
    , mState(state)
    , mSeq(seq)
    , mInitialDelay(initialDelay)
    , mArchive(archive)
    , mLocalFilename(
          archive ? HistoryArchiveState::localName(app, archive->getName())
                  : app.getHistoryManager().localFilename(
                        HistoryArchiveState::baseName()))
{
}

std::string
GetHistoryArchiveStateWork::getStatus() const
{
    if (getState() == WORK_FAILURE_RETRY)
    {
        auto eta = getRetryETA();
        return fmt::format("Awaiting checkpoint (ETA: {:d} seconds)", eta);
    }
    return Work::getStatus();
}

VirtualClock::duration
GetHistoryArchiveStateWork::getRetryDelay() const
{
    if (mInitialDelay.count() != 0 && mRetries == 0)
    {
        return mInitialDelay;
    }
    return Work::getRetryDelay();
}

void
GetHistoryArchiveStateWork::onReset()
{
    clearChildren();
    std::remove(mLocalFilename.c_str());
    addWork<GetRemoteFileWork>(mSeq == 0
                                   ? HistoryArchiveState::wellKnownRemoteName()
                                   : HistoryArchiveState::remoteName(mSeq),
                               mLocalFilename, mArchive, getMaxRetries());

    if (mSeq != 0 && mRetries == 0 && mInitialDelay.count() != 0)
    {
        // If this is our first reset (on addition) and we're fetching a
        // known snapshot, immediately initiate a timed retry, to avoid
        // cluttering the console with the initial-probe failure.
        setState(WORK_FAILURE_RETRY);
        scheduleRetry();
    }
}

void
GetHistoryArchiveStateWork::onRun()
{
    try
    {
        mState.load(mLocalFilename);
        scheduleSuccess();
    }
    catch (std::runtime_error& e)
    {
        CLOG(ERROR, "History") << "error loading history state: " << e.what();
        scheduleFailure();
    }
}
}
