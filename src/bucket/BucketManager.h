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
struct LedgerHeader;
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
    static void dropAll(Application& app);

    virtual ~BucketManager()
    {
    }
    virtual std::string const& getTmpDir() = 0;
    virtual std::string const& getBucketDir() = 0;
    virtual BucketList& getBucketList() = 0;

    virtual medida::Timer& getMergeTimer() = 0;

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
                      size_t nObjects = 0, size_t nBytes = 0) = 0;

    // Return a bucket by hash if we have it, else return nullptr.
    virtual std::shared_ptr<Bucket> getBucketByHash(uint256 const& hash) = 0;

    // Forget any buckets not referenced by the current BucketList. This will
    // not immediately cause the buckets to delete themselves, if someone else
    // is using them via a shared_ptr<>, but the BucketManager will no longer
    // independently keep them alive.
    virtual void forgetUnreferencedBuckets() = 0;

    // Feed a new batch of entries to the bucket list.
    virtual void addBatch(Application& app, uint32_t currLedger,
                          std::vector<LedgerEntry> const& liveEntries,
                          std::vector<LedgerKey> const& deadEntries) = 0;

    // Update the given LedgerHeader's bucketListHash to reflect the current
    // state of the bucket list.
    virtual void snapshotLedger(LedgerHeader& currentHeader) = 0;

    // Check for missing bucket files that would prevent `assumeState` from
    // succeeding
    virtual std::vector<std::string>
    checkForMissingBucketsFiles(HistoryArchiveState const& has) = 0;

    // Retain all buckets from history state.
    virtual void retainAll(HistoryArchiveState const& has) = 0;

    // Restart from a saved state: find and attach all buckets in `has`, set
    // current BL.
    virtual void assumeState(HistoryArchiveState const& has) = 0;

    // Ensure all needed buckets are retained
    virtual void shutdown() = 0;
};
}
