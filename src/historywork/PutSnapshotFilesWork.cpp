// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/PutSnapshotFilesWork.h"
#include "bucket/BucketManager.h"
#include "history/FileTransferInfo.h"
#include "history/HistoryArchiveManager.h"
#include "history/StateSnapshot.h"
#include "historywork/GetHistoryArchiveStateWork.h"
#include "historywork/GzipFileWork.h"
#include "historywork/MakeRemoteDirWork.h"
#include "historywork/PutHistoryArchiveStateWork.h"
#include "historywork/PutRemoteFileWork.h"
#include "main/Application.h"
#include <util/format.h>

namespace stellar
{

PutSnapshotFilesWork::PutSnapshotFilesWork(
    Application& app, std::shared_ptr<StateSnapshot> snapshot)
    : Work(app, fmt::format("update-archives-{:08x}",
                            snapshot->mLocalState.currentLedger))
    , mSnapshot(snapshot)
{
}

BasicWork::State
PutSnapshotFilesWork::doWork()
{
    if (!mStarted)
    {
        for (auto& writableArchive :
             mApp.getHistoryArchiveManager().getWritableHistoryArchives())
        {
            // Phase 1: fetch remote history archive state
            auto getState = std::make_shared<GetHistoryArchiveStateWork>(
                mApp, mRemoteState, 0, writableArchive);

            // Phase 2: put all requisite data files
            auto putFiles = std::make_shared<GzipAndPutFilesWork>(
                mApp, writableArchive, mSnapshot, mRemoteState);

            // Phase 3: update remote history archive state
            auto putState = std::make_shared<PutHistoryArchiveStateWork>(
                mApp, mSnapshot->mLocalState, writableArchive);

            std::vector<std::shared_ptr<BasicWork>> seq{getState, putFiles,
                                                        putState};

            addWork<WorkSequence>("put-snapshot-sequence", seq);
        }
        mStarted = true;
    }
    else
    {
        if (allChildrenDone())
        {
            // Some children might fail, but at least the rest of the archives
            // are updated
            if (!allChildrenSuccessful())
            {
                return State::WORK_FAILURE;
            }
            return State::WORK_SUCCESS;
        }
        else if (!anyChildRunning())
        {
            return State::WORK_WAITING;
        }
    }
    return State::WORK_RUNNING;
}

void
PutSnapshotFilesWork::doReset()
{
    mStarted = false;
}

GzipAndPutFilesWork::GzipAndPutFilesWork(
    Application& app, std::shared_ptr<HistoryArchive> archive,
    std::shared_ptr<StateSnapshot> snapshot,
    HistoryArchiveState const& remoteState)
    : Work(app, "helper-put-files-" + archive->getName())
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
            // If files are empty, they are removed and shouldn't be uploaded
            if (f && fs::exists(f->localPath_nogz()))
            {
                auto gzipFile = std::make_shared<GzipFileWork>(
                    mApp, f->localPath_nogz(), true);
                auto mkdir = std::make_shared<MakeRemoteDirWork>(
                    mApp, f->remoteDir(), mArchive);
                auto putFile = std::make_shared<PutRemoteFileWork>(
                    mApp, f->localPath_gz(), f->remoteName(), mArchive);

                std::vector<std::shared_ptr<BasicWork>> seq{gzipFile, mkdir,
                                                            putFile};

                // Inherit from work sequence to do cleanup
                addWork<WorkSequence>("gzip-and-put-file-" + f->localPath_gz(),
                                      seq);
            }
        }
        mChildrenSpawned = true;
    }
    else
    {
        if (allChildrenSuccessful())
        {
            return State::WORK_SUCCESS;
        }
        else if (anyChildRaiseFailure())
        {
            return State::WORK_FAILURE;
        }
        else if (!anyChildRunning())
        {
            return State::WORK_WAITING;
        }
    }
    return State::WORK_RUNNING;
}

void
GzipAndPutFilesWork::doReset()
{
    mChildrenSpawned = false;
}
}
