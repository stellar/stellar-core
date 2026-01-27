// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/HerderUtils.h"
#include "crypto/KeyUtils.h"
#include "lib/json/json.h"
#include "main/Config.h"
#include "rust/RustVecXdrMarshal.h"
#include "scp/LocalNode.h"
#include "scp/Slot.h"
#include "xdr/Stellar-contract.h"
#include "xdr/Stellar-ledger.h"
#include "xdrpp/types.h"
#include <algorithm>
namespace stellar
{

bool
toStellarValue(Value const& v, StellarValue& sv)
{
    try
    {
        xdr::xdr_from_opaque(v, sv);
    }
    catch (...)
    {
        return false;
    }
    return true;
}

std::optional<std::vector<Hash>>
getTxSetHashes(SCPEnvelope const& envelope)
{
    auto maybeValues = getStellarValues(envelope.statement);
    if (!maybeValues.has_value())
    {
        return std::nullopt;
    }
    auto const& values = maybeValues.value();
    auto result = std::vector<Hash>{};
    result.resize(values.size());

    std::transform(std::begin(values), std::end(values), std::begin(result),
                   [](StellarValue const& sv) { return sv.txSetHash; });

    return result;
}

std::vector<Hash>
getValidatedTxSetHashes(SCPEnvelope const& envelope)
{
    auto const maybeHashes = getTxSetHashes(envelope);
    if (!maybeHashes.has_value())
    {
        SCPStatement const& st = envelope.statement;
        throw std::runtime_error(
            fmt::format("SCPEnvelope from node {} for slot {} contains invalid "
                        "StellarValues",
                        KeyUtils::toStrKey(st.nodeID), st.slotIndex));
    }
    return maybeHashes.value();
}

std::optional<std::vector<StellarValue>>
getStellarValues(SCPStatement const& statement)
{
    auto values = Slot::getStatementValues(statement);
    auto result = std::vector<StellarValue>{};

    for (auto const& v : values)
    {
        StellarValue sv{};
        if (!toStellarValue(v, sv))
        {
            return std::nullopt;
        }
        result.push_back(sv);
    }
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

QuorumIntersectionChecker::QuorumSetMap
parseQuorumMapFromJson(std::string const& jsonPath)
{
    std::ifstream in(jsonPath);
    if (!in)
    {
        throw std::runtime_error("Could not open file '" + jsonPath + "'");
    }
    Json::Reader rdr;
    Json::Value quorumJson;
    if (!rdr.parse(in, quorumJson) || !quorumJson.isObject())
    {
        throw std::runtime_error("Failed to parse '" + jsonPath +
                                 "' as a JSON object");
    }

    Json::Value const& nodesJson = quorumJson["nodes"];
    if (!nodesJson.isArray())
    {
        throw std::runtime_error("JSON field 'nodes' must be an array");
    }

    QuorumIntersectionChecker::QuorumSetMap qmap;
    for (Json::Value const& nodeJson : nodesJson)
    {
        if (!nodeJson.isMember("node") || !nodeJson["node"].isString())
        {
            throw std::runtime_error(
                "JSON field 'node' must exist and be a string");
        }
        NodeID id = KeyUtils::fromStrKey<NodeID>(nodeJson["node"].asString());
        if (!nodeJson.isMember("qset") || !nodeJson["qset"].isObject())
        {
            throw std::runtime_error(
                "JSON field 'qset' must exist and be an object");
        }
        auto elemPair = qmap.try_emplace(
            id, nodeJson["qset"].empty()
                    ? nullptr
                    : std::make_shared<SCPQuorumSet>(
                          LocalNode::fromJson(nodeJson["qset"])));
        if (!elemPair.second)
        {
            throw std::runtime_error(
                "JSON contains multiple nodes with the same 'node' value");
        }
    }
    return qmap;
}

}
