// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "GzipAndPutFilesWork.h"
#include "bucket/BucketManager.h"
#include "historywork/GzipFileWork.h"
#include "historywork/MakeRemoteDirWork.h"
#include "historywork/PutRemoteFileWork.h"
#include "work/WorkSequence.h"

namespace stellar
{

GzipAndPutFilesWork::GzipAndPutFilesWork(
    Application& app, std::shared_ptr<HistoryArchive> archive,
    std::shared_ptr<StateSnapshot> snapshot,
    HistoryArchiveState const& remoteState)
    // Each gzip-and-put-file sequence will retry correctly
    : Work(app, "helper-put-files-" + archive->getName(),
           BasicWork::RETRY_NEVER)
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
        for (auto const& f : files)
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
                // Each inner step will retry a lot, so retry the sequence once
                // in case of an unexpected failure
                addWork<WorkSequence>("gzip-and-put-file-" + f->localPath_gz(),
                                      seq, BasicWork::RETRY_ONCE);
            }
        }
        mChildrenSpawned = true;
    }
    else
    {
        return WorkUtils::checkChildrenStatus(*this);
    }
    return State::WORK_RUNNING;
}

void
GzipAndPutFilesWork::doReset()
{
    mChildrenSpawned = false;
}
}
