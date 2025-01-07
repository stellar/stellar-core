#pragma once

// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketListBase.h"
#include "bucket/LiveBucket.h"

namespace stellar
{
// The LiveBucketList stores the current canonical state of the ledger. It is
// made up of LiveBucket buckets, which in turn store individual entries of type
// BucketEntry. When an entry is "evicted" from the ledger, it is removed from
// the LiveBucketList. Depending on the evicted entry type, it may then be added
// to the HotArchiveBucketList.
class LiveBucketList : public BucketListBase<LiveBucket>
{
  public:
    // Reset Eviction Iterator position if an incoming spill or upgrade has
    // invalidated the previous position
    static void updateStartingEvictionIterator(EvictionIterator& iter,
                                               uint32_t firstScanLevel,
                                               uint32_t ledgerSeq);

    // Update eviction iter and record stats after scanning a region in one
    // bucket. Returns true if scan has looped back to startIter, false
    // otherwise.
    static bool updateEvictionIterAndRecordStats(
        EvictionIterator& iter, EvictionIterator startIter,
        uint32_t configFirstScanLevel, uint32_t ledgerSeq,
        std::shared_ptr<EvictionStatistics> stats, EvictionCounters& counters);

    static void checkIfEvictionScanIsStuck(EvictionIterator const& evictionIter,
                                           uint32_t scanSize,
                                           std::shared_ptr<LiveBucket const> b,
                                           EvictionCounters& counters);

    // Add a batch of initial (created), live (updated) and dead entries to the
    // bucketlist, representing the entries effected by closing
    // `currLedger`. The bucketlist will incorporate these into the smallest
    // (0th) level, as well as commit or prepare merges for any levels that
    // should have spilled due to passing through `currLedger`. The `currLedger`
    // and `currProtocolVersion` values should be taken from the ledger at which
    // this batch is being added.
    void addBatch(Application& app, uint32_t currLedger,
                  uint32_t currLedgerProtocol,
                  std::vector<LedgerEntry> const& initEntries,
                  std::vector<LedgerEntry> const& liveEntries,
                  std::vector<LedgerKey> const& deadEntries);

    BucketEntryCounters sumBucketEntryCounters() const;
};

}