// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/RepairMissingBucketsWork.h"
#include "bucket/BucketManager.h"
#include "history/FileTransferInfo.h"
#include "history/HistoryManager.h"
#include "historywork/GetAndUnzipRemoteFileWork.h"
#include "historywork/VerifyBucketWork.h"
#include "main/Application.h"
#include "util/TmpDir.h"
#include "work/WorkSequence.h"

namespace stellar
{

RepairMissingBucketsWork::RepairMissingBucketsWork(
    Application& app, HistoryArchiveState const& localState, Handler endHandler)
    : Work(app, "repair-buckets", BasicWork::RETRY_NEVER)
    , mEndHandler(endHandler)
    , mLocalState(localState)
    , mDownloadDir(std::make_unique<TmpDir>(
          mApp.getTmpDirManager().tmpDir("repair-buckets")))
{
}

void
RepairMissingBucketsWork::doReset()
{
    mChildrenStarted = false;
}

BasicWork::State
RepairMissingBucketsWork::doWork()
{
    if (!mChildrenStarted)
    {
        std::unordered_set<std::string> bucketsToFetch;
        auto missingBuckets =
            mApp.getBucketManager().checkForMissingBucketsFiles(mLocalState);
        auto publishBuckets = mApp.getHistoryManager()
                                  .getMissingBucketsReferencedByPublishQueue();

        bucketsToFetch.insert(missingBuckets.begin(), missingBuckets.end());
        bucketsToFetch.insert(publishBuckets.begin(), publishBuckets.end());

        for (auto const& hash : bucketsToFetch)
        {
            FileTransferInfo ft(*mDownloadDir, HISTORY_FILE_TYPE_BUCKET, hash);

            // Each bucket gets its own work-sequence of
            // download->gunzip->verify
            auto download =
                std::make_shared<GetAndUnzipRemoteFileWork>(mApp, ft);
            auto verify = std::make_shared<VerifyBucketWork>(
                mApp, mBuckets, ft.localPath_nogz(), hexToBin256(hash));
            std::vector<std::shared_ptr<BasicWork>> seq{download, verify};

            addWork<WorkSequence>("repair-bucket-" + hash, seq);
        }
        mChildrenStarted = true;
        return State::WORK_RUNNING;
    }
    else
    {
        return WorkUtils::checkChildrenStatus(*this);
    }
}

void
RepairMissingBucketsWork::onSuccess()
{
    asio::error_code ec;
    mEndHandler(ec);
}

void
RepairMissingBucketsWork::onFailureRaise()
{
    Work::onFailureRaise();
    asio::error_code ec = std::make_error_code(std::errc::io_error);
    mEndHandler(ec);
}
}
