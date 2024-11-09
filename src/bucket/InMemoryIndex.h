#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketIndexUtils.h"
#include "bucket/BucketUtils.h"
#include "xdr/Stellar-ledger-entries.h"

#include "ledger/LedgerHashUtils.h"
#include <unordered_set>

namespace stellar
{

class SHA256;

// LedgerKey sizes usually dominate LedgerEntry size, so we don't want to
// store a key-value map to be memory efficient. Instead, we store a set of
// InternalInMemoryBucketEntry objects, which is a wrapper around either a
// LedgerKey or cached BucketEntry. This allows us to use std::unordered_set to
// efficiently store cache entries, but allows lookup by key only.
// Note that C++20 allows heterogeneous lookup in unordered_set, so we can
// simplify this class once we upgrade.
class InternalInMemoryBucketEntry
{
  private:
    struct AbstractEntry
    {
        virtual ~AbstractEntry() = default;
        virtual LedgerKey copyKey() const = 0;
        virtual size_t hash() const = 0;
        virtual IndexPtrT const& get() const = 0;

        virtual bool
        operator==(const AbstractEntry& other) const
        {
            return copyKey() == other.copyKey();
        }
    };

    // "Value" entry type used for storing BucketEntry in cache
    struct ValueEntry : public AbstractEntry
    {
      private:
        IndexPtrT entry;

      public:
        ValueEntry(IndexPtrT entry) : entry(entry)
        {
        }

        LedgerKey
        copyKey() const override
        {
            return getBucketLedgerKey(*entry);
        }

        size_t
        hash() const override
        {
            return std::hash<LedgerKey>{}(getBucketLedgerKey(*entry));
        }

        IndexPtrT const&
        get() const override
        {
            return entry;
        }
    };

    // "Key" entry type only used for querying the cache
    struct QueryKey : public AbstractEntry
    {
      private:
        LedgerKey ledgerKey;

      public:
        QueryKey(LedgerKey const& ledgerKey) : ledgerKey(ledgerKey)
        {
        }

        LedgerKey
        copyKey() const override
        {
            return ledgerKey;
        }

        size_t
        hash() const override
        {
            return std::hash<LedgerKey>{}(ledgerKey);
        }

        IndexPtrT const&
        get() const override
        {
            throw std::runtime_error("Called get() on QueryKey");
        }
    };

    std::unique_ptr<AbstractEntry> impl;

  public:
    InternalInMemoryBucketEntry(IndexPtrT entry)
        : impl(std::make_unique<ValueEntry>(entry))
    {
    }

    InternalInMemoryBucketEntry(LedgerKey const& ledgerKey)
        : impl(std::make_unique<QueryKey>(ledgerKey))
    {
    }

    size_t
    hash() const
    {
        return impl->hash();
    }

    bool
    operator==(InternalInMemoryBucketEntry const& other) const
    {
        return impl->operator==(*other.impl);
    }

    IndexPtrT const&
    get() const
    {
        return impl->get();
    }
};

struct InternalInMemoryBucketEntryHash
{
    size_t
    operator()(InternalInMemoryBucketEntry const& entry) const
    {
        return entry.hash();
    }
};

// For small Buckets, we can cache all contents in memory. Because we cache all
// entries, the index is just as large as the Bucket itself, so we never persist
// this index type. It is always recreated on startup.
class InMemoryBucketState : public NonMovableOrCopyable
{
    using InMemorySet = std::unordered_set<InternalInMemoryBucketEntry,
                                           InternalInMemoryBucketEntryHash>;

    InMemorySet mEntries;

  public:
    using IterT = InMemorySet::const_iterator;

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
    std::optional<std::pair<std::streamoff, std::streamoff>> mContractCodeRange;

  public:
    using IterT = InMemoryBucketState::IterT;

    InMemoryIndex(BucketManager const& bm,
                  std::filesystem::path const& filename, SHA256* hasher);

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

    std::optional<std::pair<std::streamoff, std::streamoff>>
    getContractCodeRange() const
    {
        return mContractCodeRange;
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