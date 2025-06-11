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
#include "ledger/LedgerTypeUtils.h"
#include "util/HashOfHash.h"
#include "util/NonCopyable.h"
#include "util/types.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-types.h"

namespace stellar
{

// ContractDataCacheT stores a ContractData LedgerEntry and its TTL. TTL is
// stored directly with the data to avoid an additional lookup and save memory.
struct ContractDataCacheT
{
    std::shared_ptr<LedgerEntry const> const ledgerEntry;
    uint32_t const liveUntilLedgerSeq;

    explicit ContractDataCacheT(std::shared_ptr<LedgerEntry const> ledgerEntry,
                                uint32_t liveUntilLedgerSeq)
        : ledgerEntry(std::move(ledgerEntry))
        , liveUntilLedgerSeq(liveUntilLedgerSeq)
    {
    }
};

// InternalContractDataCacheEntry provides a memory-efficient cache
// implementation.
//
// Soroban keys can be quite large (often dominating LedgerEntry size), so
// storing them twice in a traditional key-value map would be wasteful. Instead,
// we use std::unordered_set since LedgerEntry contains both key and value data.
//
// Since C++17's unordered_set doesn't support heterogeneous lookup (searching
// with a different type than stored), we use polymorphism to enable key-only
// lookups without constructing full entries. This will be simplified when we
// upgrade to C++20.
//
// We index entries by their TTL key (SHA256 hash of the ContractData key)
// rather than the full ContractData key. This lets us look up both ContractData
// entries and their TTLs with one index.
//
// Logical map structure:
//     TTLKey -> <std::shared_ptr<LedgerEntry>, liveUntilLedgerSeq>
//
class InternalContractDataCacheEntry
{
  private:
    // Abstract base class for polymorphic entry handling.
    // This allows QueryKey and ValueEntry to be used interchangeably in the
    // set.
    struct AbstractEntry
    {
        virtual ~AbstractEntry() = default;

        // Returns the TTL key (SHA256 hash) that indexes this entry.
        // For ContractData entries, this is getTTLKey(ledgerKey).ttl().keyHash
        // For TTL queries, this is directly the keyHash from the TTL key
        virtual uint256 copyKey() const = 0;

        // Computes hash for unordered_set storage.
        // Note: This returns size_t for STL compatibility, not the uint256 key
        virtual size_t hash() const = 0;

        // Returns the cached data. Only valid for ValueEntry instances.
        virtual ContractDataCacheT const& get() const = 0;

        // Equality comparison based on TTL keys
        virtual bool
        operator==(AbstractEntry const& other) const
        {
            return copyKey() == other.copyKey();
        }
    };

    // ValueEntry stores actual ContractData entries in the cache.
    // Contains both the LedgerEntry and its TTL information.
    struct ValueEntry : public AbstractEntry
    {
      private:
        ContractDataCacheT entry;

      public:
        ValueEntry(std::shared_ptr<LedgerEntry const> ledgerEntry,
                   uint32_t liveUntilLedgerSeq)
            : entry(ledgerEntry, liveUntilLedgerSeq)
        {
        }

        uint256
        copyKey() const override
        {
            auto ttlKey = getTTLKey(LedgerEntryKey(*entry.ledgerEntry));
            return ttlKey.ttl().keyHash;
        }

        size_t
        hash() const override
        {
            return std::hash<uint256>{}(copyKey());
        }

        ContractDataCacheT const&
        get() const override
        {
            return entry;
        }
    };

    // QueryKey is a lightweight key-only entry used for cache lookups.
    struct QueryKey : public AbstractEntry
    {
      private:
        uint256 const ledgerKeyHash;

      public:
        explicit QueryKey(uint256 const& ledgerKeyHash)
            : ledgerKeyHash(ledgerKeyHash)
        {
        }

        uint256
        copyKey() const override
        {
            return ledgerKeyHash;
        }

        size_t
        hash() const override
        {
            return std::hash<uint256>{}(ledgerKeyHash);
        }

        // Should never be called - QueryKey is only for lookups
        ContractDataCacheT const&
        get() const override
        {
            throw std::runtime_error(
                "QueryKey::get() called - this is a logic error");
        }
    };

    std::unique_ptr<AbstractEntry> impl;

  public:
    // Creates a ValueEntry from a LedgerEntry (copies the entry)
    InternalContractDataCacheEntry(LedgerEntry const& ledgerEntry,
                                   uint32_t liveUntilLedgerSeq)
        : impl(std::make_unique<ValueEntry>(
              std::make_shared<LedgerEntry const>(ledgerEntry),
              liveUntilLedgerSeq))
    {
    }

