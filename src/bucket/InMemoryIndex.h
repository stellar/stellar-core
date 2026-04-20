// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "bucket/BucketIndexUtils.h"
#include "bucket/BucketUtils.h"
#include "xdr/Stellar-ledger-entries.h"

#include "ledger/LedgerHashUtils.h"
#include <unordered_map>

namespace stellar
{

class SHA256;

// For small Buckets, we can cache all contents in memory. Because we cache all
// entries, the index is just as large as the Bucket itself, so we never persist
// this index type. It is always recreated on startup.
//
// Uses an unordered_map<LedgerKey, IndexPtrT> for O(1) lookups without
// virtual dispatch or heap allocation per query. The LedgerKey is stored
// separately from the BucketEntry, trading a small amount of memory for
// significantly faster lookups (no heap allocation per find(), no virtual
// dispatch for hash/equality).
class InMemoryBucketState : public NonMovableOrCopyable
{
    using InMemoryMap =
        std::unordered_map<LedgerKey, IndexPtrT, std::hash<LedgerKey>>;

    InMemoryMap mEntries;

  public:
    using IterT = InMemoryMap::const_iterator;

    // Insert a LedgerEntry (INIT/LIVE) into the cache.
    void insert(BucketEntry const& be);

    // Find a LedgerEntry. IterT::begin is always returned, and start is
    // ignored. This interface just helps maintain consistency with
    // DiskIndex::scan.
    std::pair<IndexReturnT, IterT> scan(IterT start,
                                        LedgerKey const& searchKey) const;

    IterT
    begin() const
    {
        return mEntries.begin();
    }
    IterT
    end() const
    {
        return mEntries.end();
    }

#ifdef BUILD_TESTS
    bool operator==(InMemoryBucketState const& in) const;
#endif
};

class InMemoryIndex
{
  private:
    InMemoryBucketState mInMemoryState;
    AssetPoolIDMap mAssetPoolIDMap;
    BucketEntryCounters mCounters{};
    std::map<LedgerEntryType, std::pair<std::streamoff, std::streamoff>>
        mTypeRanges;

  public:
    using IterT = InMemoryBucketState::IterT;

    InMemoryIndex(BucketManager const& bm,
                  std::filesystem::path const& filename, SHA256* hasher);

    InMemoryIndex(BucketManager& bm,
                  std::vector<BucketEntry> const& inMemoryState,
                  BucketMetadata const& metadata);

    IterT
    begin() const
    {
        return mInMemoryState.begin();
    }
    IterT
    end() const
    {
        return mInMemoryState.end();
    }

    AssetPoolIDMap const&
    getAssetPoolIDMap() const
    {
        return mAssetPoolIDMap;
    }

    BucketEntryCounters const&
    getBucketEntryCounters() const
    {
        return mCounters;
    }

    std::pair<IndexReturnT, IterT>
    scan(IterT start, LedgerKey const& searchKey) const
    {
        return mInMemoryState.scan(start, searchKey);
    }

    std::optional<std::pair<std::streamoff, std::streamoff>>
    getRangeForType(LedgerEntryType type) const;

#ifdef BUILD_TESTS
    bool operator==(InMemoryIndex const& in) const;
#endif
};
}
