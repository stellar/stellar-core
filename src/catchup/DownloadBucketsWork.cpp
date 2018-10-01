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
    Application& app, std::function<void()> callback,
    std::map<std::string, std::shared_ptr<Bucket>>& buckets,
    std::vector<std::string> hashes, TmpDir const& downloadDir)
    : Work{app, callback, "download-and-verify-buckets"}
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
            auto verify = addWork<VerifyBucketWork>(
                mBuckets, ft.localPath_nogz(), hexToBin256(hash));
            verify->addWork<GetAndUnzipRemoteFileWork>(ft);
        }
        mChildrenStarted = true;
    }
    else
    {
        if (allChildrenSuccessful())
        {
            return WORK_SUCCESS;
        }
        if (anyChildRaiseFailure() || anyChildFatalFailure())
        {
            return WORK_FAILURE_RETRY;
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
