#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <cstddef>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "bucket/BucketSnapshotManager.h"
#include "ledger/LedgerHashUtils.h"
#include "ledger/LedgerTypeUtils.h"
#include "util/HashOfHash.h"
#include "util/NonCopyable.h"
#include "util/types.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-types.h"

namespace stellar
{

// TTLData stores both liveUntilLedgerSeq and lastModifiedLedgerSeq for TTL
// entries. This allows us to construct a LedgerEntry for TTLs without having to
// redundantly store the keyHash.
struct TTLData
{
    uint32_t liveUntilLedgerSeq;
    uint32_t lastModifiedLedgerSeq;

    TTLData(uint32_t liveUntilLedgerSeq, uint32_t lastModifiedLedgerSeq)
        : liveUntilLedgerSeq(liveUntilLedgerSeq)
        , lastModifiedLedgerSeq(lastModifiedLedgerSeq)
    {
    }

    TTLData() : liveUntilLedgerSeq(0), lastModifiedLedgerSeq(0)
    {
    }

    bool isDefault() const;
};

// ContractDataMapEntryT stores a ContractData LedgerEntry and its TTL. TTL is
// stored directly with the data to avoid an additional lookup and save memory.
struct ContractDataMapEntryT
{
    std::shared_ptr<LedgerEntry const> const ledgerEntry;
    TTLData const ttlData;

    explicit ContractDataMapEntryT(
        std::shared_ptr<LedgerEntry const>&& ledgerEntry, TTLData ttlData)
        : ledgerEntry(std::move(ledgerEntry)), ttlData(ttlData)
    {
    }
};

// ContractCodeMapEntryT stores a ContractCode LedgerEntry and its TTL.
struct ContractCodeMapEntryT
{
    std::shared_ptr<LedgerEntry const> ledgerEntry;
    TTLData ttlData;

    explicit ContractCodeMapEntryT(
        std::shared_ptr<LedgerEntry const>&& ledgerEntry, TTLData ttlData)
        : ledgerEntry(std::move(ledgerEntry)), ttlData(ttlData)
    {
    }
};

// InternalContractDataMapEntry provides a memory-efficient map
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
class InternalContractDataMapEntry
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

        // Returns the stored data. Only valid for ValueEntry instances.
        virtual ContractDataMapEntryT const& get() const = 0;

        // Equality comparison based on TTL keys
        virtual bool
        operator==(AbstractEntry const& other) const
        {
            return copyKey() == other.copyKey();
        }
    };

    // ValueEntry stores actual ContractData entries in the map.
    // Contains both the LedgerEntry and its TTL information.
    struct ValueEntry : public AbstractEntry
    {
      private:
        ContractDataMapEntryT entry;

      public:
        ValueEntry(std::shared_ptr<LedgerEntry const>&& ledgerEntry,
                   TTLData ttlData)
            : entry(std::move(ledgerEntry), ttlData)
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

        ContractDataMapEntryT const&
        get() const override
        {
            return entry;
        }
    };

    // QueryKey is a lightweight key-only entry used for map lookups.
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
        ContractDataMapEntryT const&
        get() const override
        {
            throw std::runtime_error(
                "QueryKey::get() called - this is a logic error");
        }
    };

    std::unique_ptr<AbstractEntry> impl;

  public:
    // Creates a ValueEntry from a LedgerEntry (copies the entry)
    InternalContractDataMapEntry(LedgerEntry const& ledgerEntry,
                                 TTLData ttlData)
        : impl(std::make_unique<ValueEntry>(
              std::make_shared<LedgerEntry const>(ledgerEntry), ttlData))
    {
    }

    // Creates a ValueEntry from a shared_ptr (avoids copying)
    InternalContractDataMapEntry(
        std::shared_ptr<LedgerEntry const>&& ledgerEntry, TTLData ttlData)
        : impl(std::make_unique<ValueEntry>(std::move(ledgerEntry), ttlData))
    {
    }

    // Creates a QueryKey for lookups. Accepts both CONTRACT_DATA and TTL keys.
    // For CONTRACT_DATA keys, converts to TTL key hash.
    // For TTL keys, uses the hash directly.
    explicit InternalContractDataMapEntry(LedgerKey const& ledgerKey)
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
            throw std::runtime_error(
                "Invalid ledger key type for contract data map entry");
        }
    }

    size_t
    hash() const
    {
        return impl->hash();
    }

    bool
    operator==(InternalContractDataMapEntry const& other) const
    {
        return impl->operator==(*other.impl);
    }

    ContractDataMapEntryT const&
    get() const
    {
        return impl->get();
    }
};

struct InternalContractDataEntryHash
{
    size_t
    operator()(InternalContractDataMapEntry const& entry) const
    {
        return entry.hash();
    }
};

