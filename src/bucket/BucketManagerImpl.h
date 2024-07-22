#pragma once

#include "bucket/BucketList.h"
#include "bucket/BucketManager.h"
#include "bucket/BucketMergeMap.h"
#include "xdr/Stellar-ledger.h"

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

namespace medida
{
class Timer;
class Meter;
class Counter;
}

namespace stellar
{

class TmpDir;
class AbstractLedgerTxn;
class Application;
class Bucket;
class LiveBucketList;
class BucketSnapshotManager;
struct HistoryArchiveState;

class BucketManagerImpl : public BucketManager
{
    static std::string const kLockFilename;

    Application& mApp;
    std::unique_ptr<LiveBucketList> mLiveBucketList;
    std::unique_ptr<HotArchiveBucketList> mHotArchiveBucketList;
    std::unique_ptr<BucketSnapshotManager> mSnapshotManager;
    std::unique_ptr<TmpDirManager> mTmpDirManager;
    std::unique_ptr<TmpDir> mWorkDir;
    std::map<Hash, std::shared_ptr<Bucket>> mSharedBuckets;

    // Lock for managing raw Bucket files or the bucket directory. This lock is
    // only required for file access, but is not required for logical changes to
    // a BucketList (i.e. addLiveBatch).
    mutable std::recursive_mutex mBucketMutex;
    std::unique_ptr<std::string> mLockedBucketDir;
    medida::Meter& mBucketLiveObjectInsertBatch;
    medida::Meter& mBucketArchiveObjectInsertBatch;
    medida::Timer& mBucketAddLiveBatch;
    medida::Timer& mBucketAddArchiveBatch;
    medida::Timer& mBucketSnapMerge;
    medida::Counter& mSharedBucketsSize;
    medida::Meter& mBucketListDBBloomMisses;
    medida::Meter& mBucketListDBBloomLookups;
    medida::Counter& mLiveBucketListSizeCounter;
    medida::Counter& mArchiveBucketListSizeCounter;
    EvictionCounters mBucketListEvictionCounters;
    MergeCounters mMergeCounters;
    std::shared_ptr<EvictionStatistics> mEvictionStatistics{};

    std::future<EvictionResult> mEvictionFuture{};

    bool const mDeleteEntireBucketDirInDtor;

    // Records bucket-merges that are currently _live_ in some FutureBucket, in
    // the sense of either running, or finished (with or without the
    // FutureBucket being resolved). Entries in this map will be cleared when
    // the FutureBucket is _cleared_ (typically when the owning BucketList level
    // is committed).
    UnorderedMap<MergeKey, std::shared_future<std::shared_ptr<Bucket>>>
        mLiveFutures;

    // Records bucket-merges that are _finished_, i.e. have been adopted as
    // (possibly redundant) bucket files. This is a "weak" (bi-multi-)map of
    // hashes, that does not count towards std::shared_ptr refcounts, i.e. does
    // not keep either the output bucket or any of its input buckets
    // alive. Needs to be queried and updated on mSharedBuckets GC events.
    BucketMergeMap mFinishedMerges;

    std::atomic<bool> mIsShutdown{false};

    void cleanupStaleFiles();
    void deleteTmpDirAndUnlockBucketDir();
    void deleteEntireBucketDir();

    medida::Timer& recordBulkLoadMetrics(std::string const& label,
                                         size_t numEntries) const;
    medida::Timer& getPointLoadTimer(LedgerEntryType t) const;

#ifdef BUILD_TESTS
    bool mUseFakeTestValuesForNextClose{false};
    uint32_t mFakeTestProtocolVersion;
    uint256 mFakeTestBucketListHash;
#endif

  protected:
    void calculateSkipValues(LedgerHeader& currentHeader);
    std::string bucketFilename(std::string const& bucketHexHash);
    std::string bucketFilename(Hash const& hash);

