// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/DownloadBucketsWork.h"
#include "catchup/CatchupManager.h"
#include "history/FileTransferInfo.h"
#include "history/HistoryArchive.h"
#include "historywork/GetAndUnzipRemoteFileWork.h"
#include "historywork/VerifyBucketWork.h"
#include <Tracy.hpp>
#include <fmt/format.h>

namespace stellar
{

DownloadBucketsWork::DownloadBucketsWork(
    Application& app, std::map<std::string, std::shared_ptr<Bucket>>& buckets,
    std::vector<std::string> hashes, TmpDir const& downloadDir,
    std::shared_ptr<HistoryArchive> archive)
    : BatchWork{app, "download-verify-buckets"}
    , mBuckets{buckets}
    , mHashes{hashes}
    , mNextBucketIter{mHashes.begin()}
    , mDownloadDir{downloadDir}
    , mArchive{archive}
{
}

std::string
DownloadBucketsWork::getStatus() const
{
    if (!isDone() && !isAborting())
    {
        if (!mHashes.empty())
        {
            auto numStarted = std::distance(mHashes.begin(), mNextBucketIter);
            auto numDone = numStarted - getNumWorksInBatch();
            auto total = static_cast<uint32_t>(mHashes.size());
            auto pct = (100 * numDone) / total;
            return fmt::format(
                "downloading and verifying buckets: {:d}/{:d} ({:d}%)", numDone,
                total, pct);
        }
    }
    return Work::getStatus();
}

bool
DownloadBucketsWork::hasNext() const
{
    return mNextBucketIter != mHashes.end();
}

void
DownloadBucketsWork::resetIter()
{
    mNextBucketIter = mHashes.begin();
}

std::shared_ptr<BasicWork>
DownloadBucketsWork::yieldMoreWork()
{
    ZoneScoped;
    if (!hasNext())
    {
        throw std::runtime_error("Nothing to iterate over!");
    }

    auto hash = *mNextBucketIter;
    FileTransferInfo ft(mDownloadDir, HISTORY_FILE_TYPE_BUCKET, hash);
    auto w1 = std::make_shared<GetAndUnzipRemoteFileWork>(mApp, ft, mArchive);

    auto getFileWeak = std::weak_ptr<GetAndUnzipRemoteFileWork>(w1);
    OnFailureCallback cb = [getFileWeak, hash]() {
        auto getFile = getFileWeak.lock();
        if (getFile)
        {
            auto ar = getFile->getArchive();
            if (ar)
            {
                CLOG_INFO(History, "Bucket {} from archive {}", hash,
                          ar->getName());
            }
        }
    };

    auto w2 = std::make_shared<VerifyBucketWork>(
        mApp, mBuckets, ft.localPath_nogz(), hexToBin256(hash), cb);
    std::vector<std::shared_ptr<BasicWork>> seq{w1, w2};
    auto w3 = std::make_shared<WorkSequence>(
        mApp, "download-verify-sequence-" + hash, seq);

    ++mNextBucketIter;
    return w3;
}
}
