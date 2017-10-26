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

namespace stellar
{

RepairMissingBucketsWork::RepairMissingBucketsWork(
    Application& app, WorkParent& parent, HistoryArchiveState const& localState,
    handler endHandler)
    : BucketDownloadWork(app, parent, "repair-buckets", localState)
    , mEndHandler(endHandler)
{
}

RepairMissingBucketsWork::~RepairMissingBucketsWork()
{
    clearChildren();
}

void
RepairMissingBucketsWork::onReset()
{
    BucketDownloadWork::onReset();
    std::unordered_set<std::string> bucketsToFetch;
    auto missingBuckets =
        mApp.getBucketManager().checkForMissingBucketsFiles(mLocalState);
    auto publishBuckets =
        mApp.getHistoryManager().getMissingBucketsReferencedByPublishQueue();

    bucketsToFetch.insert(missingBuckets.begin(), missingBuckets.end());
    bucketsToFetch.insert(publishBuckets.begin(), publishBuckets.end());

    for (auto const& hash : bucketsToFetch)
    {
        FileTransferInfo ft(*mDownloadDir, HISTORY_FILE_TYPE_BUCKET, hash);
        // Each bucket gets its own work-chain of download->gunzip->verify
        auto verify = addWork<VerifyBucketWork>(mBuckets, ft.localPath_nogz(),
                                                hexToBin256(hash));
        verify->addWork<GetAndUnzipRemoteFileWork>(ft);
    }
}

Work::State
RepairMissingBucketsWork::onSuccess()
{
    asio::error_code ec;
    mEndHandler(ec);
    return WORK_SUCCESS;
}

void
RepairMissingBucketsWork::onFailureRaise()
{
    asio::error_code ec = std::make_error_code(std::errc::io_error);
    mEndHandler(ec);
}
}