  public:
    BucketManagerImpl(Application& app);
    ~BucketManagerImpl() override;
    void initialize() override;
    void dropAll() override;
    std::string bucketIndexFilename(Hash const& hash) const override;
    std::string const& getTmpDir() override;
    std::string const& getBucketDir() const override;
    LiveBucketList& getLiveBucketList() override;
    HotArchiveBucketList& getHotArchiveBucketList() override;
    BucketSnapshotManager& getBucketSnapshotManager() const override;
    medida::Timer& getMergeTimer() override;
    MergeCounters readMergeCounters() override;
    void incrMergeCounters(MergeCounters const&) override;
    TmpDirManager& getTmpDirManager() override;
    bool renameBucketDirFile(std::filesystem::path const& src,
                             std::filesystem::path const& dst) override;
    std::shared_ptr<Bucket>
    adoptFileAsBucket(std::string const& filename, uint256 const& hash,
                      MergeKey* mergeKey,
                      std::unique_ptr<BucketIndex const> index) override;
    void noteEmptyMergeOutput(MergeKey const& mergeKey) override;
    std::shared_ptr<Bucket> getBucketIfExists(uint256 const& hash) override;
    std::shared_ptr<Bucket> getBucketByHash(uint256 const& hash) override;

    std::shared_future<std::shared_ptr<Bucket>>
    getMergeFuture(MergeKey const& key) override;
    void putMergeFuture(MergeKey const& key,
                        std::shared_future<std::shared_ptr<Bucket>>) override;
#ifdef BUILD_TESTS
    void clearMergeFuturesForTesting() override;
#endif

    void forgetUnreferencedBuckets() override;
    void addLiveBatch(Application& app, uint32_t currLedger,
                      uint32_t currLedgerProtocol,
                      std::vector<LedgerEntry> const& initEntries,
                      std::vector<LedgerEntry> const& liveEntries,
                      std::vector<LedgerKey> const& deadEntries) override;
    void addArchivalBatch(Application& app, uint32_t currLedger,
                          uint32_t currLedgerProtocol,
                          std::vector<LedgerEntry> const& initEntries,
                          std::vector<LedgerKey> const& deadEntries) override;
    void snapshotLedger(LedgerHeader& currentHeader) override;
    void maybeSetIndex(std::shared_ptr<Bucket> b,
                       std::unique_ptr<BucketIndex const>&& index) override;
    void scanForEvictionLegacy(AbstractLedgerTxn& ltx,
                               uint32_t ledgerSeq) override;
    void startBackgroundEvictionScan(uint32_t ledgerSeq) override;
    void
    resolveBackgroundEvictionScan(AbstractLedgerTxn& ltx, uint32_t ledgerSeq,
                                  LedgerKeySet const& modifiedKeys) override;

    medida::Meter& getBloomMissMeter() const override;
    medida::Meter& getBloomLookupMeter() const override;

#ifdef BUILD_TESTS
    // Install a fake/assumed ledger version and bucket list hash to use in next
    // call to addLiveBatch and snapshotLedger. This interface exists only for
    // testing in a specific type of history replay.
    void setNextCloseVersionAndHashForTesting(uint32_t protocolVers,
                                              uint256 const& hash) override;

    std::set<Hash> getBucketHashesInBucketDirForTesting() const override;

    medida::Counter& getEntriesEvictedCounter() const override;
#endif

    std::set<Hash> getBucketListReferencedBuckets() const override;
    std::set<Hash> getAllReferencedBuckets() const override;
    std::vector<std::string>
    checkForMissingBucketsFiles(HistoryArchiveState const& has) override;
    void assumeState(HistoryArchiveState const& has,
                     uint32_t maxProtocolVersion, bool restartMerges) override;
    void shutdown() override;

    bool isShutdown() const override;

    std::map<LedgerKey, LedgerEntry>
    loadCompleteLedgerState(HistoryArchiveState const& has) override;

    std::shared_ptr<Bucket>
    mergeBuckets(HistoryArchiveState const& has) override;

    void visitLedgerEntries(
        HistoryArchiveState const& has, std::optional<int64_t> minLedger,
        std::function<bool(LedgerEntry const&)> const& filterEntry,
        std::function<bool(LedgerEntry const&)> const& acceptEntry,
        bool includeAllStates) override;

    std::shared_ptr<BasicWork> scheduleVerifyReferencedBucketsWork() override;

    Config const& getConfig() const override;
};

#define SKIP_1 50
#define SKIP_2 5000
#define SKIP_3 50000
#define SKIP_4 500000
}
