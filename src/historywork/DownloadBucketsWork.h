// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
#pragma once

#include "bucket/LiveBucket.h"
#include "historywork/Progress.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "util/TmpDir.h"
#include "work/BatchWork.h"

namespace stellar
{

class HistoryArchive;
class FileTransferInfo;

class DownloadBucketsWork : public BatchWork
{
    // Must hold mLiveMapMutex/mHotMapMutex when accessing
    // mLiveBuckets/mHotBuckets
    std::map<std::string, std::shared_ptr<LiveBucket>>& mLiveBuckets;
    std::map<std::string, std::shared_ptr<HotArchiveBucket>>& mHotBuckets;

    std::vector<std::string> mLiveHashes;
    std::vector<std::string> mHotHashes;
    std::vector<std::string>::const_iterator mNextLiveBucketIter;
    std::vector<std::string>::const_iterator mNextHotBucketIter;
    TmpDir const& mDownloadDir;
    std::shared_ptr<HistoryArchive> mArchive;

    // Store indexes of downloaded buckets. Child processes will actually create
    // the indexes, but DownloadBucketsWork needs to maintain actual ownership
    // of the pointers so that the success callback can pass them to the
    // BucketManager. Must be protected by a mutex to avoid race conditions.
    std::map<int, std::unique_ptr<LiveBucketIndex const>> mLiveIndexMap;
    std::map<int, std::unique_ptr<HotArchiveBucketIndex const>> mHotIndexMap;

    // Must be held when accessing mIndexMaps and bucketMaps
    std::mutex mLiveMapMutex;
    std::mutex mHotMapMutex;
    int mLiveIndexId{0};
    int mHotIndexId{0};

    template <typename BucketT>
    static void
    onSuccessCb(Application& app, FileTransferInfo const& ft,
                std::string const& hash, int currId,
                std::map<std::string, std::shared_ptr<BucketT>>& buckets,
                std::map<int, std::unique_ptr<typename BucketT::IndexT const>>&
                    indexMap,
                std::mutex& indexMutex);

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
