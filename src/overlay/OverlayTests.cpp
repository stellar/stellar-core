// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "BanManager.h"
#include "crypto/KeyUtils.h"
#include "crypto/SecretKey.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/LoopbackPeer.h"
#include "overlay/OverlayManagerImpl.h"
#include "overlay/PeerRecord.h"
#include "overlay/TCPPeer.h"
#include "simulation/Simulation.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Logging.h"
#include "util/Timer.h"

#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "medida/timer.h"
#include "util/format.h"
#include <numeric>

using namespace stellar;

TEST_CASE("loopback peer hello", "[overlay][connections]")
{
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(0);
    Config const& cfg2 = getTestConfig(1);
    auto app1 = createTestApplication(clock, cfg1);
    auto app2 = createTestApplication(clock, cfg2);

    LoopbackPeerConnection conn(*app1, *app2);
    testutil::crankSome(clock);

    REQUIRE(conn.getInitiator()->isAuthenticated());
    REQUIRE(conn.getAcceptor()->isAuthenticated());
}

TEST_CASE("loopback peer with 0 port", "[overlay][connections]")
{
    VirtualClock clock;
    auto const& cfg1 = getTestConfig(0);
    auto cfg2 = getTestConfig(1);
    cfg2.PEER_PORT = 0;

    auto app1 = createTestApplication(clock, cfg1);
    auto app2 = createTestApplication(clock, cfg2);

    LoopbackPeerConnection conn(*app1, *app2);
    testutil::crankSome(clock);

    REQUIRE(!conn.getInitiator()->isAuthenticated());
    REQUIRE(!conn.getAcceptor()->isAuthenticated());
}

TEST_CASE("loopback peer send auth before hello", "[overlay][connections]")
{
    VirtualClock clock;
    auto const& cfg1 = getTestConfig(0);
    auto const& cfg2 = getTestConfig(1);
    auto app1 = createTestApplication(clock, cfg1);
    auto app2 = createTestApplication(clock, cfg2);

    LoopbackPeerConnection conn(*app1, *app2);
    conn.getInitiator()->sendAuth();
    testutil::crankSome(clock);

    REQUIRE(!conn.getInitiator()->isAuthenticated());
    REQUIRE(!conn.getAcceptor()->isAuthenticated());
}

TEST_CASE("failed auth", "[overlay][connections]")
{
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(0);
    Config const& cfg2 = getTestConfig(1);
    auto app1 = createTestApplication(clock, cfg1);
    auto app2 = createTestApplication(clock, cfg2);

    LoopbackPeerConnection conn(*app1, *app2);
    conn.getInitiator()->setDamageAuth(true);
    testutil::crankSome(clock);

    REQUIRE(!conn.getInitiator()->isConnected());
    REQUIRE(!conn.getAcceptor()->isConnected());
    REQUIRE(conn.getInitiator()->getDropReason() == "unexpected MAC");
}

TEST_CASE("reject non preferred peer", "[overlay][connections]")
{
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(0);
    Config cfg2 = getTestConfig(1);

    cfg2.PREFERRED_PEERS_ONLY = true;

    auto app1 = createTestApplication(clock, cfg1);
    auto app2 = createTestApplication(clock, cfg2);

    LoopbackPeerConnection conn(*app1, *app2);
    testutil::crankSome(clock);

    REQUIRE(!conn.getInitiator()->isConnected());
    REQUIRE(!conn.getAcceptor()->isConnected());
    REQUIRE(conn.getAcceptor()->getDropReason() == "peer rejected");
}

TEST_CASE("accept preferred peer even when strict", "[overlay][connections]")
{
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(0);
    Config cfg2 = getTestConfig(1);

    cfg2.PREFERRED_PEERS_ONLY = true;
    cfg2.PREFERRED_PEER_KEYS.push_back(
        KeyUtils::toStrKey(cfg1.NODE_SEED.getPublicKey()));

    auto app1 = createTestApplication(clock, cfg1);
    auto app2 = createTestApplication(clock, cfg2);

    LoopbackPeerConnection conn(*app1, *app2);
    testutil::crankSome(clock);

    REQUIRE(conn.getInitiator()->isAuthenticated());
    REQUIRE(conn.getAcceptor()->isAuthenticated());
}

TEST_CASE("reject peers beyond max", "[overlay][connections]")
{
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(0);
    Config cfg2 = getTestConfig(1);
    Config const& cfg3 = getTestConfig(2);

    cfg2.MAX_PEER_CONNECTIONS = 1;

    auto app1 = createTestApplication(clock, cfg1);
    auto app2 = createTestApplication(clock, cfg2);
    auto app3 = createTestApplication(clock, cfg3);

    LoopbackPeerConnection conn1(*app1, *app2);
    LoopbackPeerConnection conn2(*app3, *app2);
    testutil::crankSome(clock);

    REQUIRE(conn1.getInitiator()->isConnected());
    REQUIRE(conn1.getAcceptor()->isConnected());
    REQUIRE(!conn2.getInitiator()->isConnected());
    REQUIRE(!conn2.getAcceptor()->isConnected());
    REQUIRE(conn2.getAcceptor()->getDropReason() == "peer rejected");
}

