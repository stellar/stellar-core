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
#include <Tracy.hpp>
#include <fmt/format.h>
#include <mutex>

namespace stellar
{

DownloadBucketsWork::DownloadBucketsWork(
    Application& app,
    std::map<std::string, std::shared_ptr<LiveBucket>>& liveBuckets,
    std::map<std::string, std::shared_ptr<HotArchiveBucket>>& hotBuckets,
    std::vector<std::string> liveHashes, std::vector<std::string> hotHashes,
    TmpDir const& downloadDir, std::shared_ptr<HistoryArchive> archive)
    : BatchWork{app, "download-verify-buckets"}
    , mLiveBucketsState{liveBuckets, std::move(liveHashes)}
    , mHotBucketsState{hotBuckets, std::move(hotHashes)}
    , mDownloadDir{downloadDir}
    , mArchive{archive}
{
}

std::string
DownloadBucketsWork::getStatus() const
{
    if (!BatchWork::isDone() && !BatchWork::isAborting())
    {
        if (!mLiveBucketsState.hashes.empty() ||
            !mHotBucketsState.hashes.empty())
        {
            auto numStarted = std::distance(mLiveBucketsState.hashes.begin(),
                                            mLiveBucketsState.nextIter) +
                              std::distance(mHotBucketsState.hashes.begin(),
                                            mHotBucketsState.nextIter);
            auto numDone = numStarted - BatchWork::getNumWorksInBatch();
            auto total = static_cast<uint32_t>(mLiveBucketsState.hashes.size() +
                                               mHotBucketsState.hashes.size());
            auto pct = (100 * numDone) / total;
            return fmt::format(
                FMT_STRING(
                    "downloading and verifying buckets: {:d}/{:d} ({:d}%)"),
                numDone, total, pct);
        }
    }
    return BatchWork::getStatus();
}

bool
DownloadBucketsWork::hasNext() const
{
    return mLiveBucketsState.nextIter != mLiveBucketsState.hashes.end() ||
           mHotBucketsState.nextIter != mHotBucketsState.hashes.end();
}

void
DownloadBucketsWork::resetIter()
{
    mLiveBucketsState.nextIter = mLiveBucketsState.hashes.begin();
    mHotBucketsState.nextIter = mHotBucketsState.hashes.begin();
}

template <typename BucketT>
void
DownloadBucketsWork::onSuccessCb(Application& app, FileTransferInfo const& ft,
                                 std::string const& hash, int currId,
                                 BucketState<BucketT>& state)
{
    // To avoid dangling references, maintain a map of index pointers
    // and do a lookup inside the callback instead of capturing anything
    // by reference.
    std::unique_ptr<typename BucketT::IndexT const> index;
    std::filesystem::path bucketPath;
    {
        // Lock for indexMap access
        std::lock_guard<std::mutex> lock(state.mutex);
        bucketPath = ft.localPath_nogz();
        auto indexIter = state.indexMap.find(currId);
        releaseAssertOrThrow(indexIter != state.indexMap.end());
        releaseAssertOrThrow(indexIter->second);
        index = std::move(indexIter->second);
        state.indexMap.erase(currId);
    }

    auto b = app.getBucketManager().adoptFileAsBucket<BucketT>(
        bucketPath, hexToBin256(hash),
        /*mergeKey=*/nullptr,
        /*index=*/std::move(index));

    // Lock for buckets access
    std::lock_guard<std::mutex> lock(state.mutex);
    state.buckets[hash] = b;
}

template <typename BucketT>
std::pair<std::shared_ptr<BasicWork>, std::function<bool(Application&)>>
DownloadBucketsWork::prepareWorkForBucketType(
    std::string const& hash, FileTransferInfo const& ft,
    OnFailureCallback const& failureCb, BucketState<BucketT>& state)
{
    std::weak_ptr<DownloadBucketsWork> weakSelf(
        std::static_pointer_cast<DownloadBucketsWork>(
            BasicWork::shared_from_this()));

    std::lock_guard<std::mutex> lock(state.mutex);
    auto currId = state.indexId++;
    auto [indexIter, inserted] = state.indexMap.emplace(currId, nullptr);
    releaseAssertOrThrow(inserted);

    auto verifyWork = std::make_shared<VerifyBucketWork<BucketT>>(
        mApp, ft.localPath_nogz(), hexToBin256(hash), indexIter->second,
        failureCb);

    // C++17 does not support templated lambdas, so we have to manually dispatch
    // based on type
    constexpr bool isLiveBucket = std::is_same_v<BucketT, LiveBucket>;
    auto adoptBucketCb = [weakSelf, ft, hash, currId](Application& app) {
        auto self = weakSelf.lock();
        if (self)
        {
            if constexpr (isLiveBucket)
            {
                onSuccessCb<LiveBucket>(app, ft, hash, currId,
                                        self->mLiveBucketsState);
            }
            else
            {
                onSuccessCb<HotArchiveBucket>(app, ft, hash, currId,
                                              self->mHotBucketsState);
            }
        }
        return true;
    };

    state.nextIter++;

    return {verifyWork, adoptBucketCb};
}

std::shared_ptr<BasicWork>
DownloadBucketsWork::yieldMoreWork()
{
    ZoneScoped;
    if (!hasNext())
    {
        throw std::runtime_error("Nothing to iterate over!");
    }

    // Every Bucket we need to download goes through three steps each, which are
    // all handled by a separate work:
    // 1. Download the bucket file from the archive and unzip it (getFileWork)
    // 2. Verify and index the bucket file (verifyWork)
    // 3. Once verified, pass the Bucket to the BucketManager to be adopted and
    // tracked (adoptWork) First, we iterate through all the live buckets, then
    // the hot archive buckets.
    auto isHotHash =
        mLiveBucketsState.nextIter == mLiveBucketsState.hashes.end();
    auto hash =
        isHotHash ? *mHotBucketsState.nextIter : *mLiveBucketsState.nextIter;

    auto const ft = FileTransferInfo(mDownloadDir,
                                     FileType::HISTORY_FILE_TYPE_BUCKET, hash);
    auto getFileWork =
        std::make_shared<GetAndUnzipRemoteFileWork>(mApp, ft, mArchive);

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

    std::shared_ptr<BasicWork> verifyWork;
    std::function<bool(Application&)> adoptBucketCb;

    if (isHotHash)
    {
        std::tie(verifyWork, adoptBucketCb) =
            prepareWorkForBucketType<HotArchiveBucket>(hash, ft, failureCb,
                                                       mHotBucketsState);
    }
    else
    {
        std::tie(verifyWork, adoptBucketCb) =
            prepareWorkForBucketType<LiveBucket>(hash, ft, failureCb,
                                                 mLiveBucketsState);
    }

    auto adoptWork = std::make_shared<WorkWithCallback>(
        mApp, "adopt-verified-bucket", adoptBucketCb);
    std::vector<std::shared_ptr<BasicWork>> seq{getFileWork, verifyWork,
                                                adoptWork};
    auto workSequence = std::make_shared<WorkSequence>(
        mApp, "download-verify-sequence-" + hash, seq);

    return workSequence;
}

// Add explicit template instantiations
template void DownloadBucketsWork::onSuccessCb<LiveBucket>(
    Application&, FileTransferInfo const&, std::string const&, int,
    DownloadBucketsWork::BucketState<LiveBucket>&);

template void DownloadBucketsWork::onSuccessCb<HotArchiveBucket>(
    Application&, FileTransferInfo const&, std::string const&, int,
    DownloadBucketsWork::BucketState<HotArchiveBucket>&);

template std::pair<std::shared_ptr<BasicWork>,
                   std::function<bool(Application&)>>
DownloadBucketsWork::prepareWorkForBucketType<LiveBucket>(
    std::string const&, FileTransferInfo const&, OnFailureCallback const&,
    DownloadBucketsWork::BucketState<LiveBucket>&);

template std::pair<std::shared_ptr<BasicWork>,
                   std::function<bool(Application&)>>
DownloadBucketsWork::prepareWorkForBucketType<HotArchiveBucket>(
    std::string const&, FileTransferInfo const&, OnFailureCallback const&,
    DownloadBucketsWork::BucketState<HotArchiveBucket>&);
}
