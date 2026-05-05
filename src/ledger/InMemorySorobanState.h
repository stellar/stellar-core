// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "rust/RustBridge.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-ledger.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace stellar
{
class SorobanNetworkConfig;

// InMemorySorobanState is a thin C++ shim over a Rust-owned SorobanState
// (see src/rust/src/soroban_apply.rs). All storage-specific logic lives on
// the Rust side; this class only marshals between C++ XDR types and the
// FFI byte buffers.
//
// Concurrency: the Rust side enforces that mutating methods require &mut
// access; cxx surfaces this as taking the C++ object non-const. Read methods
// are safe to call concurrently as long as no mutator runs.
class InMemorySorobanState
{
  private:
    rust::Box<rust_bridge::SorobanState> mState;

  public:
    InMemorySorobanState();

    // The shim deliberately doesn't expose copy semantics. The previous
    // C++ implementation deep-cloned the entry maps; that path is no longer
    // needed (the few remaining callers don't take a copy). Move is allowed
    // since rust::Box is movable.
    InMemorySorobanState(InMemorySorobanState const&) = delete;
    InMemorySorobanState& operator=(InMemorySorobanState const&) = delete;
    InMemorySorobanState(InMemorySorobanState&&) = default;
    InMemorySorobanState& operator=(InMemorySorobanState&&) = default;
    ~InMemorySorobanState() = default;

    // True if the given key targets one of the entry types stored in this
    // cache (CONTRACT_DATA, CONTRACT_CODE, TTL).
    static bool isInMemoryType(LedgerKey const& ledgerKey);

    // Returns true iff the given TTL key has a stored TTL value.
    bool hasTTL(LedgerKey const& ledgerKey) const;

    bool isEmpty() const;
    uint32_t getLedgerSeq() const;

    // Asserts the internal ledger seq matches the expected value.
    void assertLastClosedLedger(uint32_t expectedLedgerSeq) const;

    // Total in-memory state size (used by the rent-fee accounting path).
    uint64_t getSize() const;

    // Returns the entry for the given key, or nullptr if not found. The
    // returned shared_ptr owns a fresh deserialized LedgerEntry constructed
    // on the C++ side from the XDR bytes returned by Rust.
    std::shared_ptr<LedgerEntry const> get(LedgerKey const& ledgerKey) const;

    size_t getContractDataEntryCount() const;
    size_t getContractCodeEntryCount() const;

    // Initialize the in-memory state by reading the live-bucket files
    // directly from disk. The state must be empty when called. Replaces
    // the old initializeStateFromSnapshot path: bucket-list iteration,
    // dedup, and per-entry size compute all happen on the Rust side.
    //
    // `bucketPaths` are the filesystem paths to the live-bucket .xdr files
    // in priority order (level 0 curr, level 0 snap, level 1 curr, level 1
    // snap, ...). Empty/missing files are tolerated. Pre-Soroban protocols
    // (`ledgerVersion < 20`) only set the ledger seq.
    void initializeFromBucketFiles(
        std::vector<std::string> const& bucketPaths,
        uint32_t lastClosedLedgerSeq, uint32_t ledgerVersion,
        std::optional<SorobanNetworkConfig const> const& sorobanConfig);

    // Manually set the last-closed-ledger header. Used by ledger-replay
    // setup paths.
    void manuallyAdvanceLedgerHeader(LedgerHeader const& lh);

    // Mutable access to the underlying Rust SorobanState handle, used by
    // the LedgerManagerImpl apply path to call the Rust-side
    // apply_soroban_phase bridge function (which mutates the state in
    // place). External callers should prefer the public read/write methods
    // above; this is here only for the apply orchestrator.
    rust::Box<rust_bridge::SorobanState>& getRustStateForBridge();

    // Notify SorobanState of the post-apply eviction events. Walks
    // both vectors and removes the corresponding CONTRACT_DATA /
    // CONTRACT_CODE entries from the in-memory map; TTL keys and
    // non-Soroban keys are ignored. Must be called between the
    // apply phase and bucket-list commit so that next-ledger lookups
    // (and the next apply phase's RestoreFootprint footprint walks)
    // see the same view of state as the live BucketList.
    void evictEntries(std::vector<LedgerEntry> const& archivedEntries,
                      std::vector<LedgerKey> const& deletedKeys);

    // Recompute the cached size_bytes for every stored CONTRACT_CODE entry.
    // The whole iteration plus the per-protocol size compute happens on the
    // Rust side via a single FFI call — no C++ round-trip.
    void recomputeContractCodeSize(SorobanNetworkConfig const& sorobanConfig,
                                   uint32_t ledgerVersion);

#ifdef BUILD_TESTS
    void clearForTesting();
#endif
};
}
