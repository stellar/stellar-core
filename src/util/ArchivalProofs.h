#pragma once

// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "xdr/Stellar-transaction.h"
#include <xdrpp/types.h>

#include <optional>

namespace stellar
{

class Application;
class SearchableHotArchiveBucketListSnapshot;

// Returns true if the proof structure is valid. This function does not check
// the validity of the proof itself, just that the structure is correct (i.e. no
// duplicate keys being proven).
bool checkCreationProofValidity(xdr::xvector<ArchivalProof> const& proofs);

// Returns true if the new entry being created has a valid proof or if no proof
// is required (i.e. no archival filter misses or type does not require proofs).
// Warning: assumes proofs has valid structure according to
// checkCreationProofValidity
bool isCreatedKeyProven(Application& app, LedgerKey const& lk,
                        xdr::xvector<ArchivalProof> const& proofs);

// Adds a creation proof for lk to proofs. Returns true if a proof was added or
// is not necessary. Returns false if no valid proof exists.
bool addCreationProof(bool simulateBloomMiss, LedgerKey const& lk,
                      xdr::xvector<ArchivalProof>& proofs);

// Returns true if the proof structure is valid. This function does not check
// the validity of the proof itself, just that the structure is correct (i.e. no
// duplicate keys being proven).
bool checkRestorationProofValidity(xdr::xvector<ArchivalProof> const& proofs);

// Returns the LedgerEntry for the given key being restored if valid proof is
// provided or if no proof is required (i.e. entry exists on in HotArchive). If
// the proof for the given key is not valid, returns nullptr. This function will
// check the HotArchive, but will not search the live BucketList. Is is the
// responsibility of the caller to search the live BucketList before calling
// this function. Warning: assumes proofs has valid structure according to
// checkRestorationProofValidity
std::shared_ptr<LedgerEntry>
getRestoredEntryFromProof(Application& app, LedgerKey const& lk,
                          xdr::xvector<ArchivalProof> const& proofs);

// Adds a restoration proof for lk to proofs. Returns true if a proof was added
// or is not necessary. Returns false if no valid proof exists.
// If ledgerSeq is specified, the entry will be restored based on the snapshot
// at that ledger. TODO: Fix race condition when ledgerSeq is populated by
// loading keys in batches.
bool addRestorationProof(
    std::shared_ptr<SearchableHotArchiveBucketListSnapshot> hotArchive,
    LedgerKey const& lk, xdr::xvector<ArchivalProof>& proofs,
    std::optional<uint32_t> ledgerSeq = std::nullopt);
}