#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <cstddef>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "ledger/LedgerHashUtils.h"
#include "util/NonCopyable.h"
#include "util/types.h"

namespace stellar
{

class LedgerStateCache;

struct ContractDataCacheT
{
    std::shared_ptr<LedgerEntry const> ledgerEntry;
    mutable uint32_t liveUntilLedgerSeq;

    explicit ContractDataCacheT(LedgerEntry const& ledgerEntry,
                                uint32_t liveUntilLedgerSeq)
        : ledgerEntry(std::make_shared<LedgerEntry>(ledgerEntry))
        , liveUntilLedgerSeq(liveUntilLedgerSeq)
    {
    }

  private:
    // This is a little hacky, but we want to be able to just update the TTL
    // without having to re-insert the ledgerEntry as well. This entry will be
    // stored in a set and always be returned const, but we know the ttl value
    // is not used by the hashing or comparison operations, so this is safe.
    void
    updateTTL(uint32_t newLiveUntilLedgerSeq) const
    {
        liveUntilLedgerSeq = newLiveUntilLedgerSeq;
    }

    friend class LedgerStateCache;
};

// Soroban keys sizes usually dominate LedgerEntry size, so we don't want to
// store a key-value map to be memory efficient. Instead, we store a set of
// InternalContractDataCacheEntry objects, which is a wrapper around either a
// LedgerKey or cache entry. This allows us to use std::unordered_set to
// efficiently store cache entries, but allows lookup by key only.
// Note that C++20 allows heterogeneous lookup in unordered_set, so we can
// simplify this class once we upgrade.
class InternalContractDataCacheEntry
{
  private:
    struct AbstractEntry
    {
        virtual ~AbstractEntry() = default;
        virtual LedgerKey copyKey() const = 0;
        virtual size_t hash() const = 0;
        virtual ContractDataCacheT const& get() const = 0;

        virtual bool
        operator==(const AbstractEntry& other) const
        {
            return copyKey() == other.copyKey();
        }
    };

    // "Value" entry type used for storing ContractData entries in cache
    struct ValueEntry : public AbstractEntry
    {
      private:
        ContractDataCacheT entry;

      public:
        ValueEntry(LedgerEntry const& ledgerEntry, uint32_t liveUntilLedgerSeq)
            : entry(ledgerEntry, liveUntilLedgerSeq)
        {
        }

        LedgerKey
        copyKey() const override
        {
            return LedgerEntryKey(*entry.ledgerEntry);
        }

        size_t
        hash() const override
        {
            return std::hash<LedgerKey>{}(LedgerEntryKey(*entry.ledgerEntry));
        }

        ContractDataCacheT const&
        get() const override
        {
            return entry;
        }
    };

    // "Key" entry type only used for querying the cache
    // Warning: We take a reference to the LedgerKey here to avoid extra copies
    // for lookups, so the caller must ensure that the LedgerKey remains valid
    // for the lifetime of the QueryKey object.
    struct QueryKey : public AbstractEntry
    {
      private:
        LedgerKey const& ledgerKey;

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

        ContractDataCacheT const&
        get() const override
        {
            throw std::runtime_error("Called get() on QueryKey");
        }
    };

    std::unique_ptr<AbstractEntry> impl;

  public:
    InternalContractDataCacheEntry(LedgerEntry const& ledgerEntry,
                                   uint32_t liveUntilLedgerSeq)
        : impl(std::make_unique<ValueEntry>(ledgerEntry, liveUntilLedgerSeq))
    {
    }

    InternalContractDataCacheEntry(LedgerKey const& ledgerKey)
        : impl(std::make_unique<QueryKey>(ledgerKey))
    {
    }

    size_t
    hash() const
    {
        return impl->hash();
    }

    bool
    operator==(InternalContractDataCacheEntry const& other) const
    {
        return impl->operator==(*other.impl);
    }

    ContractDataCacheT const&
    get() const
    {
        return impl->get();
    }
};

struct InternalContractDataEntryHash
{
    size_t
    operator()(InternalContractDataCacheEntry const& entry) const
    {
        return entry.hash();
    }
};

// This class caches all Soroban state required for Soroban tx application
// (except for the module cache, which is managed separately). Specifically,
// it caches these LedgerEntry types in the following way:
//
// ContractData: <LedgerEntry, liveUntilLedgerSeq>
// ContractCode: <sizeForFeeCalculation, liveUntilLedgerSeq>
//
// We don't need to store explicit TTL entries, nor do we need to store WASM, we
// just need to keep track of TTLs.
class LedgerStateCache : public NonMovableOrCopyable
{
#ifdef BUILD_TESTS
  public:
#endif

    std::unordered_set<InternalContractDataCacheEntry,
                       InternalContractDataEntryHash>
        mEntries;
    std::unordered_map<LedgerKey, uint32_t> mContractCodeTTLs;

  public:
    // Update the TTL of an existing entry in the cache, or inserts a new entry
    // if no key exists.
    void updateContractCodeTTL(LedgerKey const& ledgerKey,
                               uint32_t liveUntilLedgerSeq);

    // Update ContractData entry, or create a new entry if it doesn't exist.
    // If liveUntilLedgerSeq is not nullopt, update the TTL of the entry.
    // Otherwise, the TTL will remained unchanged if the entry already exists,
    // or will default to 0 if the entry is new.
    void updateContractDataEntry(LedgerEntry const& ledgerEntry,
                                 std::optional<uint32_t> liveUntilLedgerSeq);

    // Update the TTL of an existing ContractData entry. Throws if the entry
    // does not exist.
    void updateContractDataTTL(LedgerKey const& ledgerKey,
                               uint32_t liveUntilLedgerSeq);

    // Evict a key from the cache.
    void evictKey(LedgerKey const& ledgerKey);

    // Returns nullopt if the entry is not in the cache
    std::optional<ContractDataCacheT>
    getContractDataEntry(LedgerKey const& ledgerKey) const;

    // Returns nullopt if the entry is not in the cache
    std::optional<uint32_t>
    getContractCodeTTL(LedgerKey const& ledgerKey) const;
};
}
