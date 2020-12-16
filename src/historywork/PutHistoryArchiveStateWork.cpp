// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/PutHistoryArchiveStateWork.h"
#include "history/HistoryArchive.h"
#include "historywork/MakeRemoteDirWork.h"
#include "historywork/PutRemoteFileWork.h"
#include "main/ErrorMessages.h"
#include "util/Logging.h"
#include <Tracy.hpp>

namespace stellar
{

PutHistoryArchiveStateWork::PutHistoryArchiveStateWork(
    Application& app, HistoryArchiveState const& state,
    std::shared_ptr<HistoryArchive> archive)
    // Retry once in case of an unexpected failure during upload (e.g. deleted
    // file)
    : Work(app, "put-history-archive-state", BasicWork::RETRY_ONCE)
    , mState(state)
    , mArchive(archive)
    , mLocalFilename(HistoryArchiveState::localName(app, archive->getName()))
{
    if (!mState.containsValidBuckets(mApp))
    {
        throw std::runtime_error("Malformed HAS, unable to publish");
    }
}

void
PutHistoryArchiveStateWork::doReset()
{
    mPutRemoteFileWork.reset();
    std::remove(mLocalFilename.c_str());
}

BasicWork::State
PutHistoryArchiveStateWork::doWork()
{
    ZoneScoped;
    if (!mPutRemoteFileWork)
    {
        try
        {
            mState.save(mLocalFilename);
            spawnPublishWork();
            return State::WORK_RUNNING;
        }
        catch (std::runtime_error& e)
        {
            CLOG_ERROR(History, "Error saving history state: {}", e.what());
            CLOG_ERROR(History, "{}", POSSIBLY_CORRUPTED_LOCAL_FS);
            return State::WORK_FAILURE;
        }
    }
    else
    {
        return checkChildrenStatus();
    }
}

void
PutHistoryArchiveStateWork::spawnPublishWork()
{
    ZoneScoped;
    // Put the file in the history/ww/xx/yy/history-wwxxyyzz.json file
    auto seqName = HistoryArchiveState::remoteName(mState.currentLedger);
    auto seqDir = HistoryArchiveState::remoteDir(mState.currentLedger);

    auto w1 = std::make_shared<MakeRemoteDirWork>(mApp, seqDir, mArchive);
    auto w2 = std::make_shared<PutRemoteFileWork>(mApp, mLocalFilename, seqName,
                                                  mArchive);

    std::vector<std::shared_ptr<BasicWork>> seq{w1, w2};

    // mkdir and put inside the sequence already retry a lot
    mPutRemoteFileWork = addWork<WorkSequence>("put-history-file-sequence", seq,
                                               BasicWork::RETRY_NEVER);

    // Also put it in the .well-known/stellar-history.json file
    auto wkName = HistoryArchiveState::wellKnownRemoteName();
    auto wkDir = HistoryArchiveState::wellKnownRemoteDir();

    auto w3 = std::make_shared<MakeRemoteDirWork>(mApp, wkDir, mArchive);
    auto w4 = std::make_shared<PutRemoteFileWork>(mApp, mLocalFilename, wkName,
                                                  mArchive);

    // mkdir and put inside the sequence already retry a lot
    std::vector<std::shared_ptr<BasicWork>> seqWk{w3, w4};
    auto wellKnownPut = addWork<WorkSequence>("put-history-well-known-sequence",
                                              seqWk, BasicWork::RETRY_NEVER);
}
}
