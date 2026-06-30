// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/QuorumPeerState.h"

#include "crypto/KeyUtils.h"
#include "lib/json/json.h"

#include <algorithm>
#include <cstdlib>
#include <regex>
#include <stdexcept>

namespace stellar
{

namespace
{

std::string
roleToString(RemoteQsetRole role)
{
    switch (role)
    {
    case RemoteQsetRole::Unknown:
        return "unknown";
    case RemoteQsetRole::None:
        return "none";
    case RemoteQsetRole::Direct:
        return "direct";
    default:
        throw std::runtime_error("invalid remote qset role");
    }
}

RemoteQsetRole
roleFromString(std::string const& role)
{
    if (role == "unknown")
    {
        return RemoteQsetRole::Unknown;
    }
    if (role == "none")
    {
        return RemoteQsetRole::None;
    }
    if (role == "direct")
    {
        return RemoteQsetRole::Direct;
    }
    throw std::runtime_error("invalid remote qset role");
}

std::optional<PeerBareAddress>
addressFromString(std::string const& address)
{
    static std::regex re(
        "^(\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3})\\:(\\d{1,5})$");
    std::smatch m;
    if (!std::regex_search(address, m, re) || m.empty())
    {
        return std::nullopt;
    }

    int parsedPort = atoi(m[2].str().c_str());
    if (parsedPort <= 0 || parsedPort > UINT16_MAX)
    {
        return std::nullopt;
    }

    try
    {
        return PeerBareAddress{m[1].str(),
                               static_cast<unsigned short>(parsedPort)};
    }
    catch (...)
    {
        return std::nullopt;
    }
}

}

void
QuorumPeerState::reconcile(std::set<NodeID> const& directQset)
{
    for (auto it = mInfo.begin(); it != mInfo.end();)
    {
        if (directQset.count(it->first) == 0)
        {
            it = mInfo.erase(it);
        }
        else
        {
            ++it;
        }
    }

    for (auto const& nodeID : directQset)
    {
        mInfo.emplace(nodeID, QuorumPeerInfo{});
    }
}

void
QuorumPeerState::recordHandshake(NodeID const& nodeID,
                                 RemoteQsetRole remoteRole,
                                 PeerBareAddress const& address,
                                 uint64_t nowSecs)
{
    auto& info = mInfo[nodeID];
    info.remoteRole = remoteRole;
    info.address = address;
    info.lastConnection = nowSecs;
}

std::vector<std::pair<NodeID, QuorumPeerInfo>>
QuorumPeerState::expireStaleAddresses(uint64_t nowSecs,
                                      std::chrono::seconds ttl)
{
    std::vector<std::pair<NodeID, QuorumPeerInfo>> expired;
    auto const ttlSecs = static_cast<uint64_t>(ttl.count());

    for (auto& [nodeID, info] : mInfo)
    {
        if ((info.address || info.remoteRole != RemoteQsetRole::Unknown) &&
            info.lastConnection != 0 && nowSecs > info.lastConnection &&
            nowSecs - info.lastConnection > ttlSecs)
        {
            expired.emplace_back(nodeID, info);
            info.remoteRole = RemoteQsetRole::Unknown;
            info.address.reset();
        }
    }

    return expired;
}

std::string
QuorumPeerState::toJson() const
{
    Json::Value root;
    root["peers"] = Json::arrayValue;

    std::vector<NodeID> nodeIDs;
    nodeIDs.reserve(mInfo.size());
    for (auto const& [nodeID, _] : mInfo)
    {
        nodeIDs.push_back(nodeID);
    }
    std::sort(nodeIDs.begin(), nodeIDs.end());

    for (auto const& nodeID : nodeIDs)
    {
        auto const& info = mInfo.at(nodeID);
        Json::Value peer;
        peer["nodeID"] = KeyUtils::toStrKey(nodeID);
        peer["remoteRole"] = roleToString(info.remoteRole);
        peer["lastConnection"] = static_cast<Json::UInt64>(info.lastConnection);
        if (info.address)
        {
            peer["address"] = info.address->toString();
        }
        root["peers"].append(peer);
    }

    return Json::FastWriter().write(root);
}

QuorumPeerState
QuorumPeerState::fromJson(std::string const& json)
{
    QuorumPeerState state;
    if (json.empty())
    {
        return state;
    }

    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(json, root) || !root.isObject() ||
        !root["peers"].isArray())
    {
        return state;
    }

    for (auto const& peer : root["peers"])
    {
        try
        {
            auto nodeID =
                KeyUtils::fromStrKey<NodeID>(peer["nodeID"].asString());
            QuorumPeerInfo info;
            info.remoteRole = roleFromString(peer["remoteRole"].asString());
            info.lastConnection = peer["lastConnection"].asUInt64();
            if (peer.isMember("address"))
            {
                info.address = addressFromString(peer["address"].asString());
            }
            state.mInfo[nodeID] = info;
        }
        catch (...)
        {
        }
    }

    return state;
}

UnorderedMap<NodeID, QuorumPeerInfo> const&
QuorumPeerState::getInfo() const
{
    return mInfo;
}

std::optional<QuorumPeerInfo>
QuorumPeerState::getInfo(NodeID const& nodeID) const
{
    auto it = mInfo.find(nodeID);
    if (it == mInfo.end())
    {
        return std::nullopt;
    }
    return it->second;
}

}