TEST_CASE("allow pending peers beyond max", "[overlay][connections]")
{
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(0);
    Config cfg2 = getTestConfig(1);
    Config const& cfg3 = getTestConfig(2);
    Config const& cfg4 = getTestConfig(3);

    cfg2.MAX_PEER_CONNECTIONS = 1;

    auto app1 = createTestApplication(clock, cfg1);
    auto app2 = createTestApplication(clock, cfg2);
    auto app3 = createTestApplication(clock, cfg3);
    auto app4 = createTestApplication(clock, cfg4);

    LoopbackPeerConnection conn1(*app1, *app2);
    conn1.getInitiator()->setCorked(true);
    LoopbackPeerConnection conn2(*app3, *app2);
    conn2.getInitiator()->setCorked(true);
    LoopbackPeerConnection conn3(*app4, *app2);
    testutil::crankSome(clock);

    REQUIRE(!conn1.getInitiator()->isConnected());
    REQUIRE(!conn1.getAcceptor()->isConnected());
    REQUIRE(!conn2.getInitiator()->isConnected());
    REQUIRE(!conn2.getAcceptor()->isConnected());
    REQUIRE(conn3.getInitiator()->isConnected());
    REQUIRE(conn3.getAcceptor()->isConnected());
    REQUIRE(app2->getMetrics()
                .NewMeter({"overlay", "timeout", "idle"}, "timeout")
                .count() == 2);
}

TEST_CASE("reject pending beyond max", "[overlay][connections]")
{
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(0);
    Config cfg2 = getTestConfig(1);
    Config const& cfg3 = getTestConfig(2);

    cfg2.MAX_PENDING_CONNECTIONS = 1;

    auto app1 = createTestApplication(clock, cfg1);
    auto app2 = createTestApplication(clock, cfg2);
    auto app3 = createTestApplication(clock, cfg3);

    LoopbackPeerConnection conn1(*app1, *app2);
    LoopbackPeerConnection conn2(*app3, *app2);
    testutil::crankSome(clock);

    REQUIRE(conn1.getInitiator()->isConnected());
    REQUIRE(conn1.getAcceptor()->isConnected());
    REQUIRE(!conn2.getInitiator()->isConnected());
    REQUIRE(!conn2.getAcceptor()->isConnected());
    REQUIRE(app2->getMetrics()
                .NewMeter({"overlay", "connection", "reject"}, "connection")
                .count() == 1);
}

TEST_CASE("reject peers with differing network passphrases",
          "[overlay][connections]")
{
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(0);
    Config cfg2 = getTestConfig(1);

    cfg2.NETWORK_PASSPHRASE = "nothing to see here";

    auto app1 = createTestApplication(clock, cfg1);
    auto app2 = createTestApplication(clock, cfg2);

    LoopbackPeerConnection conn(*app1, *app2);
    testutil::crankSome(clock);

    REQUIRE(!conn.getInitiator()->isConnected());
    REQUIRE(!conn.getAcceptor()->isConnected());
}

TEST_CASE("reject peers with invalid cert", "[overlay][connections]")
{
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(0);
    Config cfg2 = getTestConfig(1);

    auto app1 = createTestApplication(clock, cfg1);
    auto app2 = createTestApplication(clock, cfg2);

    LoopbackPeerConnection conn(*app1, *app2);
    conn.getAcceptor()->setDamageCert(true);
    testutil::crankSome(clock);

    REQUIRE(!conn.getInitiator()->isConnected());
    REQUIRE(!conn.getAcceptor()->isConnected());
}

TEST_CASE("reject banned peers", "[overlay][connections]")
{
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(0);
    Config cfg2 = getTestConfig(1);

    auto app1 = createTestApplication(clock, cfg1);
    auto app2 = createTestApplication(clock, cfg2);
    app1->getBanManager().banNode(cfg2.NODE_SEED.getPublicKey());

    LoopbackPeerConnection conn(*app1, *app2);
    testutil::crankSome(clock);

    REQUIRE(!conn.getInitiator()->isConnected());
    REQUIRE(!conn.getAcceptor()->isConnected());
}

