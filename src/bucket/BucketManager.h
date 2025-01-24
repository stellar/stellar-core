#pragma once

#include "bucket/BucketMergeMap.h"
#include "main/Config.h"
#include "util/TmpDir.h"
#include "util/types.h"
#include "work/BasicWork.h"
#include "xdr/Stellar-ledger.h"

#include <filesystem>
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
class HotArchiveBucketList;
class BucketSnapshotManager;
class SearchableLiveBucketListSnapshot;
struct BucketEntryCounters;
enum class LedgerEntryTypeAndDurability : uint32_t;
class SorobanNetworkConfig;

struct HistoryArchiveState;

/**
 * BucketManager is responsible for maintaining a collection of Buckets of
 * ledger entries (each sorted, de-duplicated and identified by hash) and,
 * primarily, for holding the BucketList: the distinguished, ordered collection
 * of buckets that are arranged in such a way as to efficiently provide a single
 * canonical hash for the state of all the entries in the ledger.
 *
 * Not every bucket is present in the BucketList at every instant; buckets
 * live in a few transient states while being merged, upload or downloaded
 * from history archives.
 *
 * Every bucket corresponds to a file on disk and the BucketManager owns a
 * directory in which the buckets it's responsible for reside. It locks this
 * directory exclusively while the process is running; only one BucketManager
 * should be attached to a single directory at a time.
 *
 * Buckets can be created outside the BucketManager's directory -- for example
 * in temporary directories -- and then "adopted" by the BucketManager, moved
 * into its directory and managed by it.
 */
class BucketManager : NonMovableOrCopyable
{
    template <class BucketT>
    using BucketMapT = std::map<Hash, std::shared_ptr<BucketT>>;

    template <class BucketT>
    using FutureMapT =
        UnorderedMap<MergeKey, std::shared_future<std::shared_ptr<BucketT>>>;

    static std::string const kLockFilename;

    // NB: ideally, BucketManager should have no access to mApp, as it's too
    // dangerous in the context of parallel application. BucketManager is quite
    // bloated, with lots of legacy code, so to ensure safety, annotate all
    // functions using mApp with `releaseAssert(threadIsMain())` and avoid
    // accessing mApp in the background.
    Application& mApp;
    std::unique_ptr<LiveBucketList> mLiveBucketList;
    std::unique_ptr<HotArchiveBucketList> mHotArchiveBucketList;
    std::unique_ptr<BucketSnapshotManager> mSnapshotManager;
    std::unique_ptr<TmpDirManager> mTmpDirManager;
    std::unique_ptr<TmpDir> mWorkDir;
    BucketMapT<LiveBucket> mSharedLiveBuckets;
    BucketMapT<HotArchiveBucket> mSharedHotArchiveBuckets;

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
    medida::Counter& mLiveBucketListSizeCounter;
    medida::Counter& mArchiveBucketListSizeCounter;
    EvictionCounters mBucketListEvictionCounters;
    MergeCounters mMergeCounters;
    std::shared_ptr<EvictionStatistics> mEvictionStatistics{};
    std::map<LedgerEntryTypeAndDurability, medida::Counter&>
        mBucketListEntryCountCounters;
    std::map<LedgerEntryTypeAndDurability, medida::Counter&>
        mBucketListEntrySizeCounters;

    std::future<EvictionResultCandidates> mEvictionFuture{};

    // Copy app's config for thread-safe access
    Config const mConfig;

    // Records bucket-merges that are currently _live_ in some FutureBucket, in
    // the sense of either running, or finished (with or without the
    // FutureBucket being resolved). Entries in this map will be cleared when
    // the FutureBucket is _cleared_ (typically when the owning BucketList level
    // is committed).
    FutureMapT<LiveBucket> mLiveBucketFutures;
    FutureMapT<HotArchiveBucket> mHotArchiveBucketFutures;

