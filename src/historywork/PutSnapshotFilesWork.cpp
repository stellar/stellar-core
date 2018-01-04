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

namespace stellar
{

PutSnapshotFilesWork::PutSnapshotFilesWork(
    Application& app, WorkParent& parent,
    std::shared_ptr<HistoryArchive const> archive,
    std::shared_ptr<StateSnapshot> snapshot)
    : Work(app, parent, "put-snapshot-files-" + archive->getName())
    , mArchive(archive)
    , mSnapshot(snapshot)
{
}

PutSnapshotFilesWork::~PutSnapshotFilesWork()
{
    clearChildren();
}

void
PutSnapshotFilesWork::onReset()
{
    clearChildren();

    mGetHistoryArchiveStateWork.reset();
    mPutFilesWork.reset();
    mPutHistoryArchiveStateWork.reset();
}

Work::State
PutSnapshotFilesWork::onSuccess()
{
    // Phase 1: fetch remote history archive state
    if (!mGetHistoryArchiveStateWork)
    {
        mGetHistoryArchiveStateWork = addWork<GetHistoryArchiveStateWork>(
            "get-history-archive-state", mRemoteState, 0,
            std::chrono::seconds(0), mArchive);
        return WORK_PENDING;
    }

    // Phase 2: put all requisite data files
    if (!mPutFilesWork)
    {
        mPutFilesWork = addWork<Work>("put-files");

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
            if (f && fs::exists(f->localPath_nogz()))
            {
                auto put = mPutFilesWork->addWork<PutRemoteFileWork>(
                    f->localPath_gz(), f->remoteName(), mArchive);
                auto mkdir =
                    put->addWork<MakeRemoteDirWork>(f->remoteDir(), mArchive);
                mkdir->addWork<GzipFileWork>(f->localPath_nogz(), true);
            }
        }
        return WORK_PENDING;
    }

    // Phase 3: update remote history archive state
    if (!mPutHistoryArchiveStateWork)
    {
        mPutHistoryArchiveStateWork = addWork<PutHistoryArchiveStateWork>(
            mSnapshot->mLocalState, mArchive);
        return WORK_PENDING;
    }

    return WORK_SUCCESS;
}
}
