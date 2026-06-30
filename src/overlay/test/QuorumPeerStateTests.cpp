// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SecretKey.h"
#include "overlay/QuorumPeerState.h"
#include "test/Catch2.h"

namespace stellar
{

namespace
{

NodeID
nodeID()
{
    return SecretKey::pseudoRandomForTesting().getPublicKey();
}

}

TEST_CASE("quorum peer state reconcile", "[overlay][QuorumPeerState]")
{
    auto n1 = nodeID();
    auto n2 = nodeID();
    auto n3 = nodeID();

    QuorumPeerState state;
    state.reconcile({n1, n2});

    REQUIRE(state.getInfo().size() == 2);
    REQUIRE(state.getInfo(n1)->remoteRole == RemoteQsetRole::Unknown);
    REQUIRE(state.getInfo(n2)->remoteRole == RemoteQsetRole::Unknown);

    state.recordHandshake(n1, RemoteQsetRole::Direct,
                          PeerBareAddress{"127.0.0.1", 11625}, 10);
    state.reconcile({n2, n3});

    REQUIRE(!state.getInfo(n1));
    REQUIRE(state.getInfo(n2));
    REQUIRE(state.getInfo(n3));
    REQUIRE(state.getInfo(n3)->remoteRole == RemoteQsetRole::Unknown);
}

TEST_CASE("quorum peer state json round-trip", "[overlay][QuorumPeerState]")
{
    auto n1 = nodeID();
    QuorumPeerState state;
    state.reconcile({n1});
    state.recordHandshake(n1, RemoteQsetRole::Direct,
                          PeerBareAddress{"127.0.0.1", 11625}, 123);

    auto restored = QuorumPeerState::fromJson(state.toJson());
    auto info = restored.getInfo(n1);

    REQUIRE(info);
    REQUIRE(info->remoteRole == RemoteQsetRole::Direct);
    REQUIRE(info->address);
    REQUIRE(info->address->toString() == "127.0.0.1:11625");
    REQUIRE(info->lastConnection == 123);
}

TEST_CASE("quorum peer state expires stale addresses",
          "[overlay][QuorumPeerState]")
{
    auto fresh = nodeID();
    auto stale = nodeID();

    QuorumPeerState state;
    state.reconcile({fresh, stale});
    state.recordHandshake(fresh, RemoteQsetRole::Direct,
                          PeerBareAddress{"127.0.0.1", 11625}, 90);
    state.recordHandshake(stale, RemoteQsetRole::Direct,
                          PeerBareAddress{"127.0.0.1", 11626}, 10);

    auto expired = state.expireStaleAddresses(100, std::chrono::seconds{50});
    REQUIRE(expired.size() == 1);
    REQUIRE(expired.front().first == stale);

    auto staleInfo = state.getInfo(stale);
    REQUIRE(staleInfo);
    REQUIRE(staleInfo->remoteRole == RemoteQsetRole::Unknown);
    REQUIRE(!staleInfo->address);

    auto freshInfo = state.getInfo(fresh);
    REQUIRE(freshInfo);
    REQUIRE(freshInfo->remoteRole == RemoteQsetRole::Direct);
    REQUIRE(freshInfo->address);
}

}
