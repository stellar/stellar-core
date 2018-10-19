// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/PutHistoryArchiveStateWork.h"
#include "history/HistoryArchive.h"
#include "historywork/MakeRemoteDirWork.h"
#include "historywork/PutRemoteFileWork.h"
#include "util/Logging.h"

namespace stellar
{
PutHistoryArchiveStateWork::PutHistoryArchiveStateWork(
    Application& app,
    HistoryArchiveState const& state, std::shared_ptr<HistoryArchive> archive)
    : Work(app, "put-history-archive-state")
    , mState(state)
    , mArchive(archive)
    , mLocalFilename(HistoryArchiveState::localName(app, archive->getName()))
{
}

void
PutHistoryArchiveStateWork::doReset()
{
    mPutRemoteFileWork.reset();
    std::remove(mLocalFilename.c_str());
}

void
PutHistoryArchiveStateWork::onFailureRetry()
{
    std::remove(mLocalFilename.c_str());
}

void
PutHistoryArchiveStateWork::onFailureRaise()
{
    std::remove(mLocalFilename.c_str());
}

BasicWork::State
PutHistoryArchiveStateWork::doWork()
{
    if (!mPutRemoteFileWork)
    {
        try
        {
            mState.save(mLocalFilename);
            spawnPublishWork();
        }
        catch (std::runtime_error& e)
        {
            CLOG(ERROR, "History")
                << "error loading history state: " << e.what();
            return WORK_FAILURE_RETRY;
        }
    }
    else
    {
        if (allChildrenSuccessful())
        {
            return WORK_SUCCESS;
        }
        else if (anyChildRaiseFailure())
        {
            return WORK_FAILURE_RETRY;
        }
        else if (!anyChildRunning())
        {
            return WORK_WAITING;
        }
    }
    return WORK_RUNNING;
}

void
PutHistoryArchiveStateWork::spawnPublishWork()
{
    // Put the file in the history/ww/xx/yy/history-wwxxyyzz.json file
    auto seqName = HistoryArchiveState::remoteName(mState.currentLedger);
    auto seqDir = HistoryArchiveState::remoteDir(mState.currentLedger);

    auto w1 = std::make_shared<MakeRemoteDirWork>(mApp, seqDir, mArchive);
    auto w2 = std::make_shared<PutRemoteFileWork>(mApp, mLocalFilename,
                                                     seqName, mArchive);

    std::vector<std::shared_ptr<BasicWork>> seq{w1, w2};
    mPutRemoteFileWork =
            addWork<WorkSequence>("put-history-file-sequence", seq);

    // Also put it in the .well-known/stellar-history.json file
    auto wkName = HistoryArchiveState::wellKnownRemoteName();
    auto wkDir = HistoryArchiveState::wellKnownRemoteDir();

    auto w3 = std::make_shared<MakeRemoteDirWork>(mApp, wkDir, mArchive);
    auto w4 = std::make_shared<PutRemoteFileWork>(mApp, mLocalFilename,
                                                     wkName, mArchive);
    std::vector<std::shared_ptr<BasicWork>> seqWk{w3, w4};
    addWork<WorkSequence>("put-history-well-known-sequence", seqWk);
}
}
