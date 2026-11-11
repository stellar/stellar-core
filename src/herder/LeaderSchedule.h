// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "xdr/Stellar-ledger.h"
#include "xdr/Stellar-types.h"
#include <optional>
#include <vector>

namespace stellar
{

// Deterministic leader schedule (experimental; replaces SCP nomination).
//
// Returns `nodes` (duplicates removed) ordered by SHA256(seed || nodeID),
// ties (impossible in practice) broken by node ID. Round r (1-based) of the
// slot is led by entry (r - 1) % size, so every node gets a turn before any
// node leads twice.
//
// The order depends only on `seed` and the *set* of nodes, so every node that
// agrees on both computes the same schedule.
std::vector<NodeID> computeLeaderSchedule(Hash const& seed,
                                          std::vector<NodeID> nodes);

// Leader terms: the node whose value closed a ledger leads the next one too,
// for up to LEADER_TERM_LEDGERS ledgers in a row. A stable leader can prepare
// its next transaction set while the previous ledger is still in its ballot
// (see HerderImpl::startProposalPrevalidation).
//
// Terms end on fixed ledger numbers (every multiple of LEADER_TERM_LEDGERS
// starts a new one), so that the schedule of a slot is a function of the
// previous ledger's header alone, as before.
constexpr uint32_t LEADER_TERM_LEDGERS = 5;

// What the ledger `ledgerSeq`, closed with `value`, means for the leader of
// the next ledger.
//  * `mIncumbent`: the node that keeps leading: the node that signed
//    `value`. Set unless the term is over or was forfeited.
//  * `mForfeited`: the node that lost its term early and goes to the end of
//    the next ledger's order: the node that signed `value`, when
//      - `value` is an empty-tx-set value (its leader's transaction set never
//        arrived, so consensus dropped it), or
//      - the ledger applied no transaction (`appliedTransactions` is false).
//        That covers a ledger whose transaction set was found invalid at
//        apply, and an honestly empty ledger: nothing is lost by rotating
//        after one.
// Neither is set when the next ledger starts a new term (every multiple of
// LEADER_TERM_LEDGERS) or `value` is unsigned (the genesis ledger).
//
// A leader whose round times out loses the term as well, without being named
// here: the leader of the round that closes the ledger signs the value it
// proposes (see HerderSCPDriver::reproposeValue), and so becomes the
// incumbent.
struct LeaderTermOutcome
{
    std::optional<NodeID> mIncumbent;
    std::optional<NodeID> mForfeited;
};
LeaderTermOutcome leaderTermOutcome(uint32_t ledgerSeq,
                                    StellarValue const& value,
                                    bool appliedTransactions);

// The same from the header of a closed ledger, where "applied no transaction"
// reads: `txSetResultHash` is the hash of an empty result set. Only checked
// with `ledgersHaveResults`; without it (the test-only overlay-only mode,
// where no ledger records results) every ledger counts as having applied.
LeaderTermOutcome leaderTermOutcome(LedgerHeader const& lcl,
                                    bool ledgersHaveResults);

// The leader schedule of the ledger after `lcl`.
//
// The order is seeded with `lcl.previousLedgerHash`, the hash of the ledger
// *before* `lcl`. `lcl`'s own hash would do as well for agreement, but it is
// only known once `lcl` has closed; with the older hash, the order for the
// next ledger is already known while `lcl` is going through consensus.
//
// An incumbent (see leaderTermOutcome) that is one of `nodes` leads round 1,
// and a node that forfeited its term comes last; the seeded order of the
// other nodes is kept.
std::vector<NodeID> computeNextLeaderSchedule(LedgerHeader const& lcl,
                                              std::vector<NodeID> nodes,
                                              bool ledgersHaveResults);

// The same, for a ledger that has not closed yet: `seed` is the hash of the
// ledger before it and `outcome` what it is expected to mean for its leader.
std::vector<NodeID> computeLeaderSchedule(Hash const& seed,
                                          std::vector<NodeID> nodes,
                                          LeaderTermOutcome const& outcome);
}
