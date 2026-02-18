// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "bucket/BucketSnapshotManager.h"
#include "invariant/InvariantManagerImpl.h"
#include "ledger/LedgerTypeUtils.h"
#include "util/types.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-types.h"

namespace stellar
{

class InvariantManagerImpl;
class SorobanMetrics;

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
    // We store the current in-memory size for the contract code (including
    // its parsed module that is stored in the ModuleCache) in order to both
    // make the contract code updates faster, and also make them more
    // resilient to protocol and config upgrades (otherwise we would need to
    // always pass two configs for any update of the code entry).
    uint32_t sizeBytes;

    explicit ContractCodeMapEntryT(
        std::shared_ptr<LedgerEntry const>&& ledgerEntry, TTLData ttlData,
        uint32_t sizeBytes)
        : ledgerEntry(std::move(ledgerEntry))
        , ttlData(ttlData)
        , sizeBytes(sizeBytes)
    {
    }
};

// InternalContractDataMap provides a memory-efficient map implementation.
//
// Soroban keys can be quite large (often dominating LedgerEntry size), so
// storing them twice in a traditional key-value map would be wasteful. Instead,
// we use std::unordered_set since LedgerEntry contains both key and value data.
//
// We index entries by their TTL key (SHA256 hash of the ContractData key)
// rather than the full ContractData key. This lets us look up both ContractData
// entries and their TTLs with one index.
//
// Logical map structure:
//     TTLKey -> <std::shared_ptr<LedgerEntry>, liveUntilLedgerSeq>
using InternalContractDataMap =
    std::unordered_set<ContractDataMapEntryT,
                       struct InternalContractDataEntryHash,
                       struct InternalContractDataEntryEqual>;

inline Hash
getInternalContractDataMapHash(ContractDataMapEntryT const& entry)
{
    return getTTLKey(LedgerEntryKey(*entry.ledgerEntry)).ttl().keyHash;
}

inline Hash
getInternalContractDataMapHash(LedgerEntry const& entry)
{
    return getTTLKey(LedgerEntryKey(entry)).ttl().keyHash;
}

inline Hash
getInternalContractDataMapHash(LedgerKey const& key)
{
    if (key.type() == CONTRACT_DATA)
    {
        auto ttlKey = getTTLKey(key);
        return ttlKey.ttl().keyHash;
    }
    else if (key.type() == TTL)
    {
        return key.ttl().keyHash;
    }
    else
    {
        throw std::runtime_error(
            "Invalid ledger key type for contract data map entry hash");
    }
}

template <typename T>
concept IsInternalContractDataMapType =
    std::same_as<T, ContractDataMapEntryT> || std::same_as<T, LedgerEntry> ||
    std::same_as<T, LedgerKey>;

struct InternalContractDataEntryHash
{
    using is_transparent = void;

    size_t
    operator()(IsInternalContractDataMapType auto const& entry) const
    {
        return std::hash<uint256>()(getInternalContractDataMapHash(entry));
    }
};

struct InternalContractDataEntryEqual
{
    using is_transparent = void;

