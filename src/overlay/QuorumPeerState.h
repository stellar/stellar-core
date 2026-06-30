// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "overlay/PeerBareAddress.h"
#include "util/UnorderedMap.h"
#include "xdr/Stellar-types.h"

#include <chrono>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace stellar
{

enum class RemoteQsetRole : uint32_t
{
    Unknown = 0,
    None = 1,
    Direct = 2
};

struct QuorumPeerInfo
{
    RemoteQsetRole remoteRole{RemoteQsetRole::Unknown};
    std::optional<PeerBareAddress> address;
    uint64_t lastConnection{0};
};

class QuorumPeerState
{
    UnorderedMap<NodeID, QuorumPeerInfo> mInfo;

  public:
    void reconcile(std::set<NodeID> const& directQset);
    void recordHandshake(NodeID const& nodeID, RemoteQsetRole remoteRole,
                         PeerBareAddress const& address, uint64_t nowSecs);
    std::vector<std::pair<NodeID, QuorumPeerInfo>>
    expireStaleAddresses(uint64_t nowSecs, std::chrono::seconds ttl);

    std::string toJson() const;
    static QuorumPeerState fromJson(std::string const& json);

    UnorderedMap<NodeID, QuorumPeerInfo> const& getInfo() const;
    std::optional<QuorumPeerInfo> getInfo(NodeID const& nodeID) const;
};

}
