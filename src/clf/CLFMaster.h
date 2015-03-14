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

class CLFMaster
{
    class Impl;
    std::unique_ptr<Impl> mImpl;

    static std::string const kLockFilename;

    void forgetBucket(std::string const& filename);
    friend class Bucket;

  public:
    CLFMaster(Application&);
    ~CLFMaster();
    std::string const& getTmpDir();
    std::string const& getBucketDir();
    BucketList& getBucketList();

    medida::TimerContext getMergeTimer();

    // Get a reference to a persistent bucket in the CLF-managed bucket
    // directory, from the CLF's shared bucket-set.
    //
    // Concretely: if `hash` names an existing bucket, delete `filename` and
    // return
    // the existing bucket; otherwise move `filename` to the bucket directory,
    // stored under `hash`, and return a new bucket pointing to that.
    //
    // This method is mostly-threadsafe -- assuming you don't destruct the
    // CLFMaster mid-call -- and is intended to be called from both main and
    // worker threads. Very carefully.
    std::shared_ptr<Bucket> adoptFileAsBucket(std::string const& filename,
                                              uint256 const& hash,
                                              size_t nObjects = 0,
                                              size_t nBytes = 0);

    // Return a bucket by hash if we have it, else return nullptr.
    std::shared_ptr<Bucket> getBucketByHash(uint256 const& hash) const;

    // Forget any buckets not referenced by the current BucketList. This
    // will not immediately cause the buckets to delete themselves, if
    // someone else is using them via a shared_ptr<>, but the CLF will no
    // longer independently keep them alive.
    void forgetUnreferencedBuckets();

    // feed a new batch of entries to the bucket list
    void addBatch(Application& app, uint32_t currLedger,
                  std::vector<LedgerEntry> const& liveEntries,
                  std::vector<LedgerKey> const& deadEntries);

    // updates the given LedgerHeader to reflect the current state of the bucket
    // list
    void snapshotLedger(LedgerHeader& currentHeader);
};
}
