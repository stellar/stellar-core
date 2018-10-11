// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/PutSnapshotFilesWork.h"
#include "bucket/BucketManager.h"
#include "history/FileTransferInfo.h"
#include "history/StateSnapshot.h"
#include "historywork/GetHistoryArchiveStateWork.h"
#include "historywork/GzipFileWork.h"
#include "historywork/MakeRemoteDirWork.h"
#include "historywork/PutHistoryArchiveStateWork.h"
#include "historywork/PutRemoteFileWork.h"
#include "main/Application.h"
#include "util/format.h"

namespace stellar
{
PutSnapshotFilesWork::PutSnapshotFilesWork(
    Application& app, std::function<void()> callback,
    std::shared_ptr<HistoryArchive> archive,
    std::shared_ptr<StateSnapshot> snapshot)
    : Work(app, callback, "put-snapshot-files-" + archive->getName())
    , mArchive(archive)
    , mSnapshot(snapshot)
{
}

void
PutSnapshotFilesWork::doReset()
{
    mPublishSnapshot.reset();
}

BasicWork::State
PutSnapshotFilesWork::doWork()
{
    if (!mPublishSnapshot)
    {
        mPublishSnapshot = addWork<WorkSequence>("publish-snapshots-files");

        // Phase 1: fetch remote history archive state
        mPublishSnapshot->addToSequence<GetHistoryArchiveStateWork>(
            mRemoteState, 0, mArchive);

        // Phase 2: put all requisite data files
        mPublishSnapshot->addToSequence<GzipAndPutFilesWork>(
            mArchive, mSnapshot, mRemoteState);

        // Phase 3: update remote history archive state
        mPublishSnapshot->addToSequence<PutHistoryArchiveStateWork>(
            mSnapshot->mLocalState, mArchive);

        return WORK_RUNNING;
    }
    else
    {
        auto state = mPublishSnapshot->getState();
        return state == WORK_FAILURE_RAISE ? WORK_FAILURE_RETRY : state;
    }
}

GzipAndPutFilesWork::GzipAndPutFilesWork(
    Application& app, std::function<void()> callback,
    std::shared_ptr<HistoryArchive> archive,
    std::shared_ptr<StateSnapshot> snapshot,
    HistoryArchiveState const& remoteState)
    : Work(app, callback, "helper-put-files-" + archive->getName())
    , mArchive(archive)
    , mSnapshot(snapshot)
    , mRemoteState(remoteState)
{
}

BasicWork::State
GzipAndPutFilesWork::doWork()
{
    if (!mChildrenSpawned)
    {
        std::vector<std::shared_ptr<FileTransferInfo>> files = {
            mSnapshot->mLedgerSnapFile, mSnapshot->mTransactionSnapFile,
            mSnapshot->mTransactionResultSnapFile,
            mSnapshot->mSCPHistorySnapFile};

        std::vector<std::string> bucketsToSend =
            mSnapshot->mLocalState.differingBuckets(mRemoteState);

        for (auto const& hash : bucketsToSend)
        {
            auto b = mApp.getBucketManager().getBucketByHash(hexToBin256(hash));
            assert(b);
            files.push_back(std::make_shared<FileTransferInfo>(*b));
        }
        for (auto f : files)
        {
            // Empty files are removed and shouldn't be uploaded
            if (f && fs::exists(f->localPath_nogz()))
            {
                auto publish = addWork<WorkSequence>("publish-snapshots");
                publish->addToSequence<GzipFileWork>(f->localPath_nogz(), true);
                publish->addToSequence<MakeRemoteDirWork>(f->remoteDir(),
                                                          mArchive);
                publish->addToSequence<PutRemoteFileWork>(
                    f->localPath_gz(), f->remoteName(), mArchive);
            }
        }
        mChildrenSpawned = true;
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
GzipAndPutFilesWork::doReset()
{
    mChildrenSpawned = false;
}
}