TEST_CASE("reject peers with incompatible overlay versions",
          "[overlay][connections]")
{
    Config const& cfg1 = getTestConfig(0);

    auto doVersionCheck = [&](uint32 version) {
        VirtualClock clock;
        Config cfg2 = getTestConfig(1);

        cfg2.OVERLAY_PROTOCOL_MIN_VERSION = version;
        cfg2.OVERLAY_PROTOCOL_VERSION = version;
        auto app1 = createTestApplication(clock, cfg1);
        auto app2 = createTestApplication(clock, cfg2);

        LoopbackPeerConnection conn(*app1, *app2);
        testutil::crankSome(clock);

        REQUIRE(!conn.getInitiator()->isConnected());
        REQUIRE(!conn.getAcceptor()->isConnected());
        REQUIRE(conn.getInitiator()->getDropReason() ==
                "wrong protocol version");
    };
    SECTION("cfg2 above")
    {
        doVersionCheck(cfg1.OVERLAY_PROTOCOL_VERSION + 1);
    }
    SECTION("cfg2 below")
    {
        doVersionCheck(cfg1.OVERLAY_PROTOCOL_MIN_VERSION - 1);
    }
}

TEST_CASE("reject peers who dont handshake quickly", "[overlay][connections]")
{
    auto test = [](unsigned short authenticationTimeout) {
        VirtualClock clock;
        Config cfg1 = getTestConfig(0);
        Config cfg2 = getTestConfig(1);

        cfg1.PEER_AUTHENTICATION_TIMEOUT = authenticationTimeout;
        cfg2.PEER_AUTHENTICATION_TIMEOUT = authenticationTimeout;

        auto app1 = createTestApplication(clock, cfg1);
        auto app2 = createTestApplication(clock, cfg2);
        auto waitTime = std::chrono::seconds(authenticationTimeout + 1);
        auto padTime = std::chrono::seconds(2);

        LoopbackPeerConnection conn(*app1, *app2);
        conn.getInitiator()->setCorked(true);
        auto start = clock.now();
        while (clock.now() < (start + waitTime) &&
               conn.getInitiator()->isConnected() &&
               conn.getAcceptor()->isConnected())
        {
            LOG(INFO) << "clock.now() = "
                      << clock.now().time_since_epoch().count();
            clock.crank(false);
        }
        REQUIRE(clock.now() < (start + waitTime + padTime));
        REQUIRE(!conn.getInitiator()->isConnected());
        REQUIRE(!conn.getAcceptor()->isConnected());
        REQUIRE(app2->getMetrics()
                    .NewMeter({"overlay", "timeout", "idle"}, "timeout")
                    .count() != 0);
    };

    SECTION("2 seconds timeout")
    {
        test(2);
    }

    SECTION("5 seconds timeout")
    {
        test(5);
    }
}

TEST_CASE("reject peers with the same nodeid", "[overlay][connections]")
{
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(0);
    Config const& cfg2 = getTestConfig(1);
    Config cfg3 = getTestConfig(2);

    cfg3.NODE_SEED = cfg1.NODE_SEED;

    auto app1 = createTestApplication(clock, cfg1);
    auto app2 = createTestApplication(clock, cfg2);
    auto app3 = createTestApplication(clock, cfg3);

    LoopbackPeerConnection conn(*app1, *app2);
    LoopbackPeerConnection conn2(*app3, *app2);
    testutil::crankSome(clock);

    REQUIRE(conn.getInitiator()->isAuthenticated());
    REQUIRE(conn.getAcceptor()->isAuthenticated());
    REQUIRE(!conn2.getInitiator()->isConnected());
    REQUIRE(!conn2.getAcceptor()->isConnected());
    REQUIRE(conn2.getAcceptor()->getDropReason() ==
            "connecting already-connected peer");
}

