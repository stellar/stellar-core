#pragma once

// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/LedgerCmp.h"
#include "util/NonCopyable.h"
#include <list>
#include <set>

#include <optional>

namespace stellar
{

class Bucket;
class XDRInputFileStream;
class SearchableBucketListSnapshot;
struct EvictionResultEntry;
class LedgerKeyMeter;

// A lightweight wrapper around Bucket for thread safe BucketListDB lookups
class BucketSnapshot : public NonMovable
{
    std::shared_ptr<Bucket const> const mBucket;

    // Lazily-constructed and retained for read path.
    mutable std::unique_ptr<XDRInputFileStream> mStream{};

    // Returns (lazily-constructed) file stream for bucket file. Note
    // this might be in some random position left over from a previous read --
    // must be seek()'ed before use.
    XDRInputFileStream& getStream() const;

    // Loads the bucket entry for LedgerKey k. Starts at file offset pos and
    // reads until key is found or the end of the page. Returns <BucketEntry,
    // bloomMiss>, where bloomMiss is true if a bloomMiss occurred during the
    // load.
    std::pair<std::optional<BucketEntry>, bool>
    getEntryAtOffset(LedgerKey const& k, std::streamoff pos,
                     size_t pageSize) const;

    BucketSnapshot(std::shared_ptr<Bucket const> const b);

    // Only allow copy constructor, is threadsafe
    BucketSnapshot(BucketSnapshot const& b);
    BucketSnapshot& operator=(BucketSnapshot const&) = delete;

  public:
    bool isEmpty() const;
    std::shared_ptr<Bucket const> getRawBucket() const;

    // Loads bucket entry for LedgerKey k. Returns <BucketEntry, bloomMiss>,
    // where bloomMiss is true if a bloomMiss occurred during the load.
    std::pair<std::optional<BucketEntry>, bool>
    getBucketEntry(LedgerKey const& k) const;

    // Loads LedgerEntry's for given keys. When a key is found, the
    // entry is added to result and the key is removed from keys.
    // If a pointer to a LedgerKeyMeter is provided, a key will only be loaded
    // if the meter has a transaction with sufficient read quota for the key.
    void loadKeysWithLimits(std::set<LedgerKey, LedgerEntryIdCmp>& keys,
                            std::vector<LedgerEntry>& result,
                            LedgerKeyMeter* lkMeter) const;

    // Return all PoolIDs that contain the given asset on either side of the
    // pool
    std::vector<PoolID> const& getPoolIDsByAsset(Asset const& asset) const;

    bool scanForEviction(EvictionIterator& iter, uint32_t& bytesToScan,
                         uint32_t ledgerSeq,
                         std::list<EvictionResultEntry>& evictableKeys,
                         SearchableBucketListSnapshot& bl) const;

    friend struct BucketLevelSnapshot;
};
}