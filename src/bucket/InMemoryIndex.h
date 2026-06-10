// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "bucket/BucketIndexUtils.h"
#include "bucket/BucketUtils.h"
#include "bucket/LedgerCmp.h"
#include "xdr/Stellar-ledger-entries.h"

#include "ledger/LedgerHashUtils.h"
#include <unordered_set>

namespace stellar
{

class SHA256;

// LedgerKey sizes usually dominate LedgerEntry size, so we don't want to
// store a key-value map to be memory efficient. Instead, we store a set of
// InternalInMemoryBucketEntry objects, which hold either a cached BucketEntry
// (stored elements) or a pointer to a LedgerKey (transient query objects).
// This allows us to use std::unordered_set to efficiently store cache
// entries, but allows lookup by key only. The key hash is computed once at
// construction and cached; hashing and equality are computed in place against
// the key data embedded in the BucketEntry, without materializing LedgerKey
// copies.
// Note that C++20 allows heterogeneous lookup in unordered_set, so we can
// simplify this class once we upgrade.
class InternalInMemoryBucketEntry
{
  private:
    // Non-null for stored elements.
    IndexPtrT mEntry;
    // Non-null for query objects only. Query objects must not outlive the
    // LedgerKey they reference: they are only intended to be constructed as
    // temporary arguments to find().
    LedgerKey const* mQueryKey{nullptr};
    size_t mHash;

    // Invoke f with this entry's identity key, which is either a LedgerKey
    // or a LedgerEntry::_data_t (both supported by LedgerEntryIdCmp and
    // hashLedgerIdentity).
    template <typename F>
    auto
    visitKey(F&& f) const
    {
        if (mQueryKey)
        {
            return f(*mQueryKey);
        }
        if (mEntry->type() == DEADENTRY)
        {
            return f(mEntry->deadEntry());
        }
        return f(mEntry->liveEntry().data);
    }

  public:
    explicit InternalInMemoryBucketEntry(IndexPtrT entry)
        : mEntry(std::move(entry))
        , mHash(mEntry->type() == DEADENTRY
                    ? hashLedgerIdentity(mEntry->deadEntry())
                    : hashLedgerIdentity(mEntry->liveEntry().data))
    {
    }

    explicit InternalInMemoryBucketEntry(LedgerKey const& ledgerKey)
        : mQueryKey(&ledgerKey), mHash(hashLedgerIdentity(ledgerKey))
    {
    }

    size_t
    hash() const
    {
        return mHash;
    }

    bool
    operator==(InternalInMemoryBucketEntry const& other) const
    {
        return visitKey([&](auto const& a) {
            return other.visitKey([&](auto const& b) {
                LedgerEntryIdCmp cmp;
                return !cmp(a, b) && !cmp(b, a);
            });
        });
    }

    IndexPtrT const&
    get() const
    {
        if (!mEntry)
        {
            throw std::runtime_error("Called get() on query key");
        }
        return mEntry;
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

    // Reserve space for the expected number of entries.
    void reserve(size_t n);

    // Insert a BucketEntry into the cache. The pointer may alias a backing
    // vector (shared ownership keeps the storage alive).
    void insert(IndexPtrT entry);

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
    std::map<LedgerEntryType, std::pair<std::streamoff, std::streamoff>>
        mTypeRanges;

  public:
    using IterT = InMemoryBucketState::IterT;

    InMemoryIndex(BucketManager const& bm,
                  std::filesystem::path const& filename, SHA256* hasher);

    // Builds the index from an in-memory bucket state. The index aliases the
    // entries in the backing vector (sharing ownership of the vector), so the
    // vector must not be mutated after this call.
    InMemoryIndex(BucketManager& bm,
                  std::shared_ptr<std::vector<BucketEntry> const> const&
                      inMemoryState,
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
