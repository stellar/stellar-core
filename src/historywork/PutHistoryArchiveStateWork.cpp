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
    Application& app, WorkParent& parent, HistoryArchiveState const& state,
    std::shared_ptr<HistoryArchive const> archive)
    : Work(app, parent, "put-history-archive-state")
    , mState(state)
    , mArchive(archive)
    , mLocalFilename(HistoryArchiveState::localName(app, archive->getName()))
{
}

PutHistoryArchiveStateWork::~PutHistoryArchiveStateWork()
{
    clearChildren();
}

void
PutHistoryArchiveStateWork::onReset()
{
    clearChildren();
    mPutRemoteFileWork.reset();
    std::remove(mLocalFilename.c_str());
}

void
PutHistoryArchiveStateWork::onRun()
{
    if (!mPutRemoteFileWork)
    {
        try
        {
            mState.save(mLocalFilename);
            scheduleSuccess();
        }
        catch (std::runtime_error& e)
        {
            CLOG(ERROR, "History")
                << "error loading history state: " << e.what();
            scheduleFailure();
        }
    }
    else
    {
        scheduleSuccess();
    }
}

Work::State
PutHistoryArchiveStateWork::onSuccess()
{
    if (!mPutRemoteFileWork)
    {
        // Put the file in the history/ww/xx/yy/history-wwxxyyzz.json file
        auto seqName = HistoryArchiveState::remoteName(mState.currentLedger);
        auto seqDir = HistoryArchiveState::remoteDir(mState.currentLedger);
        mPutRemoteFileWork =
            addWork<PutRemoteFileWork>(mLocalFilename, seqName, mArchive);
        mPutRemoteFileWork->addWork<MakeRemoteDirWork>(seqDir, mArchive);

        // Also put it in the .well-known/stellar-history.json file
        auto wkName = HistoryArchiveState::wellKnownRemoteName();
        auto wkDir = HistoryArchiveState::wellKnownRemoteDir();
        auto wkWork =
            addWork<PutRemoteFileWork>(mLocalFilename, wkName, mArchive);
        wkWork->addWork<MakeRemoteDirWork>(wkDir, mArchive);

        return WORK_PENDING;
    }
    return WORK_SUCCESS;
}
}
