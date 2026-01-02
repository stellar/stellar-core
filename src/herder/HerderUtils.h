// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "herder/QuorumIntersectionChecker.h"
#include "rust/RustBridge.h"
#include "xdr/Stellar-types.h"
#include "xdrpp/marshal.h"
#include <vector>

namespace stellar
{

struct SCPEnvelope;
struct SCPStatement;
struct StellarValue;

// converts a Value into a StellarValue
// returns false on error
bool toStellarValue(Value const& v, StellarValue& sv);

// Extract the transaction set hashes present in `envelope`.
// Returns nullopt if any of the values in the envelope cannot be parsed.
std::optional<std::vector<Hash>> getTxSetHashes(SCPEnvelope const& envelope);

// Like `getTxSetHashes`, but throws if any of the values in the envelope cannot
// be parsed. Use only when it shouldn't be possible for the envelope to contain
// values that cannot be parsed.
std::vector<Hash> getValidatedTxSetHashes(SCPEnvelope const& envelope);

// Extract the values present in `envelope`.
// Returns nullopt if any of the values in the envelope cannot be parsed.
std::optional<std::vector<StellarValue>>
getStellarValues(SCPStatement const& envelope);

std::string toShortString(std::optional<Config> const& cfg, NodeID const& id);

QuorumIntersectionChecker::QuorumSetMap
toQuorumIntersectionMap(QuorumTracker::QuorumMap const& qmap);

QuorumIntersectionChecker::QuorumSetMap
parseQuorumMapFromJson(std::string const& jsonPath);
}