    // Records bucket-merges that are _finished_, i.e. have been adopted as
    // (possibly redundant) bucket files. This is a "weak" (bi-multi-)map of
    // hashes, that does not count towards std::shared_ptr refcounts, i.e. does
    // not keep either the output bucket or any of its input buckets
    // alive. Needs to be queried and updated on mSharedBuckets GC events.
    BucketMergeMap mFinishedMerges;

    std::atomic<bool> mIsShutdown{false};

    void cleanupStaleFiles(HistoryArchiveState const& has);
    void deleteTmpDirAndUnlockBucketDir();
    void deleteEntireBucketDir();

    void updateSharedBucketSize();

    template <class BucketT>
    std::shared_ptr<BucketT> adoptFileAsBucketInternal(
        std::string const& filename, uint256 const& hash, MergeKey* mergeKey,
        std::unique_ptr<typename BucketT::IndexT const> index,
        BucketMapT<BucketT>& bucketMap, FutureMapT<BucketT>& futureMap);

    template <class BucketT>
    std::shared_ptr<BucketT>
    getBucketByHashInternal(uint256 const& hash,
                            BucketMapT<BucketT>& bucketMap);
    template <class BucketT>
    std::shared_ptr<BucketT>
    getBucketIfExistsInternal(uint256 const& hash,
                              BucketMapT<BucketT> const& bucketMap) const;

    template <class BucketT>
    std::shared_future<std::shared_ptr<BucketT>>
    getMergeFutureInternal(MergeKey const& key, FutureMapT<BucketT>& futureMap);

    template <class BucketT>
    void
    putMergeFutureInternal(MergeKey const& key,
                           std::shared_future<std::shared_ptr<BucketT>> future,
                           FutureMapT<BucketT>& futureMap);

    template <class BucketT>
    void noteEmptyMergeOutputInternal(MergeKey const& mergeKey,
                                      FutureMapT<BucketT>& futureMap);

#ifdef BUILD_TESTS
    bool mUseFakeTestValuesForNextClose{false};
    uint32_t mFakeTestProtocolVersion;
    uint256 mFakeTestBucketListHash;
#endif

  protected:
    BucketManager(Application& app);
    void calculateSkipValues(LedgerHeader& currentHeader);
    std::string bucketFilename(std::string const& bucketHexHash);
    std::string bucketFilename(Hash const& hash);

  public:
    static std::unique_ptr<BucketManager> create(Application& app);
    virtual ~BucketManager();

    void initialize();
    void dropAll();
    std::string bucketIndexFilename(Hash const& hash) const;
    std::string const& getTmpDir();
    TmpDirManager& getTmpDirManager();
    std::string const& getBucketDir() const;
    LiveBucketList& getLiveBucketList();
    HotArchiveBucketList& getHotArchiveBucketList();
    BucketSnapshotManager& getBucketSnapshotManager() const;
    bool renameBucketDirFile(std::filesystem::path const& src,
                             std::filesystem::path const& dst);

    medida::Timer& getMergeTimer();

    template <class BucketT> medida::Meter& getBloomMissMeter() const;
    template <class BucketT> medida::Meter& getBloomLookupMeter() const;

    // Reading and writing the merge counters is done in bulk, and takes a lock
    // briefly; this can be done from any thread.
    MergeCounters readMergeCounters();
    void incrMergeCounters(MergeCounters const& delta);

    // Get a reference to a persistent bucket (in the BucketManager's bucket
    // directory), from the BucketManager's shared bucket-set.
    //
    // Concretely: if `hash` names an existing bucket -- either in-memory or on
    // disk -- delete `filename` and return an object for the existing bucket;
    // otherwise move `filename` to the bucket directory, stored under `hash`,
    // and return a new bucket pointing to that.
    //
    // This method is mostly-threadsafe -- assuming you don't destruct the
    // BucketManager mid-call -- and is intended to be called from both main and
    // worker threads. Very carefully.
    template <class BucketT>
    std::shared_ptr<BucketT>
    adoptFileAsBucket(std::string const& filename, uint256 const& hash,
                      MergeKey* mergeKey,
                      std::unique_ptr<typename BucketT::IndexT const> index);

