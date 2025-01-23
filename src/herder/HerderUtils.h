#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

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

template <typename T>
std::vector<uint8_t>
toVec(T const& t)
{
    return std::vector<uint8_t>(xdr::xdr_to_opaque(t));
}

template <typename T>
CxxBuf
toCxxBuf(T const& t)
{
    return CxxBuf{std::make_unique<std::vector<uint8_t>>(toVec(t))};
}

std::vector<Hash> getTxSetHashes(SCPEnvelope const& envelope);
std::vector<StellarValue> getStellarValues(SCPStatement const& envelope);
std::string toShortString(std::optional<Config> const& cfg, NodeID const& id);
QuorumIntersectionChecker::QuorumSetMap
toQuorumIntersectionMap(QuorumTracker::QuorumMap const& qmap);
std::pair<std::vector<PublicKey>, std::vector<PublicKey>>
toQuorumSplitNodeIDs(QuorumSplit& split);
}
