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
    Application& app, WorkParent& parent,
    std::map<std::string, std::shared_ptr<Bucket>>& buckets,
    std::vector<std::string> hashes, TmpDir const& downloadDir)
    : Work{app, parent, "download-and-verify-buckets"}
    , mBuckets{buckets}
    , mHashes{std::move(hashes)}
    , mDownloadDir{downloadDir}
    , mDownloadBucketStart{app.getMetrics().NewMeter(
          {"history", "download-bucket", "start"}, "event")}
    , mDownloadBucketSuccess{app.getMetrics().NewMeter(
          {"history", "download-bucket", "success"}, "event")}
    , mDownloadBucketFailure{app.getMetrics().NewMeter(
          {"history", "download-bucket", "failure"}, "event")}
{
}

DownloadBucketsWork::~DownloadBucketsWork()
{
    clearChildren();
}

std::string
DownloadBucketsWork::getStatus() const
{
    if (mState == WORK_RUNNING || mState == WORK_PENDING)
    {
        return "downloading buckets";
    }
    return Work::getStatus();
}

void
DownloadBucketsWork::onReset()
{
    clearChildren();

    for (auto const& hash : mHashes)
    {
        FileTransferInfo ft(mDownloadDir, HISTORY_FILE_TYPE_BUCKET, hash);
        // Each bucket gets its own work-chain of
        // download->gunzip->verify

        auto verify = addWork<VerifyBucketWork>(mBuckets, ft.localPath_nogz(),
                                                hexToBin256(hash));
        verify->addWork<GetAndUnzipRemoteFileWork>(ft);
        mDownloadBucketStart.Mark();
    }
}

void
DownloadBucketsWork::notify(std::string const& child)
{
    auto i = mChildren.find(child);
    if (i == mChildren.end())
    {
        CLOG(WARNING, "Work")
            << "DownloadBucketsWork notified by unknown child " << child;
        return;
    }

    switch (i->second->getState())
    {
    case Work::WORK_SUCCESS:
        mDownloadBucketSuccess.Mark();
        break;
    case Work::WORK_FAILURE_RETRY:
    case Work::WORK_FAILURE_FATAL:
    case Work::WORK_FAILURE_RAISE:
        mDownloadBucketFailure.Mark();
        break;
    default:
        break;
    }

    advance();
}
}