// InMemorySorobanState provides an efficient in-memory map for Soroban contract
// state.
//
// This includes contract data entries, contract code entries, and their TTLs.
//
// This logically provides two maps:
//   - ContractData: TTLKey -> (LedgerEntry, liveUntilLedgerSeq)
//   - ContractCode: TTLKey -> (LedgerEntry, liveUntilLedgerSeq)
//
// Implementation notes:
// - We don't store TTL entries explicitly, liveUntilLedgerSeq is
//   stored with the ContractData/ContractCode entry
// - During initialization, TTLs may arrive before their corresponding data
//   entries, so mPendingTTLs temporarily holds these orphaned TTLs
//
// This class is NOT thread-safe and should only be accessed and populated from
// the apply thread.
class InMemorySorobanState : public NonMovableOrCopyable
{
#ifdef BUILD_TESTS
  public:
#endif

    // Primary storage for ContractData entries with embedded TTL information.
    // Uses unordered_set with custom entries to save memory vs traditional map.
    std::unordered_set<InternalContractDataMapEntry,
                       InternalContractDataEntryHash>
        mContractDataEntries;

    // Storage for ContractCode entries. Maps from TTL key hash to entry, ttl
    // struct. Unlike ContractData, we use a map here because the key size is
    // dominated by LedgerEntry size, so there's no real need for extra
    // complexity.
    std::unordered_map<uint256, ContractCodeMapEntryT> mContractCodeEntries;

    // Temporary storage for orphaned TTLs that arrive before their
    // corresponding data entries during initialization. After initialization,
    // this should be empty.
    std::unordered_map<LedgerKey, LedgerEntry> mPendingTTLs;

    // ledgerSeq which the InMemorySorobanState currently "snapshots".
    uint32_t mLastClosedLedgerSeq = 0;

    // Helper to update an existing ContractData entry's TTL without changing
    // data
    void updateContractDataTTL(
        std::unordered_set<InternalContractDataMapEntry,
                           InternalContractDataEntryHash>::iterator dataIt,
        TTLData newTtlData);

    // Should be called after initialization/updates finish to check consistency
    // invariants.
    void checkUpdateInvariants() const;

    // Returns the TTL entry for the given key, or nullptr if not found.
    // LedgerKey must be of type TTL.
    std::shared_ptr<LedgerEntry const> getTTL(LedgerKey const& ledgerKey) const;

  public:
    static bool isInMemoryType(LedgerKey const& ledgerKey);

    // Creates new TTL entry. Throws if a non-zero TTL value at the key already
    // exists. LedgerEntry must be of type TTL.
    void createTTL(LedgerEntry const& ttlEntry);

    // Creates new ContractData entry. Throws if key already exists.
    void createContractDataEntry(LedgerEntry const& ledgerEntry);

    // Update the TTL of an existing ContractData or ContractCode entry. Throws
    // if the key does not exist. LedgerEntry must be of type TTL. We don't know
    // if a TTL maps to a ContractData or ContractCode entry, so we will check
    // both mContractDataEntries and mContractCodeEntries.
    void updateTTL(LedgerEntry const& ttlEntry);

    // Updates an existing ContractData entry. Throws if the key does
    // not exist. LedgerEntry must be of type CONTRACT_DATA.
    void updateContractData(LedgerEntry const& ledgerEntry);

    // Note: since we store TTLs with there associated entry, there is no
    // explicit evictTTL function.

    // Evicts a ContractData entry from the map. LedgerKey must be of type
    // CONTRACT_DATA.
    void deleteContractData(LedgerKey const& ledgerKey);

    // Returns the entry for the given key, or nullptr if not found.
    std::shared_ptr<LedgerEntry const> get(LedgerKey const& ledgerKey) const;

    // Creates new ContractCode entry. Throws if key already exists.
    void createContractCodeEntry(LedgerEntry const& ledgerEntry);

    // Updates an existing ContractCode entry. Throws if the key does
    // not exist. LedgerEntry must be of type CONTRACT_CODE.
    void updateContractCode(LedgerEntry const& ledgerEntry);

    // Evicts a ContractCode entry from the map. LedgerKey must be of type
    // CONTRACT_CODE.
    void deleteContractCode(LedgerKey const& ledgerKey);

    // Returns true if the given TTL entry exists in the map. LedgerKey must
    // be of type TTL.
    bool hasTTL(LedgerKey const& ledgerKey) const;

    // Initialize the map from a bucket list snapshot
    void initializeStateFromSnapshot(SearchableSnapshotConstPtr snap);

    // Update the map with entries from a ledger close. ledgerSeq must be
    // exactly mLastClosedLedgerSeq + 1.
    void updateState(std::vector<LedgerEntry> const& initEntries,
                     std::vector<LedgerEntry> const& liveEntries,
                     std::vector<LedgerKey> const& deadEntries,
                     LedgerHeader const& lh);

    // Should only be called in manual ledger close paths.
    void manuallyAdvanceLedgerHeader(LedgerHeader const& lh);
};
}
