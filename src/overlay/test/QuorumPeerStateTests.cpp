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
    auto removed = state.reconcile({n1, n2});
    REQUIRE(removed.empty());

    REQUIRE(state.getInfo().size() == 2);
    REQUIRE(state.getInfo(n1)->remoteRole == RemoteQsetRole::Unknown);
    REQUIRE(state.getInfo(n2)->remoteRole == RemoteQsetRole::Unknown);

    state.recordHandshake(n1, RemoteQsetRole::Direct,
                          PeerBareAddress{"127.0.0.1", 11625}, 10);
    removed = state.reconcile({n2, n3});

    REQUIRE(!state.getInfo(n1));
    REQUIRE(state.getInfo(n2));
    REQUIRE(state.getInfo(n3));
    REQUIRE(state.getInfo(n3)->remoteRole == RemoteQsetRole::Unknown);

    // The removed entry is returned with its last known state so the caller
    // can demote its peer record.
    REQUIRE(removed.size() == 1);
    REQUIRE(removed.front().first == n1);
    REQUIRE(removed.front().second.address);
    REQUIRE(removed.front().second.address->toString() == "127.0.0.1:11625");
}

TEST_CASE("quorum peer state reports address changes",
          "[overlay][QuorumPeerState]")
{
    auto n1 = nodeID();
    QuorumPeerState state;
    state.reconcile({n1});

    auto oldAddress = PeerBareAddress{"127.0.0.1", 11625};
    auto newAddress = PeerBareAddress{"127.0.0.2", 11625};

    REQUIRE(!state.recordHandshake(n1, RemoteQsetRole::Direct, oldAddress, 10));
    // Same address: no change reported
    REQUIRE(!state.recordHandshake(n1, RemoteQsetRole::Direct, oldAddress, 20));
    // New address: previous address is returned
    auto previous =
        state.recordHandshake(n1, RemoteQsetRole::Direct, newAddress, 30);
    REQUIRE(previous);
    REQUIRE(*previous == oldAddress);
    REQUIRE(state.getInfo(n1)->address == newAddress);
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

TEST_CASE("quorum peer state does not expire non-mutual peers",
          "[overlay][QuorumPeerState]")
{
    auto nonMutual = nodeID();

    QuorumPeerState state;
    state.reconcile({nonMutual});
    state.recordHandshake(nonMutual, RemoteQsetRole::None,
                          PeerBareAddress{"127.0.0.1", 11625}, 10);

    // None entries must never cycle back to Unknown: rediscovery is useless
    // (we are not chasing them) and would cause perpetual re-probing.
    auto expired = state.expireStaleAddresses(1000000, std::chrono::seconds{50});
    REQUIRE(expired.empty());

    auto info = state.getInfo(nonMutual);
    REQUIRE(info);
    REQUIRE(info->remoteRole == RemoteQsetRole::None);
    REQUIRE(info->address);
}

TEST_CASE("quorum peer state refresh prevents expiry",
          "[overlay][QuorumPeerState]")
{
    auto n1 = nodeID();

    QuorumPeerState state;
    state.reconcile({n1});
    state.recordHandshake(n1, RemoteQsetRole::Direct,
                          PeerBareAddress{"127.0.0.1", 11625}, 10);

    // A refresh (e.g. for a live connection) moves the TTL window forward
    state.refreshLastConnection(n1, 90);
    REQUIRE(state.expireStaleAddresses(100, std::chrono::seconds{50}).empty());
    REQUIRE(state.getInfo(n1)->remoteRole == RemoteQsetRole::Direct);

    // Refreshes never move lastConnection backwards
    state.refreshLastConnection(n1, 5);
    REQUIRE(state.getInfo(n1)->lastConnection == 90);

    // Without further refreshes the entry eventually expires
    auto expired = state.expireStaleAddresses(200, std::chrono::seconds{50});
    REQUIRE(expired.size() == 1);
    REQUIRE(expired.front().first == n1);
}

TEST_CASE("quorum peer state tolerates malformed json",
          "[overlay][QuorumPeerState]")
{
    // Entirely bogus payloads produce an empty state without throwing
    REQUIRE(QuorumPeerState::fromJson("").getInfo().empty());
    REQUIRE(QuorumPeerState::fromJson("garbage").getInfo().empty());
    REQUIRE(QuorumPeerState::fromJson("[1,2,3]").getInfo().empty());
    REQUIRE(QuorumPeerState::fromJson("{\"peers\": 5}").getInfo().empty());
    REQUIRE(QuorumPeerState::fromJson("{\"peers\": {}}").getInfo().empty());

    // Malformed entries are skipped while well-formed ones survive
    auto n1 = nodeID();
    QuorumPeerState state;
    state.reconcile({n1});
    state.recordHandshake(n1, RemoteQsetRole::Direct,
                          PeerBareAddress{"127.0.0.1", 11625}, 123);
    auto json = state.toJson();

    // Splice a corrupt entry into the peers array
    auto insertAt = json.find("[") + 1;
    auto corrupt = std::string("{\"nodeID\":\"notakey\",\"remoteRole\":"
                               "\"bogus\",\"lastConnection\":\"x\"},");
    json.insert(insertAt, corrupt);

    auto restored = QuorumPeerState::fromJson(json);
    REQUIRE(restored.getInfo().size() == 1);
    auto info = restored.getInfo(n1);
    REQUIRE(info);
    REQUIRE(info->remoteRole == RemoteQsetRole::Direct);

    // A valid entry with a hostname-style (unparseable) address keeps the
    // entry but drops the address
    QuorumPeerState state2;
    state2.reconcile({n1});
    state2.recordHandshake(n1, RemoteQsetRole::Direct,
                           PeerBareAddress{"127.0.0.1", 11625}, 123);
    auto json2 = state2.toJson();
    auto pos = json2.find("127.0.0.1:11625");
    REQUIRE(pos != std::string::npos);
    json2.replace(pos, std::string("127.0.0.1:11625").size(),
                  "example.org:123");
    auto restored2 = QuorumPeerState::fromJson(json2);
    REQUIRE(restored2.getInfo(n1));
    REQUIRE(!restored2.getInfo(n1)->address);
}

}
