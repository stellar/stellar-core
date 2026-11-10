// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "xdr/Stellar-types.h"
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
// agrees on both computes the same schedule: with a symmetric quorum, seeding
// with the hash of the last closed ledger header gives the whole network the
// same leader for each round of the next ledger.
std::vector<NodeID> computeLeaderSchedule(Hash const& seed,
                                          std::vector<NodeID> nodes);
}