TEST_CASE("connecting to saturated nodes", "[overlay][connections]")
{
    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    auto simulation =
        std::make_shared<Simulation>(Simulation::OVER_TCP, networkID);

    auto getConfiguration = [](int id, unsigned short peerConnections) {
        auto cfg = getTestConfig(id);
        cfg.MAX_PEER_CONNECTIONS = peerConnections;
        cfg.TARGET_PEER_CONNECTIONS = peerConnections;
        cfg.MAX_ADDITIONAL_PEER_CONNECTIONS = 0;
        return cfg;
    };

    auto numberOfAppConnections = [](Application& app) {
        return app.getOverlayManager().getAuthenticatedPeersCount();
    };

    auto numberOfSimulationConnections = [&]() {
        auto nodes = simulation->getNodes();
        return std::accumulate(std::begin(nodes), std::end(nodes), 0,
                               [&](int x, Application::pointer app) {
                                   return x + numberOfAppConnections(*app);
                               });
    };

    auto headCfg = getConfiguration(1, 1);
    auto node1Cfg = getConfiguration(2, 2);
    auto node2Cfg = getConfiguration(3, 2);
    auto node3Cfg = getConfiguration(4, 2);

    SIMULATION_CREATE_NODE(Head);
    SIMULATION_CREATE_NODE(Node1);
    SIMULATION_CREATE_NODE(Node2);
    SIMULATION_CREATE_NODE(Node3);

    SCPQuorumSet qSet;
    qSet.threshold = 2;
    qSet.validators.push_back(vHeadNodeID);
    qSet.validators.push_back(vNode1NodeID);
    qSet.validators.push_back(vNode2NodeID);
    qSet.validators.push_back(vNode3NodeID);

    auto headId = simulation->addNode(vHeadSecretKey, qSet, &headCfg)
                      ->getConfig()
                      .NODE_SEED.getPublicKey();
    simulation->addNode(vNode1SecretKey, qSet, &node1Cfg);
    simulation->addNode(vNode2SecretKey, qSet, &node2Cfg);
    simulation->addNode(vNode3SecretKey, qSet, &node3Cfg);

    simulation->addPendingConnection(vNode1NodeID, vHeadNodeID);
    simulation->addPendingConnection(vNode2NodeID, vHeadNodeID);
    simulation->addPendingConnection(vNode3NodeID, vHeadNodeID);

    simulation->startAllNodes();
    simulation->crankForAtLeast(std::chrono::seconds{15}, false);
    simulation->removeNode(headId);
    simulation->crankForAtLeast(std::chrono::seconds{15}, false);

    // all three (two-way) connections are made
    REQUIRE(numberOfSimulationConnections() == 6);
    simulation->crankForAtLeast(std::chrono::seconds{1}, true);
}

TEST_CASE("preferred peers always connect", "[overlay][connections]")
{
    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    auto simulation =
        std::make_shared<Simulation>(Simulation::OVER_TCP, networkID);

    auto getConfiguration = [](int id, unsigned short targetConnections,
                               unsigned short peerConnections) {
        auto cfg = getTestConfig(id);
        cfg.MAX_PEER_CONNECTIONS = peerConnections;
        cfg.TARGET_PEER_CONNECTIONS = targetConnections;
        cfg.MAX_ADDITIONAL_PEER_CONNECTIONS = 0;
        return cfg;
    };

    auto numberOfAppConnections = [](Application& app) {
        return app.getOverlayManager().getAuthenticatedPeersCount();
    };

    Config configs[3];
    for (int i = 0; i < 3; i++)
    {
        configs[i] = getConfiguration(i + 1, i == 0 ? 1 : 0, 2);
    }

    SIMULATION_CREATE_NODE(Node1);
    SIMULATION_CREATE_NODE(Node2);
    SIMULATION_CREATE_NODE(Node3);

    SCPQuorumSet qSet;
    qSet.threshold = 2;
    qSet.validators.push_back(vNode1NodeID);
    qSet.validators.push_back(vNode2NodeID);
    qSet.validators.push_back(vNode3NodeID);

    // node1 has node2 as preferred peer
    configs[0].PREFERRED_PEERS.emplace_back(
        fmt::format("localhost:{}", configs[1].PEER_PORT));

    simulation->addNode(vNode1SecretKey, qSet, &configs[0]);
    simulation->addNode(vNode2SecretKey, qSet, &configs[1]);
    simulation->addNode(vNode3SecretKey, qSet, &configs[2]);

    simulation->startAllNodes();
    simulation->crankForAtLeast(std::chrono::seconds{3}, false);
    // node1 connected to node2 (counted from both apps)
    REQUIRE(numberOfAppConnections(*simulation->getNode(vNode1NodeID)) == 1);
    REQUIRE(numberOfAppConnections(*simulation->getNode(vNode2NodeID)) == 1);
    REQUIRE(numberOfAppConnections(*simulation->getNode(vNode3NodeID)) == 0);

    // disconnect node1 and node2
    simulation->dropConnection(vNode1NodeID, vNode2NodeID);
    // and connect node 3 to node1 (to take the slot)
    simulation->addConnection(vNode3NodeID, vNode1NodeID);
    simulation->crankForAtLeast(std::chrono::seconds{1}, false);
    REQUIRE(numberOfAppConnections(*simulation->getNode(vNode1NodeID)) == 1);
    REQUIRE(numberOfAppConnections(*simulation->getNode(vNode2NodeID)) == 0);
    REQUIRE(numberOfAppConnections(*simulation->getNode(vNode3NodeID)) == 1);

    // wait a bit more, node1 connects to its preferred peer
    simulation->crankForAtLeast(std::chrono::seconds{3}, true);
    REQUIRE(numberOfAppConnections(*simulation->getNode(vNode1NodeID)) == 2);
    REQUIRE(numberOfAppConnections(*simulation->getNode(vNode2NodeID)) == 1);
    REQUIRE(numberOfAppConnections(*simulation->getNode(vNode3NodeID)) == 1);
}
