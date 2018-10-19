// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/DownloadBucketsWork.h"
#include "history/FileTransferInfo.h"
#include "historywork/GetAndUnzipRemoteFileWork.h"
#include "historywork/VerifyBucketWork.h"
#include "main/Application.h"
#include <medida/meter.h>
#include <medida/metrics_registry.h>

namespace stellar
{
DownloadBucketsWork::DownloadBucketsWork(
    Application& app,
    std::map<std::string, std::shared_ptr<Bucket>>& buckets,
    std::vector<std::string> hashes, TmpDir const& downloadDir)
    : Work{app, "download-and-verify-buckets"}
    , mBuckets{buckets}
    , mHashes{std::move(hashes)}
    , mDownloadDir{downloadDir}
{
}

BasicWork::State
DownloadBucketsWork::doWork()
{
    if (!mChildrenStarted)
    {
        for (auto const& hash : mHashes)
        {
            FileTransferInfo ft(mDownloadDir, HISTORY_FILE_TYPE_BUCKET, hash);

            auto w1 = std::make_shared<GetAndUnzipRemoteFileWork>(mApp, ft);
            auto w2 = std::make_shared<VerifyBucketWork>(
                            mApp, mBuckets, ft.localPath_nogz(), hexToBin256(hash));
            std::vector<std::shared_ptr<BasicWork>> seq{w1, w2};
            addWork<WorkSequence>("download-verify-sequence-" + hash, seq);
        }
        mChildrenStarted = true;
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
DownloadBucketsWork::doReset()
{
    mChildrenStarted = false;
}
}
