// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
#pragma once

#include "bucket/HotArchiveBucket.h"
#include "bucket/LiveBucket.h"
#include "historywork/Progress.h"
#include "util/TmpDir.h"
#include "work/BatchWork.h"

namespace stellar
{

class HistoryArchive;
class FileTransferInfo;

class DownloadBucketsWork : public BatchWork
{
    // Wrapper around state associated with each BucketList
    template <typename BucketT> struct BucketState
    {
        // Must hold mutex when accessing buckets
        std::map<std::string, std::shared_ptr<BucketT>>& buckets;

        std::vector<std::string> hashes;
        std::vector<std::string>::const_iterator nextIter;

        // Store indexes of downloaded buckets. Child processes will actually
        // create the indexes, but DownloadBucketsWork needs to maintain actual
        // ownership of the pointers so that the success callback can pass them
        // to the BucketManager. Must be protected by mutex.
        std::map<int, std::unique_ptr<typename BucketT::IndexT const>> indexMap;

        std::mutex mutex;
        int indexId{0};

        BucketState(std::map<std::string, std::shared_ptr<BucketT>>& bucketMap,
                    std::vector<std::string> bucketHashes)
            : buckets(bucketMap)
            , hashes(std::move(bucketHashes))
            , nextIter(hashes.begin())
        {
        }
    };

    BucketState<LiveBucket> mLiveBucketsState;
    BucketState<HotArchiveBucket> mHotBucketsState;

    TmpDir const& mDownloadDir;
    std::shared_ptr<HistoryArchive> mArchive;

    template <typename BucketT>
    static void onSuccessCb(Application& app, FileTransferInfo const& ft,
                            std::string const& hash, int currId,
                            BucketState<BucketT>& state);

    // Helper function returns pair of verifyBucketWork and a callback to adopt
    // the verified BucketList.
    template <typename BucketT>
    std::pair<std::shared_ptr<BasicWork>, std::function<bool(Application&)>>
    prepareWorkForBucketType(std::string const& hash,
                             FileTransferInfo const& ft,
                             OnFailureCallback const& failureCb,
                             BucketState<BucketT>& state);

  public:
    // Note: hashes must contain both live and hot archive bucket hashes
    DownloadBucketsWork(
        Application& app,
        std::map<std::string, std::shared_ptr<LiveBucket>>& liveBuckets,
        std::map<std::string, std::shared_ptr<HotArchiveBucket>>& hotBuckets,
        std::vector<std::string> liveHashes, std::vector<std::string> hotHashes,
        TmpDir const& downloadDir,
        std::shared_ptr<HistoryArchive> archive = nullptr);
    ~DownloadBucketsWork() = default;
    std::string getStatus() const override;

  protected:
    bool hasNext() const override;
    std::shared_ptr<BasicWork> yieldMoreWork() override;
    void resetIter() override;
};
}
