// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/DownloadBucketsWork.h"
#include "bucket/BucketManager.h"
#include "catchup/LedgerApplyManager.h"
#include "history/FileTransferInfo.h"
#include "history/HistoryArchive.h"
#include "historywork/GetAndUnzipRemoteFileWork.h"
#include "historywork/VerifyBucketWork.h"
#include "work/WorkWithCallback.h"
#include <Tracy.hpp>
#include <fmt/format.h>
#include <mutex>

namespace stellar
{

DownloadBucketsWork::DownloadBucketsWork(
    Application& app,
    std::map<std::string, std::shared_ptr<LiveBucket>>& buckets,
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
                FMT_STRING(
                    "downloading and verifying buckets: {:d}/{:d} ({:d}%)"),
                numDone, total, pct);
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
    FileTransferInfo ft(mDownloadDir, FileType::HISTORY_FILE_TYPE_BUCKET, hash);
    auto w1 = std::make_shared<GetAndUnzipRemoteFileWork>(mApp, ft, mArchive);

    auto getFileWeak = std::weak_ptr<GetAndUnzipRemoteFileWork>(w1);
    OnFailureCallback failureCb = [getFileWeak, hash]() {
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
    std::weak_ptr<DownloadBucketsWork> weak(
        std::static_pointer_cast<DownloadBucketsWork>(shared_from_this()));

    auto currId = mIndexId++;
    mIndexMapMutex.lock();
    auto [indexIter, inserted] = mIndexMap.emplace(currId, nullptr);
    mIndexMapMutex.unlock();
    releaseAssertOrThrow(inserted);

    auto successCb = [weak, ft, hash, currId](Application& app) -> bool {
        auto self = weak.lock();
        if (self)
        {
            // To avoid dangling references, maintain a map of index pointers
            // and do a lookup inside the callback instead of capturing anything
            // by reference.
            std::unique_ptr<LiveBucketIndex const> index{};
            {
                std::lock_guard<std::mutex> lock(self->mIndexMapMutex);
                auto indexIter = self->mIndexMap.find(currId);
                releaseAssertOrThrow(indexIter != self->mIndexMap.end());
                releaseAssertOrThrow(indexIter->second);
                index = std::move(indexIter->second);
                self->mIndexMap.erase(indexIter);
            }

            auto bucketPath = ft.localPath_nogz();
            auto b = app.getBucketManager().adoptFileAsBucket<LiveBucket>(
                bucketPath, hexToBin256(hash),
                /*mergeKey=*/nullptr,
                /*index=*/std::move(index));
            self->mBuckets[hash] = b;
        }
        return true;
    };
    auto w2 = std::make_shared<VerifyBucketWork>(mApp, ft.localPath_nogz(),
                                                 hexToBin256(hash),
                                                 indexIter->second, failureCb);
    auto w3 = std::make_shared<WorkWithCallback>(mApp, "adopt-verified-bucket",
                                                 successCb);
    std::vector<std::shared_ptr<BasicWork>> seq{w1, w2, w3};
    auto w4 = std::make_shared<WorkSequence>(
        mApp, "download-verify-sequence-" + hash, seq);

    ++mNextBucketIter;
    return w4;
}
}
