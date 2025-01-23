// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/HerderUtils.h"
#include "crypto/KeyUtils.h"
#include "main/Config.h"
#include "rust/RustVecXdrMarshal.h"
#include "scp/Slot.h"
#include "xdr/Stellar-ledger.h"
#include <algorithm>

namespace stellar
{

std::vector<Hash>
getTxSetHashes(SCPEnvelope const& envelope)
{
    auto values = getStellarValues(envelope.statement);
    auto result = std::vector<Hash>{};
    result.resize(values.size());

    std::transform(std::begin(values), std::end(values), std::begin(result),
                   [](StellarValue const& sv) { return sv.txSetHash; });

    return result;
}

std::vector<StellarValue>
getStellarValues(SCPStatement const& statement)
{
    auto values = Slot::getStatementValues(statement);
    auto result = std::vector<StellarValue>{};
    result.resize(values.size());

    std::transform(std::begin(values), std::end(values), std::begin(result),
                   [](Value const& v) {
                       auto wb = StellarValue{};
                       xdr::xdr_from_opaque(v, wb);
                       return wb;
                   });

    return result;
}

// Render `id` as a short, human readable string. If `cfg` has a value, this
// function uses `cfg` to render the string. Otherwise, it returns the first 5
// hex values `id`.
std::string
toShortString(std::optional<Config> const& cfg, NodeID const& id)
{
    if (cfg)
    {
        return cfg->toShortString(id);
    }
    else
    {
        return KeyUtils::toShortString(id).substr(0, 5);
    }
}

QuorumIntersectionChecker::QuorumSetMap
toQuorumIntersectionMap(QuorumTracker::QuorumMap const& qmap)
{
    QuorumIntersectionChecker::QuorumSetMap ret;
    for (auto const& elem : qmap)
    {
        ret[elem.first] = elem.second.mQuorumSet;
    }
    return ret;
}

std::pair<std::vector<PublicKey>, std::vector<PublicKey>>
toQuorumSplitNodeIDs(QuorumSplit& split)
{
    std::vector<NodeID> leftNodes;
    leftNodes.reserve(split.left.size());
    for (const auto& str : split.left)
    {
        leftNodes.push_back(KeyUtils::fromStrKey<NodeID>(std::string(str)));
    }
    std::vector<NodeID> rightNodes;
    rightNodes.reserve(split.right.size());
    for (const auto& str : split.right)
    {
        rightNodes.push_back(KeyUtils::fromStrKey<NodeID>(std::string(str)));
    }
    return std::make_pair(std::move(leftNodes), std::move(rightNodes));
}

}
