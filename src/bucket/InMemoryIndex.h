#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketIndexUtils.h"
#include "bucket/BucketUtils.h"
#include "xdr/Stellar-ledger-entries.h"

#include <variant>
#include <vector>

namespace stellar
{

// For small Buckets, we can cache all contents in memory. Because we cache all
// entries, the index is just as large as the Bucket itself, so we never persist
// this index type. It is always recreated on startup.
class InMemoryBucketState : public NonMovableOrCopyable
{
    // Entries sorted by LedgerKey. INIT/LIVE entries stored as
    // LedgerEntry, DEADENTRY stored as LedgerKey.
    std::vector<IndexPtrT> mEntries;

  public:
    using IterT = std::vector<IndexPtrT>::const_iterator;

    // Insert a LedgerEntry (INIT/LIVE) into the ordered container. Entries must
    // be ordered.
    void pushBack(BucketEntry const& be);

    // Find a LedgerEntry by key starting from the given iterator.
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
    bool
    operator==(InMemoryBucketState const& in) const
    {
        return mEntries == in.mEntries;
    }
#endif
};

class InMemoryIndex
{
  private:
    InMemoryBucketState mInMemoryState;
    AssetPoolIDMap mAssetPoolIDMap;
    BucketEntryCounters mCounters{};
    std::optional<std::pair<std::streamoff, std::streamoff>> mOfferRange;

  public:
    using IterT = InMemoryBucketState::IterT;

    InMemoryIndex(BucketManager const& bm,
                  std::filesystem::path const& filename);

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
    getOfferRange() const
    {
        return mOfferRange;
    }

#ifdef BUILD_TESTS
    bool
    operator==(InMemoryIndex const& in) const
    {
        return mInMemoryState == in.mInMemoryState &&
               mAssetPoolIDMap == in.mAssetPoolIDMap &&
               mCounters == in.mCounters;
    }
#endif
};
}