// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/OverlayManager.h"
#include "overlay/test/LoopbackPeer.h"
#include "simulation/Simulation.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Timer.h"
#include <lib/catch.hpp>
#include <medida/metrics_registry.h>

using namespace stellar;

TEST_CASE("disconnect peer when overloaded", "[overlay][LoadManager]")
{
    auto const& cfg1 = getTestConfig(1);
    auto cfg2 = getTestConfig(2);
    auto const& cfg3 = getTestConfig(3);

    cfg2.RUN_STANDALONE = false;
    cfg2.MINIMUM_IDLE_PERCENT = 90;
    cfg2.TARGET_PEER_CONNECTIONS = 0;
    cfg2.MAX_ADDITIONAL_PEER_CONNECTIONS = 3;

    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    auto sim =
        std::make_shared<Simulation>(Simulation::OVER_LOOPBACK, networkID);

    SIMULATION_CREATE_NODE(Node1);
    SIMULATION_CREATE_NODE(Node2);
    SIMULATION_CREATE_NODE(Node3);
    sim->addNode(vNode1SecretKey, cfg1.QUORUM_SET, &cfg1);
    sim->addNode(vNode2SecretKey, cfg2.QUORUM_SET, &cfg2);
    sim->addNode(vNode3SecretKey, cfg3.QUORUM_SET, &cfg3);

    auto app1 = sim->getNode(vNode1NodeID);
    auto app2 = sim->getNode(vNode2NodeID);
    auto app3 = sim->getNode(vNode3NodeID);

    sim->addPendingConnection(vNode1NodeID, vNode2NodeID);
    sim->addPendingConnection(vNode3NodeID, vNode2NodeID);

    sim->startAllNodes();

    sim->crankForAtLeast(std::chrono::seconds(10), false);

    auto conn = sim->getLoopbackConnection(vNode1NodeID, vNode2NodeID);
    // now, bork app3
    auto conn2 = sim->getLoopbackConnection(vNode3NodeID, vNode2NodeID);
    conn2->getInitiator()->setCorked(true);

    // app1 and app3 are both connected to app2. app1 will hammer on the
    // connection, app3 will do nothing. app2 should disconnect app1.
    // but app3 should remain connected since the i/o timeout is 30s.

    VirtualTimer timer(app1->getClock());

    auto end = app1->getClock().now() + std::chrono::seconds(12);

    testutil::injectSendPeersAndReschedule(end, app1->getClock(), timer, *conn);

    sim->crankUntil(
        [&]() {
            auto connDisconnected = !conn->getInitiator()->isConnected() &&
                                    !conn->getAcceptor()->isConnected();
            return connDisconnected;
        },
        std::chrono::seconds(15), false);

    REQUIRE(conn2->getInitiator()->isConnected());
    REQUIRE(conn2->getAcceptor()->isConnected());
    REQUIRE(app2->getMetrics()
                .NewMeter({"overlay", "drop", "load-shed"}, "drop")
                .count() != 0);
}