    bool
    operator()(IsInternalContractDataMapType auto const& lhs,
               IsInternalContractDataMapType auto const& rhs) const
    {
        return getInternalContractDataMapHash(lhs) ==
               getInternalContractDataMapHash(rhs);
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
// This class is NOT thread-safe by default. While multiple threads may call
// const methods concurrently, there is no synchronization or locks. It is the
// caller's responsibility to ensure that no thread is reading state when any
// non-const function is called.
class InMemorySorobanState
{
#ifdef BUILD_TESTS
  public:
#endif

    // Primary storage for ContractData entries with embedded TTL information.
    // Uses unordered_set with custom entries to save memory vs traditional map.
    InternalContractDataMap mContractDataEntries;

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

    // Total size of the in-memory state in bytes as defined by the protocol (
    // including using the in-memory module size for the ContractCode entries).
    // Note, that these are int64 and not uint64 even though we store this in
    // ledger as uint64 - neither of the type limits is realistically
    // reachable, but signed int makes math simpler and safer.
    int64_t mContractCodeStateSize = 0;
    int64_t mContractDataStateSize = 0;

    // Helper to update an existing ContractData entry's TTL without changing
    // data
    void updateContractDataTTL(InternalContractDataMap::iterator dataIt,
                               TTLData newTtlData);

    // Should be called after initialization/updates finish to check consistency
    // invariants.
    void checkUpdateInvariants() const;

    void updateStateSizeOnEntryUpdate(uint32_t oldEntrySize,
                                      uint32_t newEntrySize,
                                      bool isContractCode);

    // Returns the TTL entry for the given key, or nullptr if not found.
    // LedgerKey must be of type TTL.
    std::shared_ptr<LedgerEntry const> getTTL(LedgerKey const& ledgerKey) const;

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

    // Creates new ContractCode entry. Throws if key already exists.
    void createContractCodeEntry(LedgerEntry const& ledgerEntry,
                                 SorobanNetworkConfig const& sorobanConfig,
                                 uint32_t ledgerVersion);

    // Updates an existing ContractCode entry. Throws if the key does
    // not exist. LedgerEntry must be of type CONTRACT_CODE.
    void updateContractCode(LedgerEntry const& ledgerEntry,
                            SorobanNetworkConfig const& sorobanConfig,
                            uint32_t ledgerVersion);

    // Evicts a ContractCode entry from the map. LedgerKey must be of type
    // CONTRACT_CODE.
    void deleteContractCode(LedgerKey const& ledgerKey);

    void reportMetrics(SorobanMetrics& metrics) const;

  public:
    InMemorySorobanState() = default;
    InMemorySorobanState(InMemorySorobanState const& other);

    // These following functions are read-only and may be called concurrently so
    // long as no updates are occurring.
    static bool isInMemoryType(LedgerKey const& ledgerKey);

    // Returns true if the given TTL entry exists in the map. LedgerKey must
    // be of type TTL.
    bool hasTTL(LedgerKey const& ledgerKey) const;

    bool isEmpty() const;

    uint32_t getLedgerSeq() const;

    // Asserts that the internal ledgerSeq matches the expected value.
    void assertLastClosedLedger(uint32_t expectedLedgerSeq) const;

    // Returns the total size of the in-memory Soroban state to be used for the
    // rent fee computation purposes.
    // Note, that this size depends on in-memory cost for ContractCode entries.
    // Thus it has to be updated via `recomputeContractCodeSize` when the
    // memory config settings have been changed, or protocol version has been
    // updated.
    uint64_t getSize() const;

    // Returns the entry for the given key, or nullptr if not found.
    std::shared_ptr<LedgerEntry const> get(LedgerKey const& ledgerKey) const;

    // Returns the number of CONTRACT_DATA entries stored.
    size_t getContractDataEntryCount() const;

    // Returns the number of CONTRACT_CODE entries stored.
    size_t getContractCodeEntryCount() const;

    // The following functions are not read-only and must never be called
    // concurrently. It is the caller's responsibility to ensure that no thread
    // is reading state when these functions are called.

    // Initialize the map from a bucket list snapshot
    void initializeStateFromSnapshot(SearchableSnapshotConstPtr snap,
                                     uint32_t ledgerVersion);

    // Update the map with entries from a ledger close. ledgerSeq must be
    // exactly mLastClosedLedgerSeq + 1.
    void
    updateState(std::vector<LedgerEntry> const& initEntries,
                std::vector<LedgerEntry> const& liveEntries,
                std::vector<LedgerKey> const& deadEntries,
                LedgerHeader const& lh,
                std::optional<SorobanNetworkConfig const> const& sorobanConfig,
                SorobanMetrics& metrics);

    // Should only be called in manual ledger close paths.
    void manuallyAdvanceLedgerHeader(LedgerHeader const& lh);

    // Recomputes the size of all the stored ContractCode entries and updates
    // the state size accordingly.
    // Note, that while this should be *reasonably* fast to be done every once
    // in a while during the protocol upgrades, we shouldn't call this 'just in
    // case' in order to avoid unnecessary performance overhead.
    void recomputeContractCodeSize(SorobanNetworkConfig const& sorobanConfig,
                                   uint32_t ledgerVersion);

#ifdef BUILD_TESTS
    void clearForTesting();
#endif
};
}