    // Companion method to `adoptFileAsLiveBucket` also called from the
    // `BucketOutputIterator::getBucket` merge-completion path. This method
    // however should be called when the output bucket is _empty_ and thereby
    // doesn't correspond to a file on disk; the method forgets about the
    // `FutureBucket` associated with the in-progress merge, allowing the merge
    // inputs to be GC'ed.
    template <class BucketT>
    void noteEmptyMergeOutput(MergeKey const& mergeKey);

    // Returns a bucket by hash if it exists and is currently managed by the
    // bucket list.
    template <class BucketT>
    std::shared_ptr<BucketT> getBucketIfExists(uint256 const& hash);

    // Return a bucket by hash if we have it, else return nullptr.
    template <class BucketT>
    std::shared_ptr<BucketT> getBucketByHash(uint256 const& hash);

    // Get a reference to a merge-future that's either running (or finished
    // somewhat recently) from either a map of the std::shared_futures doing the
    // merges and/or a set of records mapping merge inputs to outputs and the
    // set of outputs held in the BucketManager. Returns an invalid future if no
    // such future can be found or synthesized.
    template <class BucketT>
    std::shared_future<std::shared_ptr<BucketT>>
    getMergeFuture(MergeKey const& key);

    // Add a reference to a merge _in progress_ (not yet adopted as a file) to
    // the BucketManager's internal map of std::shared_futures doing merges.
    // There is no corresponding entry-removal API: the std::shared_future will
    // be removed from the map when the merge completes and the output file is
    // adopted.
    template <class BucketT>
    void putMergeFuture(MergeKey const& key,
                        std::shared_future<std::shared_ptr<BucketT>> future);

#ifdef BUILD_TESTS
    // Drop all references to merge futures in progress.
    void clearMergeFuturesForTesting();
#endif

    // Forget any buckets not referenced by the current BucketList. This will
    // not immediately cause the buckets to delete themselves, if someone else
    // is using them via a shared_ptr<>, but the BucketManager will no longer
    // independently keep them alive.
    void forgetUnreferencedBuckets(HistoryArchiveState const& has);

    // Feed a new batch of entries to the bucket list. This interface expects to
    // be given separate init (created) and live (updated) entry vectors. The
    // `header` value should be taken from the ledger at which this batch is
    // being added.
    void addLiveBatch(Application& app, LedgerHeader header,
                      std::vector<LedgerEntry> const& initEntries,
                      std::vector<LedgerEntry> const& liveEntries,
                      std::vector<LedgerKey> const& deadEntries);
    void addHotArchiveBatch(Application& app, LedgerHeader header,
                            std::vector<LedgerEntry> const& archivedEntries,
                            std::vector<LedgerKey> const& restoredEntries,
                            std::vector<LedgerKey> const& deletedEntries);

    // Update the given LedgerHeader's bucketListHash to reflect the current
    // state of the bucket list.
    void snapshotLedger(LedgerHeader& currentHeader);

    // Sets index for bucket b if b is not already indexed and if BucketManager
    // is not shutting down. In most cases, there should only be a single index
    // for each bucket. However, during startup there are race conditions where
    // a bucket may be indexed twice. If there is an index race, set index with
    // this function, otherwise use BucketBase::setIndex().
    template <class BucketT>
    void maybeSetIndex(std::shared_ptr<BucketT> b,
                       std::unique_ptr<typename BucketT::IndexT const>&& index);

    // Scans BucketList for non-live entries to evict starting at the entry
    // pointed to by EvictionIterator. Evicts until `maxEntriesToEvict` entries
    // have been evicted or maxEvictionScanSize bytes have been scanned.
    void startBackgroundEvictionScan(uint32_t ledgerSeq, uint32_t ledgerVers,
                                     SorobanNetworkConfig const& cfg);

