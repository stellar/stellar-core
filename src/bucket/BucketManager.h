#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/Bucket.h"
#include "overlay/StellarXDR.h"
#include "util/NonCopyable.h"
#include <memory>

#include "medida/timer_context.h"

namespace stellar
{

class Application;
class BucketList;
class TmpDirManager;
struct LedgerHeader;
struct HistoryArchiveState;

// A fine-grained merge-operation-counter structure for tracking various
// events during merges. These are not medida counters becasue we do not
// want or need to publish this level of granularity outside of testing, and
// we do want merges to run as quickly as possible.
struct MergeCounters
{
    uint64_t mPreInitEntryProtocolMerges{0};
    uint64_t mPostInitEntryProtocolMerges{0};

    uint64_t mNewMetaEntries{0};
    uint64_t mNewInitEntries{0};
    uint64_t mNewLiveEntries{0};
    uint64_t mNewDeadEntries{0};
    uint64_t mOldMetaEntries{0};
    uint64_t mOldInitEntries{0};
    uint64_t mOldLiveEntries{0};
    uint64_t mOldDeadEntries{0};

    uint64_t mOldEntriesDefaultAccepted{0};
    uint64_t mNewEntriesDefaultAccepted{0};
    uint64_t mNewInitEntriesMergedWithOldDead{0};
    uint64_t mOldInitEntriesMergedWithNewLive{0};
    uint64_t mOldInitEntriesMergedWithNewDead{0};
    uint64_t mNewEntriesMergedWithOldNeitherInit{0};

    uint64_t mShadowScanSteps{0};
    uint64_t mMetaEntryShadowElisions{0};
    uint64_t mLiveEntryShadowElisions{0};
    uint64_t mInitEntryShadowElisions{0};
    uint64_t mDeadEntryShadowElisions{0};

    uint64_t mOutputIteratorTombstoneElisions{0};
    uint64_t mOutputIteratorBufferUpdates{0};
    uint64_t mOutputIteratorActualWrites{0};
    MergeCounters& operator+=(MergeCounters const& delta);
};

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
 * should be attached to a single diretory at a time.
 *
 * Buckets can be created outside the BucketManager's directory -- for example
 * in temporary directories -- and then "adopted" by the BucketManager, moved
 * into its directory and managed by it.
 */

class BucketManager : NonMovableOrCopyable
{

  public:
    static std::unique_ptr<BucketManager> create(Application&);

    virtual ~BucketManager()
    {
    }
    virtual void initialize() = 0;
    virtual void dropAll() = 0;
    virtual std::string const& getTmpDir() = 0;
    virtual TmpDirManager& getTmpDirManager() = 0;
    virtual std::string const& getBucketDir() = 0;
    virtual BucketList& getBucketList() = 0;

    virtual medida::Timer& getMergeTimer() = 0;

    // Reading and writing the merge counters is done in bulk, and takes a lock
    // briefly; this can be done from any thread.
    virtual MergeCounters readMergeCounters() = 0;
    virtual void incrMergeCounters(MergeCounters const& delta) = 0;

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
    virtual std::shared_ptr<Bucket>
    adoptFileAsBucket(std::string const& filename, uint256 const& hash,
                      size_t nObjects, size_t nBytes) = 0;

    // Return a bucket by hash if we have it, else return nullptr.
    virtual std::shared_ptr<Bucket> getBucketByHash(uint256 const& hash) = 0;

    // Forget any buckets not referenced by the current BucketList. This will
    // not immediately cause the buckets to delete themselves, if someone else
    // is using them via a shared_ptr<>, but the BucketManager will no longer
    // independently keep them alive.
    virtual void forgetUnreferencedBuckets() = 0;

    // Feed a new batch of entries to the bucket list. This interface expects to
    // be given separate init (created) and live (updated) entry vectors. The
    // `currLedger` and `currProtocolVersion` values should be taken from the
    // ledger at which this batch is being added.
    virtual void addBatch(Application& app, uint32_t currLedger,
                          uint32_t currLedgerProtocol,
                          std::vector<LedgerEntry> const& initEntries,
                          std::vector<LedgerEntry> const& liveEntries,
                          std::vector<LedgerKey> const& deadEntries) = 0;

    // Update the given LedgerHeader's bucketListHash to reflect the current
    // state of the bucket list.
    virtual void snapshotLedger(LedgerHeader& currentHeader) = 0;

    // Check for missing bucket files that would prevent `assumeState` from
    // succeeding
    virtual std::vector<std::string>
    checkForMissingBucketsFiles(HistoryArchiveState const& has) = 0;

    // Restart from a saved state: find and attach all buckets in `has`, set
    // current BL. Pass `maxProtocolVersion` to any restarted merges.
    virtual void assumeState(HistoryArchiveState const& has,
                             uint32_t maxProtocolVersion) = 0;

    // Ensure all needed buckets are retained
    virtual void shutdown() = 0;
};
}
