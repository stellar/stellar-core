// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/DownloadBucketsWork.h"
#include "bucket/BucketManager.h"
#include "bucket/HotArchiveBucket.h"
#include "catchup/LedgerApplyManager.h"
#include "history/FileTransferInfo.h"
#include "history/HistoryArchive.h"
#include "historywork/GetAndUnzipRemoteFileWork.h"
#include "historywork/VerifyBucketWork.h"
#include "work/WorkWithCallback.h"
#include "xdr/Stellar-contract-config-setting.h"
#include <Tracy.hpp>
#include <fmt/format.h>

namespace stellar
{

DownloadBucketsWork::DownloadBucketsWork(
    Application& app,
    std::map<std::string, std::shared_ptr<LiveBucket>>& liveBuckets,
    std::map<std::string, std::shared_ptr<HotArchiveBucket>>& hotBuckets,
    std::vector<std::string> liveHashes, std::vector<std::string> hotHashes,
    TmpDir const& downloadDir, std::shared_ptr<HistoryArchive> archive)
    : BatchWork{app, "download-verify-buckets"}
    , mLiveBuckets{liveBuckets}
    , mHotBuckets{hotBuckets}
    , mLiveHashes{liveHashes}
    , mHotHashes{hotHashes}
    , mNextLiveBucketIter{mLiveHashes.begin()}
    , mNextHotBucketIter{mHotHashes.begin()}
    , mDownloadDir{downloadDir}
    , mArchive{archive}
{
}

std::string
DownloadBucketsWork::getStatus() const
{
    if (!isDone() && !isAborting())
    {
        if (!mLiveHashes.empty())
        {
            auto numStarted =
                std::distance(mLiveHashes.begin(), mNextLiveBucketIter) +
                std::distance(mHotHashes.begin(), mNextHotBucketIter);
            auto numDone = numStarted - getNumWorksInBatch();
            auto total =
                static_cast<uint32_t>(mLiveHashes.size() + mHotHashes.size());
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
    return mNextLiveBucketIter != mLiveHashes.end() ||
           mNextHotBucketIter != mHotHashes.end();
}

void
DownloadBucketsWork::resetIter()
{
    mNextLiveBucketIter = mLiveHashes.begin();
    mNextHotBucketIter = mHotHashes.begin();
}

template<typename BucketT>
bool
DownloadBucketsWork::onSuccessCb(
    Application& app, FileTransferInfo const& ft, std::string const& hash,
    int currId,
    std::map<std::string, std::shared_ptr<BucketT>>& buckets,
    std::map<int, std::unique_ptr<typename BucketT::IndexT const>>& indexMap)
{
    auto bucketPath = ft.localPath_nogz();
    auto indexIter = indexMap.find(currId);
    releaseAssertOrThrow(indexIter != indexMap.end());
    releaseAssertOrThrow(indexIter->second);

    auto b = app.getBucketManager().adoptFileAsBucket<BucketT>(
        bucketPath, hexToBin256(hash),
        /*mergeKey=*/nullptr,
        /*index=*/std::move(indexIter->second));
    buckets[hash] = b;
    indexMap.erase(currId);
    return true;
}

std::shared_ptr<BasicWork>
DownloadBucketsWork::yieldMoreWork()
{
    ZoneScoped;
    if (!hasNext())
    {
        throw std::runtime_error("Nothing to iterate over!");
    }

    // Every Bucket we need to download goes through three steps each, which are all handled by a separate work:
    // 1. Download the bucket file from the archive and unzip it (getFileWork)
    // 2. Verify and index the bucket file (verifyWork)
    // 3. Once verified, pass the Bucket to the BucketManager to be adopted and tracked (adoptWork)
    // First, we iterate through all the live buckets, then the hot archive buckets.
    auto isHotHash = mNextLiveBucketIter == mLiveHashes.end();
    auto hash = isHotHash ? *mNextHotBucketIter : *mNextLiveBucketIter;

    FileTransferInfo ft(mDownloadDir, FileType::HISTORY_FILE_TYPE_BUCKET, hash);
    auto getFileWork = std::make_shared<GetAndUnzipRemoteFileWork>(mApp, ft, mArchive);

    auto getFileWeakPtr = std::weak_ptr<GetAndUnzipRemoteFileWork>(getFileWork);
    OnFailureCallback failureCb = [getFileWeakPtr, hash]() {
        auto getFile = getFileWeakPtr.lock();
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


    std::weak_ptr<DownloadBucketsWork> weakSelf(
        std::static_pointer_cast<DownloadBucketsWork>(shared_from_this()));


    std::shared_ptr<BasicWork> verifyWork;
    std::function<bool(Application& app)> adoptBucketCb;

    if (isHotHash)
    {
        auto currId = mHotIndexId++;
        auto [indexIter, inserted] = mHotIndexMap.emplace(currId, nullptr);
        releaseAssertOrThrow(inserted);
        verifyWork = std::make_shared<VerifyBucketWork<HotArchiveBucket>>(mApp, ft.localPath_nogz(),
                                                hexToBin256(hash),
                                                indexIter->second, failureCb);
        adoptBucketCb = [this, &ft, hash, currId](Application& app) {
            return onSuccessCb<HotArchiveBucket>(app, ft, hash, currId,
                                               mHotBuckets, mHotIndexMap);
        };

        mNextHotBucketIter++;
    }
    else
    {
        auto currId = mLiveIndexId++;
        auto [indexIter, inserted] = mLiveIndexMap.emplace(currId, nullptr);
        releaseAssertOrThrow(inserted);
        verifyWork = std::make_shared<VerifyBucketWork<LiveBucket>>(mApp, ft.localPath_nogz(),
                                                hexToBin256(hash),
                                                indexIter->second, failureCb);
        adoptBucketCb = [this, &ft, hash, currId](Application& app) {
            return onSuccessCb<LiveBucket>(app, ft, hash, currId,
                                               mLiveBuckets, mLiveIndexMap);
        };

        mNextLiveBucketIter++;
    }

    auto adoptWork = std::make_shared<WorkWithCallback>(mApp, "adopt-verified-bucket",
                                               adoptBucketCb);
    std::vector<std::shared_ptr<BasicWork>> seq{getFileWork, verifyWork, adoptWork};
    auto workSequence = std::make_shared<WorkSequence>(
        mApp, "download-verify-sequence-" + hash, seq);

    return workSequence;
}

// Add explicit template instantiations
template bool DownloadBucketsWork::onSuccessCb<LiveBucket>(
    Application&, FileTransferInfo const&, std::string const&, int,
    std::map<std::string, std::shared_ptr<LiveBucket>>&,
    std::map<int, std::unique_ptr<LiveBucketIndex const>>&);

template bool DownloadBucketsWork::onSuccessCb<HotArchiveBucket>(
    Application&, FileTransferInfo const&, std::string const&, int,
    std::map<std::string, std::shared_ptr<HotArchiveBucket>>&,
    std::map<int, std::unique_ptr<HotArchiveBucketIndex const>>&);
}