    // Creates a ValueEntry from a shared_ptr (avoids copying)
    InternalContractDataCacheEntry(
        std::shared_ptr<LedgerEntry const> ledgerEntry,
        uint32_t liveUntilLedgerSeq)
        : impl(std::make_unique<ValueEntry>(ledgerEntry, liveUntilLedgerSeq))
    {
    }

    // Creates a QueryKey for lookups. Accepts both CONTRACT_DATA and TTL keys.
    // For CONTRACT_DATA keys, converts to TTL key hash.
    // For TTL keys, uses the hash directly.
    explicit InternalContractDataCacheEntry(LedgerKey const& ledgerKey)
    {
        if (ledgerKey.type() == CONTRACT_DATA)
        {
            auto ttlKey = getTTLKey(ledgerKey);
            impl = std::make_unique<QueryKey>(ttlKey.ttl().keyHash);
        }
        else if (ledgerKey.type() == TTL)
        {
            impl = std::make_unique<QueryKey>(ledgerKey.ttl().keyHash);
        }
        else
        {
            throw std::runtime_error("Invalid ledger key type for cache entry");
        }
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

// LedgerStateCache provides an efficient cache for Soroban contract state.
//
// This includes contract data entries, their TTL, and contract code TTL (WASM
// modules are cached seperatedly).
//
// The cache logically provides two maps:
//   - ContractData: TTLKey -> (LedgerEntry, liveUntilLedgerSeq)
//   - ContractCode: TTLKey -> liveUntilLedgerSeq
//
// Implementation notes:
// - We don't store ContractData TTL entries explicitly, liveUntilLedgerSeq is
// stored with the ContractData entry
// - We don't store WASM code - just track ContractCode TTLs
// - During initialization, TTLs may arrive before their corresponding data
//   entries, so mTTLs temporarily holds these orphaned TTLs
//
// This class is NOT thread-safe and should only be accessed and populated from
// the apply thread.
class LedgerStateCache : public NonMovableOrCopyable
{
#ifdef BUILD_TESTS
  public:
#endif

    // Primary storage for ContractData entries with embedded TTL information.
    // Uses unordered_set with custom entries to save memory vs traditional map.
    std::unordered_set<InternalContractDataCacheEntry,
                       InternalContractDataEntryHash>
        mContractDataEntries;

    // Storage for ContractCode TTLs (we don't need to cache WASM blobs, those
    // are handled by the module cache). During initialization, also temporarily
    // stores orphaned ContractData TTLs that arrive before their corresponding
    // data entries.
    std::unordered_map<uint256, uint32_t> mTTLs;

  private:
    // Helper to update an existing ContractData entry's TTL without changing
    // data
    void updateContractDataTTL(
        std::unordered_set<InternalContractDataCacheEntry,
                           InternalContractDataEntryHash>::iterator dataIt,
        uint32_t newLiveUntilLedgerSeq);

  public:
    // Creates new TTL entry. Throws if a non-zero TTL value at the key already
    // exists. LedgerEntry must be of type TTL.
    void createTTL(LedgerEntry const& ttlEntry);

    // Creates new ContractData entry. Throws if key already exists.
    void createContractDataEntry(LedgerEntry const& ledgerEntry);

    // Update the TTL of an existing ContractData or ContractCode entry. Throws
    // if the key does not exist. LedgerEntry must be of type TTL. We don't know
    // if a TTL maps to a ContractData or ContractCode entry, so we will check
    // both mEntries and mTTLs.
    void updateTTL(LedgerEntry const& ttlEntry);

    // Updates an existing ContractData entry. Throws if the key does
    // not exist. LedgerEntry must be of type CONTRACT_DATA.
    void updateContractData(LedgerEntry const& ledgerEntry);

    // Evicts a TTL key from the cache. LedgerKey must be of type TTL.
    void evictTTL(LedgerKey const& ledgerKey);

    // Evicts a ContractData entry from the cache. LedgerKey must be of type
    // CONTRACT_DATA.
    void evictContractData(LedgerKey const& ledgerKey);

    // Returns nullopt if the entry is not in the cache. LedgerKey must be of
    // type CONTRACT_DATA.
    std::optional<ContractDataCacheT>
    getContractDataEntry(LedgerKey const& ledgerKey) const;

    // Returns nullopt if the entry is not in the cache. LedgerKey must be of
    // type CONTRACT_CODE.
    std::optional<uint32_t>
    getContractCodeTTL(LedgerKey const& ledgerKey) const;

    // Returns true if the given TTL entry exists in the cache. LedgerKey must
    // be of type TTL.
    bool hasTTL(LedgerKey const& ledgerKey) const;
};
}