    // Returns a pair of vectors representing entries evicted this ledger, where
    // the first vector constains all deleted keys (TTL and temporary), and
    // the second vector contains all archived entries (persistent and
    // ContractCode). Note that when an entry is archived, its TTL key will be
    // included in the deleted keys vector.
    EvictedStateVectors
    resolveBackgroundEvictionScan(AbstractLedgerTxn& ltx, uint32_t ledgerSeq,
                                  LedgerKeySet const& modifiedKeys,
                                  uint32_t ledgerVers,
                                  SorobanNetworkConfig const& networkConfig);

    medida::Meter& getBloomMissMeter() const;
    medida::Meter& getBloomLookupMeter() const;

#ifdef BUILD_TESTS
    // Install a fake/assumed ledger version and bucket list hash to use in next
    // call to addLiveBatch and snapshotLedger. This interface exists only for
    // testing in a specific type of history replay.
    void setNextCloseVersionAndHashForTesting(uint32_t protocolVers,
                                              uint256 const& hash);

    // Return the set of buckets in the current `getBucketDir()` directory.
    // This interface exists only for checking that the BucketDir isn't
    // leaking buckets, in tests.
    std::set<Hash> getBucketHashesInBucketDirForTesting() const;

    medida::Counter& getEntriesEvictedCounter() const;
#endif

    // Return the set of buckets referenced by the BucketList
    std::set<Hash> getBucketListReferencedBuckets() const;

    // Return the set of buckets referenced by the BucketList, LCL HAS,
    // and publish queue.
    std::set<Hash>
    getAllReferencedBuckets(HistoryArchiveState const& has) const;

    // Check for missing bucket files that would prevent `assumeState` from
    // succeeding
    std::vector<std::string>
    checkForMissingBucketsFiles(HistoryArchiveState const& has);

    // Assume state from `has` in BucketList: find and attach all buckets in
    // `has`, set current BL.
    void assumeState(HistoryArchiveState const& has,
                     uint32_t maxProtocolVersion, bool restartMerges);

    void shutdown();

    bool isShutdown() const;

    // Load the complete state of the ledger from the provided HAS. Throws if
    // any of the buckets referenced in the HAS do not exist.
    //
    // Note: this returns an _ordered_ map because we want to enable writing it
    // straight to a single "merged bucket" with a canonical order for debugging
    // purposes.
    //
    // Also note: this returns a large map -- likely multiple GB of memory on
    // public nodes. The whole ledger. Call carefully, and only offline.
    std::map<LedgerKey, LedgerEntry>
    loadCompleteLedgerState(HistoryArchiveState const& has);

    // Merge the bucket list of the provided HAS into a single "super bucket"
    // consisting of only live entries, and return it.
    std::shared_ptr<LiveBucket> mergeBuckets(HistoryArchiveState const& has);

    // Visits all the active ledger entries or subset thereof.
    //
    // The order in which the entries are visited is not defined, but roughly
    // goes from more fresh entries to the older ones.
    //
    // This accepts two visitors. `filterEntry` has to return `true`
    // if the ledger entry can *potentially* be accepted. The passed entry isn't
    // necessarily fresh or even alive. `acceptEntry` will only get the fresh
    // alive entries that have passed the filter. If it returns `false` the
    // iteration will immediately finish.
    //
    // When `minLedger` is specified, only entries that have been modified at
    // `minLedger` or later are visited.
    //
    // When `filterEntry` and `acceptEntry` always return `true`, this is
    // equivalent to iterating over `loadCompleteLedgerState`, so the same
    // memory/runtime implications apply.
    void visitLedgerEntries(
        HistoryArchiveState const& has, std::optional<int64_t> minLedger,
        std::function<bool(LedgerEntry const&)> const& filterEntry,
        std::function<bool(LedgerEntry const&)> const& acceptEntry,
        bool includeAllStates);

    // Schedule a Work class that verifies the hashes of all referenced buckets
    // on background threads.
    std::shared_ptr<BasicWork>
    scheduleVerifyReferencedBucketsWork(HistoryArchiveState const& has);

    Config const& getConfig() const;
    void reportBucketEntryCountMetrics();
};

#define SKIP_1 50
#define SKIP_2 5000
#define SKIP_3 50000
#define SKIP_4 500000
}
