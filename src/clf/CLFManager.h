#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <memory>
#include "generated/StellarXDR.h"
#include "clf/Bucket.h"

#include "medida/timer_context.h"

namespace stellar
{

class Application;
class BucketList;
struct LedgerHeader;

class CLFManager
{

public:
    static std::unique_ptr<CLFManager> create(Application&);

    virtual ~CLFManager() {}
    virtual std::string const& getTmpDir() = 0;
    virtual std::string const& getBucketDir() = 0;
    virtual BucketList& getBucketList() = 0;

    virtual medida::Timer& getMergeTimer() = 0;

    // Get a reference to a persistent bucket in the CLF-managed bucket
    // directory, from the CLF's shared bucket-set.
    //
    // Concretely: if `hash` names an existing bucket, delete `filename` and
    // return
    // the existing bucket; otherwise move `filename` to the bucket directory,
    // stored under `hash`, and return a new bucket pointing to that.
    //
    // This method is mostly-threadsafe -- assuming you don't destruct the
    // CLFManager mid-call -- and is intended to be called from both main and
    // worker threads. Very carefully.
    virtual std::shared_ptr<Bucket> adoptFileAsBucket(std::string const& filename,
                                                      uint256 const& hash,
                                                      size_t nObjects = 0,
                                                      size_t nBytes = 0) = 0;

    // Return a bucket by hash if we have it, else return nullptr.
    virtual std::shared_ptr<Bucket> getBucketByHash(uint256 const& hash) const = 0;

    // Forget any buckets not referenced by the current BucketList. This will
    // not immediately cause the buckets to delete themselves, if someone else
    // is using them via a shared_ptr<>, but the CLFManager will no longer
    // independently keep them alive.
    virtual void forgetUnreferencedBuckets() = 0;

    // Feed a new batch of entries to the bucket list.
    virtual void addBatch(Application& app, uint32_t currLedger,
                          std::vector<LedgerEntry> const& liveEntries,
                          std::vector<LedgerKey> const& deadEntries) = 0;

    // Update the given LedgerHeader's clfHash to reflect the current state of
    // the bucket list.
    virtual void snapshotLedger(LedgerHeader& currentHeader) = 0;
};
}
