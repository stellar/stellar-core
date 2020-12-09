// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/KeyUtils.h"
#include "crypto/SecretKey.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/BanManager.h"
#include "overlay/OverlayManagerImpl.h"
#include "overlay/PeerManager.h"
#include "overlay/TCPPeer.h"
#include "overlay/test/LoopbackPeer.h"
#include "simulation/Simulation.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Logging.h"
#include "util/Timer.h"

#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "medida/timer.h"
#include <fmt/format.h>
#include <numeric>

using namespace stellar;

namespace
{
bool
doesNotKnow(Application& knowingApp, Application& knownApp)
{
    return !knowingApp.getOverlayManager()
                .getPeerManager()
                .load(PeerBareAddress{"127.0.0.1",
                                      knownApp.getConfig().PEER_PORT})
                .second;
}

bool
knowsAs(Application& knowingApp, Application& knownApp, PeerType peerType)
{
    auto data = knowingApp.getOverlayManager().getPeerManager().load(
        PeerBareAddress{"127.0.0.1", knownApp.getConfig().PEER_PORT});
    if (!data.second)
    {
        return false;
    }

    return data.first.mType == static_cast<int>(peerType);
}

bool
knowsAsInbound(Application& knowingApp, Application& knownApp)
{
    return knowsAs(knowingApp, knownApp, PeerType::INBOUND);
}

bool
knowsAsOutbound(Application& knowingApp, Application& knownApp)
{
    return knowsAs(knowingApp, knownApp, PeerType::OUTBOUND);
}

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

    REQUIRE(knowsAsOutbound(*app1, *app2));
    REQUIRE(knowsAsInbound(*app2, *app1));

    testutil::shutdownWorkScheduler(*app2);
    testutil::shutdownWorkScheduler(*app1);
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

    testutil::shutdownWorkScheduler(*app2);
    testutil::shutdownWorkScheduler(*app1);
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

    REQUIRE(doesNotKnow(*app1, *app2));
    REQUIRE(doesNotKnow(*app2, *app1));

    testutil::shutdownWorkScheduler(*app2);
    testutil::shutdownWorkScheduler(*app1);
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

    REQUIRE(knowsAsOutbound(*app1, *app2));
    REQUIRE(knowsAsInbound(*app2, *app1));

    testutil::shutdownWorkScheduler(*app2);
    testutil::shutdownWorkScheduler(*app1);
}

TEST_CASE("reject non preferred peer", "[overlay][connections]")
{
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(0);
    Config cfg2 = getTestConfig(1);

    cfg2.PREFERRED_PEERS_ONLY = true;

    auto app1 = createTestApplication(clock, cfg1);
    auto app2 = createTestApplication(clock, cfg2);

    SECTION("inbound")
    {
        LoopbackPeerConnection conn(*app1, *app2);
        testutil::crankSome(clock);

        REQUIRE(!conn.getInitiator()->isConnected());
        REQUIRE(!conn.getAcceptor()->isConnected());
        REQUIRE(conn.getAcceptor()->getDropReason() == "peer rejected");

        REQUIRE(knowsAsOutbound(*app1, *app2));
        REQUIRE(knowsAsInbound(*app2, *app1));

        testutil::shutdownWorkScheduler(*app2);
        testutil::shutdownWorkScheduler(*app1);
    }

    SECTION("outbound")
    {
        LoopbackPeerConnection conn(*app2, *app1);
        testutil::crankSome(clock);

        REQUIRE(!conn.getInitiator()->isConnected());
        REQUIRE(!conn.getAcceptor()->isConnected());
        REQUIRE(conn.getInitiator()->getDropReason() == "peer rejected");

        REQUIRE(knowsAsInbound(*app1, *app2));
        REQUIRE(knowsAsOutbound(*app2, *app1));

        testutil::shutdownWorkScheduler(*app2);
        testutil::shutdownWorkScheduler(*app1);
    }
}

TEST_CASE("accept preferred peer even when strict", "[overlay][connections]")
{
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(0);
    Config cfg2 = getTestConfig(1);

    cfg2.PREFERRED_PEERS_ONLY = true;
    cfg2.PREFERRED_PEER_KEYS.emplace(cfg1.NODE_SEED.getPublicKey());

    auto app1 = createTestApplication(clock, cfg1);
    auto app2 = createTestApplication(clock, cfg2);

    SECTION("inbound")
    {
        LoopbackPeerConnection conn(*app1, *app2);
        testutil::crankSome(clock);

        REQUIRE(conn.getInitiator()->isAuthenticated());
        REQUIRE(conn.getAcceptor()->isAuthenticated());

        REQUIRE(knowsAsOutbound(*app1, *app2));
        REQUIRE(knowsAsInbound(*app2, *app1));

        testutil::shutdownWorkScheduler(*app2);
        testutil::shutdownWorkScheduler(*app1);
    }

    SECTION("outbound")
    {
        LoopbackPeerConnection conn(*app2, *app1);
        testutil::crankSome(clock);

        REQUIRE(conn.getInitiator()->isAuthenticated());
        REQUIRE(conn.getAcceptor()->isAuthenticated());

        REQUIRE(knowsAsInbound(*app1, *app2));
        REQUIRE(knowsAsOutbound(*app2, *app1));

        testutil::shutdownWorkScheduler(*app2);
        testutil::shutdownWorkScheduler(*app1);
    }
}

TEST_CASE("reject peers beyond max", "[overlay][connections]")
{
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(0);
    Config cfg2 = getTestConfig(1);
    Config const& cfg3 = getTestConfig(2);

    SECTION("inbound")
    {
        cfg2.MAX_ADDITIONAL_PEER_CONNECTIONS = 1;
        cfg2.TARGET_PEER_CONNECTIONS = 0;

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

        REQUIRE(knowsAsOutbound(*app1, *app2));
        REQUIRE(knowsAsInbound(*app2, *app1));
        REQUIRE(knowsAsOutbound(*app3, *app2));
        REQUIRE(knowsAsInbound(*app2, *app3));

        testutil::shutdownWorkScheduler(*app3);
        testutil::shutdownWorkScheduler(*app2);
        testutil::shutdownWorkScheduler(*app1);
    }

    SECTION("outbound")
    {
        cfg2.MAX_ADDITIONAL_PEER_CONNECTIONS = 0;
        cfg2.TARGET_PEER_CONNECTIONS = 1;

        auto app1 = createTestApplication(clock, cfg1);
        auto app2 = createTestApplication(clock, cfg2);
        auto app3 = createTestApplication(clock, cfg3);

        LoopbackPeerConnection conn1(*app2, *app1);
        LoopbackPeerConnection conn2(*app2, *app3);
        testutil::crankSome(clock);

        REQUIRE(conn1.getInitiator()->isConnected());
        REQUIRE(conn1.getAcceptor()->isConnected());
        REQUIRE(!conn2.getInitiator()->isConnected());
        REQUIRE(!conn2.getAcceptor()->isConnected());
        REQUIRE(conn2.getInitiator()->getDropReason() == "peer rejected");

        REQUIRE(knowsAsInbound(*app1, *app2));
        REQUIRE(knowsAsOutbound(*app2, *app1));
        REQUIRE(knowsAsInbound(*app3, *app2));
        REQUIRE(knowsAsOutbound(*app2, *app3));

        testutil::shutdownWorkScheduler(*app3);
        testutil::shutdownWorkScheduler(*app2);
        testutil::shutdownWorkScheduler(*app1);
    }
}

TEST_CASE("reject peers beyond max - preferred peer wins",
          "[overlay][connections]")
{
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(0);
    Config cfg2 = getTestConfig(1);
    Config const& cfg3 = getTestConfig(2);

    SECTION("preferred connects first")
    {
        SECTION("inbound")
        {
            cfg2.MAX_ADDITIONAL_PEER_CONNECTIONS = 1;
            cfg2.TARGET_PEER_CONNECTIONS = 0;
            cfg2.PREFERRED_PEER_KEYS.emplace(cfg3.NODE_SEED.getPublicKey());

            auto app1 = createTestApplication(clock, cfg1);
            auto app2 = createTestApplication(clock, cfg2);
            auto app3 = createTestApplication(clock, cfg3);

            LoopbackPeerConnection conn2(*app3, *app2);
            LoopbackPeerConnection conn1(*app1, *app2);
            testutil::crankSome(clock);

            REQUIRE(!conn1.getInitiator()->isConnected());
            REQUIRE(!conn1.getAcceptor()->isConnected());
            REQUIRE(conn2.getInitiator()->isConnected());
            REQUIRE(conn2.getAcceptor()->isConnected());
            REQUIRE(conn1.getAcceptor()->getDropReason() == "peer rejected");

            REQUIRE(knowsAsOutbound(*app1, *app2));
            REQUIRE(knowsAsInbound(*app2, *app1));
            REQUIRE(knowsAsOutbound(*app3, *app2));
            REQUIRE(knowsAsInbound(*app2, *app3));

            testutil::shutdownWorkScheduler(*app3);
            testutil::shutdownWorkScheduler(*app2);
            testutil::shutdownWorkScheduler(*app1);
        }

        SECTION("outbound")
        {
            cfg2.MAX_ADDITIONAL_PEER_CONNECTIONS = 0;
            cfg2.TARGET_PEER_CONNECTIONS = 1;
            cfg2.PREFERRED_PEER_KEYS.emplace(cfg3.NODE_SEED.getPublicKey());

            auto app1 = createTestApplication(clock, cfg1);
            auto app2 = createTestApplication(clock, cfg2);
            auto app3 = createTestApplication(clock, cfg3);

            LoopbackPeerConnection conn2(*app2, *app3);
            LoopbackPeerConnection conn1(*app2, *app1);
            testutil::crankSome(clock);

            REQUIRE(!conn1.getInitiator()->isConnected());
            REQUIRE(!conn1.getAcceptor()->isConnected());
            REQUIRE(conn2.getInitiator()->isConnected());
            REQUIRE(conn2.getAcceptor()->isConnected());
            REQUIRE(conn1.getInitiator()->getDropReason() == "peer rejected");

            REQUIRE(knowsAsInbound(*app1, *app2));
            REQUIRE(knowsAsOutbound(*app2, *app1));
            REQUIRE(knowsAsInbound(*app3, *app2));
            REQUIRE(knowsAsOutbound(*app2, *app3));

            testutil::shutdownWorkScheduler(*app3);
            testutil::shutdownWorkScheduler(*app2);
            testutil::shutdownWorkScheduler(*app1);
        }
    }

    SECTION("preferred connects second")
    {
        SECTION("inbound")
        {
            cfg2.MAX_ADDITIONAL_PEER_CONNECTIONS = 1;
            cfg2.TARGET_PEER_CONNECTIONS = 0;
            cfg2.PREFERRED_PEER_KEYS.emplace(cfg3.NODE_SEED.getPublicKey());

            auto app1 = createTestApplication(clock, cfg1);
            auto app2 = createTestApplication(clock, cfg2);
            auto app3 = createTestApplication(clock, cfg3);

            LoopbackPeerConnection conn1(*app1, *app2);
            LoopbackPeerConnection conn2(*app3, *app2);
            testutil::crankSome(clock);

            REQUIRE(!conn1.getInitiator()->isConnected());
            REQUIRE(!conn1.getAcceptor()->isConnected());
            REQUIRE(conn2.getInitiator()->isConnected());
            REQUIRE(conn2.getAcceptor()->isConnected());
            REQUIRE(conn1.getAcceptor()->getDropReason() ==
                    "preferred peer selected instead");

            REQUIRE(knowsAsOutbound(*app1, *app2));
            REQUIRE(knowsAsInbound(*app2, *app1));
            REQUIRE(knowsAsOutbound(*app3, *app2));
            REQUIRE(knowsAsInbound(*app2, *app3));

            testutil::shutdownWorkScheduler(*app3);
            testutil::shutdownWorkScheduler(*app2);
            testutil::shutdownWorkScheduler(*app1);
        }

        SECTION("outbound")
        {
            cfg2.MAX_ADDITIONAL_PEER_CONNECTIONS = 0;
            cfg2.TARGET_PEER_CONNECTIONS = 1;
            cfg2.PREFERRED_PEER_KEYS.emplace(cfg3.NODE_SEED.getPublicKey());

            auto app1 = createTestApplication(clock, cfg1);
            auto app2 = createTestApplication(clock, cfg2);
            auto app3 = createTestApplication(clock, cfg3);

            LoopbackPeerConnection conn1(*app2, *app1);
            LoopbackPeerConnection conn2(*app2, *app3);
            testutil::crankSome(clock);

            REQUIRE(!conn1.getInitiator()->isConnected());
            REQUIRE(!conn1.getAcceptor()->isConnected());
            REQUIRE(conn2.getInitiator()->isConnected());
            REQUIRE(conn2.getAcceptor()->isConnected());
            REQUIRE(conn1.getInitiator()->getDropReason() ==
                    "preferred peer selected instead");

            REQUIRE(knowsAsInbound(*app1, *app2));
            REQUIRE(knowsAsOutbound(*app2, *app1));
            REQUIRE(knowsAsInbound(*app3, *app2));
            REQUIRE(knowsAsOutbound(*app2, *app3));

            testutil::shutdownWorkScheduler(*app3);
            testutil::shutdownWorkScheduler(*app2);
            testutil::shutdownWorkScheduler(*app1);
        }
    }
}

TEST_CASE("allow inbound pending peers up to max", "[overlay][connections]")
{
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(0);
    Config cfg2 = getTestConfig(1);
    Config const& cfg3 = getTestConfig(2);
    Config const& cfg4 = getTestConfig(3);
    Config const& cfg5 = getTestConfig(4);

    cfg2.MAX_INBOUND_PENDING_CONNECTIONS = 3;
    cfg2.MAX_OUTBOUND_PENDING_CONNECTIONS = 3;

    auto app1 = createTestApplication(clock, cfg1);
    auto app2 = createTestApplication(clock, cfg2);
    auto app3 = createTestApplication(clock, cfg3);
    auto app4 = createTestApplication(clock, cfg4);
    auto app5 = createTestApplication(clock, cfg5);

    LoopbackPeerConnection conn1(*app1, *app2);
    REQUIRE(conn1.getInitiator()->getState() == Peer::CONNECTED);
    REQUIRE(conn1.getAcceptor()->getState() == Peer::CONNECTED);
    conn1.getInitiator()->setCorked(true);

    LoopbackPeerConnection conn2(*app3, *app2);
    REQUIRE(conn2.getInitiator()->getState() == Peer::CONNECTED);
    REQUIRE(conn2.getAcceptor()->getState() == Peer::CONNECTED);
    conn2.getInitiator()->setCorked(true);

    LoopbackPeerConnection conn3(*app4, *app2);
    REQUIRE(conn3.getInitiator()->getState() == Peer::CONNECTED);
    REQUIRE(conn3.getAcceptor()->getState() == Peer::CONNECTED);

    LoopbackPeerConnection conn4(*app5, *app2);
    REQUIRE(conn4.getInitiator()->getState() == Peer::CONNECTED);
    REQUIRE(conn4.getAcceptor()->getState() == Peer::CLOSING);

    testutil::crankSome(clock);

    REQUIRE(conn1.getInitiator()->getState() == Peer::CLOSING);
    REQUIRE(conn1.getAcceptor()->getState() == Peer::CLOSING);
    REQUIRE(conn2.getInitiator()->getState() == Peer::CLOSING);
    REQUIRE(conn2.getAcceptor()->getState() == Peer::CLOSING);
    REQUIRE(conn3.getInitiator()->isConnected());
    REQUIRE(conn3.getAcceptor()->isConnected());
    REQUIRE(conn4.getInitiator()->getState() == Peer::CLOSING);
    REQUIRE(conn4.getAcceptor()->getState() == Peer::CLOSING);
    REQUIRE(app2->getMetrics()
                .NewMeter({"overlay", "timeout", "idle"}, "timeout")
                .count() == 2);

    REQUIRE(doesNotKnow(*app1, *app2)); // corked
    REQUIRE(doesNotKnow(*app2, *app1)); // corked
    REQUIRE(doesNotKnow(*app3, *app2)); // corked
    REQUIRE(doesNotKnow(*app2, *app3)); // corked
    REQUIRE(knowsAsOutbound(*app4, *app2));
    REQUIRE(knowsAsInbound(*app2, *app4));
    REQUIRE(doesNotKnow(*app5, *app2)); // didn't get to hello phase
    REQUIRE(doesNotKnow(*app2, *app5)); // didn't get to hello phase

    testutil::shutdownWorkScheduler(*app5);
    testutil::shutdownWorkScheduler(*app4);
    testutil::shutdownWorkScheduler(*app3);
    testutil::shutdownWorkScheduler(*app2);
    testutil::shutdownWorkScheduler(*app1);
}

TEST_CASE("allow inbound pending peers over max if possibly preferred",
          "[overlay][connections]")
{
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(0);
    Config cfg2 = getTestConfig(1);
    Config const& cfg3 = getTestConfig(2);
    Config const& cfg4 = getTestConfig(3);
    Config const& cfg5 = getTestConfig(4);

    cfg2.MAX_INBOUND_PENDING_CONNECTIONS = 3;
    cfg2.MAX_OUTBOUND_PENDING_CONNECTIONS = 3;
    cfg2.PREFERRED_PEERS.emplace_back("127.0.0.1:17");

    auto app1 = createTestApplication(clock, cfg1);
    auto app2 = createTestApplication(clock, cfg2);
    auto app3 = createTestApplication(clock, cfg3);
    auto app4 = createTestApplication(clock, cfg4);
    auto app5 = createTestApplication(clock, cfg5);

    (static_cast<OverlayManagerImpl&>(app2->getOverlayManager()))
        .storeConfigPeers();

    LoopbackPeerConnection conn1(*app1, *app2);
    REQUIRE(conn1.getInitiator()->getState() == Peer::CONNECTED);
    REQUIRE(conn1.getAcceptor()->getState() == Peer::CONNECTED);
    conn1.getInitiator()->setCorked(true);

    LoopbackPeerConnection conn2(*app3, *app2);
    REQUIRE(conn2.getInitiator()->getState() == Peer::CONNECTED);
    REQUIRE(conn2.getAcceptor()->getState() == Peer::CONNECTED);
    conn2.getInitiator()->setCorked(true);

    LoopbackPeerConnection conn3(*app4, *app2);
    REQUIRE(conn3.getInitiator()->getState() == Peer::CONNECTED);
    REQUIRE(conn3.getAcceptor()->getState() == Peer::CONNECTED);

    LoopbackPeerConnection conn4(*app5, *app2);
    REQUIRE(conn4.getInitiator()->getState() == Peer::CONNECTED);
    REQUIRE(conn4.getAcceptor()->getState() == Peer::CONNECTED);

    testutil::crankSome(clock);

    REQUIRE(conn1.getInitiator()->getState() == Peer::CLOSING);
    REQUIRE(conn1.getAcceptor()->getState() == Peer::CLOSING);
    REQUIRE(conn2.getInitiator()->getState() == Peer::CLOSING);
    REQUIRE(conn2.getAcceptor()->getState() == Peer::CLOSING);
    REQUIRE(conn3.getInitiator()->isConnected());
    REQUIRE(conn3.getAcceptor()->isConnected());
    REQUIRE(conn4.getInitiator()->isConnected());
    REQUIRE(conn4.getAcceptor()->isConnected());
    REQUIRE(app2->getMetrics()
                .NewMeter({"overlay", "timeout", "idle"}, "timeout")
                .count() == 2);
    REQUIRE(app2->getMetrics()
                .NewMeter({"overlay", "connection", "reject"}, "connection")
                .count() == 0);

    REQUIRE(doesNotKnow(*app1, *app2)); // corked
    REQUIRE(doesNotKnow(*app2, *app1)); // corked
    REQUIRE(doesNotKnow(*app3, *app2)); // corked
    REQUIRE(doesNotKnow(*app2, *app3)); // corked
    REQUIRE(knowsAsOutbound(*app4, *app2));
    REQUIRE(knowsAsInbound(*app2, *app4));
    REQUIRE(knowsAsOutbound(*app5, *app2));
    REQUIRE(knowsAsInbound(*app2, *app5));

    testutil::shutdownWorkScheduler(*app5);
    testutil::shutdownWorkScheduler(*app4);
    testutil::shutdownWorkScheduler(*app3);
    testutil::shutdownWorkScheduler(*app2);
    testutil::shutdownWorkScheduler(*app1);
}

TEST_CASE("allow outbound pending peers up to max", "[overlay][connections]")
{
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(0);
    Config cfg2 = getTestConfig(1);
    Config const& cfg3 = getTestConfig(2);
    Config const& cfg4 = getTestConfig(3);
    Config const& cfg5 = getTestConfig(4);

    cfg2.MAX_INBOUND_PENDING_CONNECTIONS = 3;
    cfg2.MAX_OUTBOUND_PENDING_CONNECTIONS = 3;

    auto app1 = createTestApplication(clock, cfg1);
    auto app2 = createTestApplication(clock, cfg2);
    auto app3 = createTestApplication(clock, cfg3);
    auto app4 = createTestApplication(clock, cfg4);
    auto app5 = createTestApplication(clock, cfg5);

    LoopbackPeerConnection conn1(*app2, *app1);
    REQUIRE(conn1.getInitiator()->getState() == Peer::CONNECTED);
    REQUIRE(conn1.getAcceptor()->getState() == Peer::CONNECTED);
    conn1.getInitiator()->setCorked(true);

    LoopbackPeerConnection conn2(*app2, *app3);
    REQUIRE(conn2.getInitiator()->getState() == Peer::CONNECTED);
    REQUIRE(conn2.getAcceptor()->getState() == Peer::CONNECTED);
    conn2.getInitiator()->setCorked(true);

    LoopbackPeerConnection conn3(*app2, *app4);
    REQUIRE(conn3.getInitiator()->getState() == Peer::CONNECTED);
    REQUIRE(conn3.getAcceptor()->getState() == Peer::CONNECTED);

    LoopbackPeerConnection conn4(*app2, *app5);
    REQUIRE(conn4.getInitiator()->getState() == Peer::CLOSING);
    REQUIRE(conn4.getAcceptor()->getState() == Peer::CONNECTED);
    conn2.getInitiator()->setCorked(true);

    testutil::crankSome(clock);

    REQUIRE(conn1.getInitiator()->getState() == Peer::CLOSING);
    REQUIRE(conn1.getAcceptor()->getState() == Peer::CLOSING);
    REQUIRE(conn2.getInitiator()->getState() == Peer::CLOSING);
    REQUIRE(conn2.getAcceptor()->getState() == Peer::CLOSING);
    REQUIRE(conn3.getInitiator()->isConnected());
    REQUIRE(conn3.getAcceptor()->isConnected());
    REQUIRE(conn4.getInitiator()->getState() == Peer::CLOSING);
    REQUIRE(conn4.getAcceptor()->getState() == Peer::CLOSING);
    REQUIRE(app2->getMetrics()
                .NewMeter({"overlay", "timeout", "idle"}, "timeout")
                .count() == 2);

    REQUIRE(doesNotKnow(*app1, *app2)); // corked
    REQUIRE(doesNotKnow(*app2, *app1)); // corked
    REQUIRE(doesNotKnow(*app3, *app2)); // corked
    REQUIRE(doesNotKnow(*app2, *app3)); // corked
    REQUIRE(knowsAsInbound(*app4, *app2));
    REQUIRE(knowsAsOutbound(*app2, *app4));
    REQUIRE(doesNotKnow(*app5, *app2)); // corked
    REQUIRE(doesNotKnow(*app2, *app5)); // corked

    testutil::shutdownWorkScheduler(*app5);
    testutil::shutdownWorkScheduler(*app4);
    testutil::shutdownWorkScheduler(*app3);
    testutil::shutdownWorkScheduler(*app2);
    testutil::shutdownWorkScheduler(*app1);
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

    REQUIRE(doesNotKnow(*app1, *app2));
    REQUIRE(doesNotKnow(*app2, *app1));

    testutil::shutdownWorkScheduler(*app2);
    testutil::shutdownWorkScheduler(*app1);
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

    REQUIRE(doesNotKnow(*app1, *app2));
    REQUIRE(knowsAsInbound(*app2, *app1));

    testutil::shutdownWorkScheduler(*app2);
    testutil::shutdownWorkScheduler(*app1);
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

    REQUIRE(doesNotKnow(*app1, *app2));
    REQUIRE(knowsAsInbound(*app2, *app1));

    testutil::shutdownWorkScheduler(*app2);
    testutil::shutdownWorkScheduler(*app1);
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

        REQUIRE(doesNotKnow(*app1, *app2));
        REQUIRE(doesNotKnow(*app2, *app1));

        testutil::shutdownWorkScheduler(*app2);
        testutil::shutdownWorkScheduler(*app1);
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
        Config cfg1 = getTestConfig(1);
        Config cfg2 = getTestConfig(2);

        cfg1.PEER_AUTHENTICATION_TIMEOUT = authenticationTimeout;
        cfg2.PEER_AUTHENTICATION_TIMEOUT = authenticationTimeout;

        auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
        auto sim =
            std::make_shared<Simulation>(Simulation::OVER_LOOPBACK, networkID);

        SIMULATION_CREATE_NODE(Node1);
        SIMULATION_CREATE_NODE(Node2);
        sim->addNode(vNode1SecretKey, cfg1.QUORUM_SET, &cfg1);
        sim->addNode(vNode2SecretKey, cfg2.QUORUM_SET, &cfg2);
        auto waitTime = std::chrono::seconds(authenticationTimeout + 1);
        auto padTime = std::chrono::seconds(2);

        sim->addPendingConnection(vNode1NodeID, vNode2NodeID);

        sim->startAllNodes();

        auto conn = sim->getLoopbackConnection(vNode1NodeID, vNode2NodeID);

        conn->getInitiator()->setCorked(true);

        sim->crankForAtLeast(waitTime + padTime, false);

        sim->crankUntil(
            [&]() {
                return !(conn->getInitiator()->isConnected() ||
                         conn->getAcceptor()->isConnected());
            },
            padTime, true);

        auto app1 = sim->getNode(vNode1NodeID);
        auto app2 = sim->getNode(vNode2NodeID);

        auto idle1 = app1->getMetrics()
                         .NewMeter({"overlay", "timeout", "idle"}, "timeout")
                         .count();
        auto idle2 = app2->getMetrics()
                         .NewMeter({"overlay", "timeout", "idle"}, "timeout")
                         .count();

        REQUIRE((idle1 != 0 || idle2 != 0));

        REQUIRE(doesNotKnow(*app1, *app2));
        REQUIRE(doesNotKnow(*app2, *app1));
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

TEST_CASE("drop peers who straggle", "[overlay][connections][straggler]")
{
    auto test = [](unsigned short stragglerTimeout) {
        VirtualClock clock;
        Config cfg1 = getTestConfig(0);
        Config cfg2 = getTestConfig(1);

        // Straggler detection piggy-backs on the idle timer so we drive
        // the test from idle-timer-firing granularity.
        assert(cfg1.PEER_TIMEOUT == cfg2.PEER_TIMEOUT);
        assert(stragglerTimeout >= cfg1.PEER_TIMEOUT * 2);

        // Initiator (cfg1) will straggle, and acceptor (cfg2) will notice and
        // disconnect.
        cfg2.PEER_STRAGGLER_TIMEOUT = stragglerTimeout;

        auto app1 = createTestApplication(clock, cfg1);
        auto app2 = createTestApplication(clock, cfg2);
        auto waitTime = std::chrono::seconds(stragglerTimeout * 3);
        auto padTime = std::chrono::seconds(5);

        LoopbackPeerConnection conn(*app1, *app2);
        auto start = clock.now();

        testutil::crankSome(clock);
        REQUIRE(conn.getInitiator()->isAuthenticated());
        REQUIRE(conn.getAcceptor()->isAuthenticated());

        conn.getInitiator()->setStraggling(true);
        auto straggler = conn.getInitiator();
        VirtualTimer sendTimer(*app1);

        while (clock.now() < (start + waitTime) &&
               (conn.getInitiator()->isConnected() ||
                conn.getAcceptor()->isConnected()))
        {
            // Straggler keeps asking for peers once per second -- this is
            // easy traffic to fake-generate -- but not accepting response
            // messages in a timely fashion.
            sendTimer.expires_from_now(std::chrono::seconds(1));
            sendTimer.async_wait([straggler](asio::error_code const& error) {
                if (!error)
                {
                    straggler->sendGetPeers();
                }
            });
            clock.crank(false);
        }
        LOG_INFO(DEFAULT_LOG, "loop complete, clock.now() = {}",
                 clock.now().time_since_epoch().count());
        REQUIRE(clock.now() < (start + waitTime + padTime));
        REQUIRE(!conn.getInitiator()->isConnected());
        REQUIRE(!conn.getAcceptor()->isConnected());
        REQUIRE(app1->getMetrics()
                    .NewMeter({"overlay", "timeout", "idle"}, "timeout")
                    .count() == 0);
        REQUIRE(app2->getMetrics()
                    .NewMeter({"overlay", "timeout", "idle"}, "timeout")
                    .count() == 0);
        REQUIRE(app2->getMetrics()
                    .NewMeter({"overlay", "timeout", "straggler"}, "timeout")
                    .count() != 0);

        testutil::shutdownWorkScheduler(*app2);
        testutil::shutdownWorkScheduler(*app1);
    };

    SECTION("60 seconds straggle timeout")
    {
        test(60);
    }

    SECTION("120 seconds straggle timeout")
    {
        test(120);
    }

    SECTION("150 seconds straggle timeout")
    {
        test(150);
    }
}

TEST_CASE("reject peers with the same nodeid", "[overlay][connections]")
{
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(1);
    Config cfg2 = getTestConfig(2);

    cfg2.NODE_SEED = cfg1.NODE_SEED;

    auto app1 = createTestApplication(clock, cfg1);
    auto app2 = createTestApplication(clock, cfg2);

    SECTION("inbound")
    {
        LoopbackPeerConnection conn(*app1, *app2);
        testutil::crankSome(clock);

        REQUIRE(conn.getInitiator()->getDropReason() == "connecting to self");
    }

    SECTION("outbound")
    {
        LoopbackPeerConnection conn(*app2, *app1);
        testutil::crankSome(clock);

        REQUIRE(conn.getInitiator()->getDropReason() == "connecting to self");
    }

    testutil::shutdownWorkScheduler(*app2);
    testutil::shutdownWorkScheduler(*app1);
}

TEST_CASE("connecting to saturated nodes", "[overlay][connections][acceptance]")
{
    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    auto simulation =
        std::make_shared<Simulation>(Simulation::OVER_TCP, networkID);

    auto getConfiguration = [](int id, unsigned short targetOutboundConnections,
                               unsigned short maxInboundConnections) {
        auto cfg = getTestConfig(id);
        cfg.TARGET_PEER_CONNECTIONS = targetOutboundConnections;
        cfg.MAX_ADDITIONAL_PEER_CONNECTIONS = maxInboundConnections;
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

    auto headCfg = getConfiguration(1, 0, 1);
    auto node1Cfg = getConfiguration(2, 1, 1);
    auto node2Cfg = getConfiguration(3, 1, 1);
    auto node3Cfg = getConfiguration(4, 1, 1);

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

    // large timeout here as nodes may have a few bad attempts
    // (crossed connections) and we rely on jittered backoffs
    // to mitigate this

    simulation->addPendingConnection(vNode1NodeID, vHeadNodeID);
    simulation->startAllNodes();
    UNSCOPED_INFO("1 connects to h");
    simulation->crankUntil(
        [&]() { return numberOfSimulationConnections() == 2; },
        std::chrono::seconds{3}, false);

    simulation->addNode(vNode2SecretKey, qSet, &node2Cfg);
    simulation->addPendingConnection(vNode2NodeID, vHeadNodeID);
    simulation->startAllNodes();
    UNSCOPED_INFO("2 connects to 1");
    simulation->crankUntil(
        [&]() { return numberOfSimulationConnections() == 4; },
        std::chrono::seconds{20}, false);

    simulation->addNode(vNode3SecretKey, qSet, &node3Cfg);
    simulation->addPendingConnection(vNode3NodeID, vHeadNodeID);
    simulation->startAllNodes();
    UNSCOPED_INFO("3 connects to 2");
    simulation->crankUntil(
        [&]() { return numberOfSimulationConnections() == 6; },
        std::chrono::seconds{30}, false);

    simulation->removeNode(headId);
    UNSCOPED_INFO("wait for node to be disconnected");
    simulation->crankForAtLeast(std::chrono::seconds{2}, false);
    UNSCOPED_INFO("wait for 1 to connect to 3");
    simulation->crankUntil(
        [&]() { return numberOfSimulationConnections() == 6; },
        std::chrono::seconds{30}, true);
}

TEST_CASE("inbounds nodes can be promoted to ouboundvalid",
          "[overlay][acceptance]")
{
    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    auto simulation =
        std::make_shared<Simulation>(Simulation::OVER_TCP, networkID);

    SIMULATION_CREATE_NODE(Node1);
    SIMULATION_CREATE_NODE(Node2);
    SIMULATION_CREATE_NODE(Node3);

    SCPQuorumSet qSet;
    qSet.threshold = 1;
    qSet.validators.push_back(vNode1NodeID);

    auto nodes = std::vector<Application::pointer>{};
    auto configs = std::vector<Config>{};
    auto addresses = std::vector<PeerBareAddress>{};
    for (auto i = 0; i < 3; i++)
    {
        configs.push_back(getTestConfig(i + 1));
        addresses.emplace_back("127.0.0.1", configs[i].PEER_PORT);
    }

    configs[0].KNOWN_PEERS.emplace_back(
        fmt::format("127.0.0.1:{}", configs[1].PEER_PORT));
    configs[2].KNOWN_PEERS.emplace_back(
        fmt::format("127.0.0.1:{}", configs[0].PEER_PORT));

    nodes.push_back(simulation->addNode(vNode1SecretKey, qSet, &configs[0]));
    nodes.push_back(simulation->addNode(vNode2SecretKey, qSet, &configs[1]));
    nodes.push_back(simulation->addNode(vNode3SecretKey, qSet, &configs[2]));

    enum class TestPeerType
    {
        ANY,
        KNOWN,
        OUTBOUND
    };

    auto getTestPeerType = [&](int i, int j) {
        auto& node = nodes[i];
        auto peer =
            node->getOverlayManager().getPeerManager().load(addresses[j]);
        if (!peer.second)
        {
            return TestPeerType::ANY;
        }

        return peer.first.mType == static_cast<int>(PeerType::INBOUND)
                   ? TestPeerType::KNOWN
                   : TestPeerType::OUTBOUND;
    };

    using ExpectedResultType = std::vector<std::vector<TestPeerType>>;
    auto peerTypesMatch = [&](ExpectedResultType expected) {
        for (auto i = 0; i < expected.size(); i++)
        {
            for (auto j = 0; j < expected[i].size(); j++)
            {
                if (expected[i][j] > getTestPeerType(i, j))
                {
                    return false;
                }
            }
        }
        return true;
    };

    simulation->startAllNodes();

    // at first, nodes only know about KNOWN_PEERS
    simulation->crankUntil(
        [&] {
            return peerTypesMatch(
                {{TestPeerType::ANY, TestPeerType::KNOWN, TestPeerType::ANY},
                 {TestPeerType::ANY, TestPeerType::ANY, TestPeerType::ANY},
                 {TestPeerType::KNOWN, TestPeerType::ANY, TestPeerType::ANY}});
        },
        std::chrono::seconds(2), false);

    // then, after connection, some are made OUTBOUND
    simulation->crankUntil(
        [&] {
            return peerTypesMatch(
                {{TestPeerType::ANY, TestPeerType::OUTBOUND,
                  TestPeerType::KNOWN},
                 {TestPeerType::KNOWN, TestPeerType::ANY, TestPeerType::ANY},
                 {TestPeerType::OUTBOUND, TestPeerType::ANY,
                  TestPeerType::ANY}});
        },
        std::chrono::seconds(10), false);

    // then, after promotion, more are made OUTBOUND
    simulation->crankUntil(
        [&] {
            return peerTypesMatch(
                {{TestPeerType::ANY, TestPeerType::OUTBOUND,
                  TestPeerType::OUTBOUND},
                 {TestPeerType::OUTBOUND, TestPeerType::ANY, TestPeerType::ANY},
                 {TestPeerType::OUTBOUND, TestPeerType::ANY,
                  TestPeerType::ANY}});
        },
        std::chrono::seconds(30), false);

    // and when all connections are made, all nodes know about each other
    simulation->crankUntil(
        [&] {
            return peerTypesMatch(
                {{TestPeerType::ANY, TestPeerType::OUTBOUND,
                  TestPeerType::OUTBOUND},
                 {TestPeerType::OUTBOUND, TestPeerType::ANY,
                  TestPeerType::OUTBOUND},
                 {TestPeerType::OUTBOUND, TestPeerType::OUTBOUND,
                  TestPeerType::ANY}});
        },
        std::chrono::seconds(30), false);

    simulation->crankForAtLeast(std::chrono::seconds{3}, true);
}

PeerBareAddress
localhost(unsigned short port)
{
    return PeerBareAddress{"127.0.0.1", port};
}

TEST_CASE("database is purged at overlay start", "[overlay]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.RUN_STANDALONE = false;
    auto app = createTestApplication(clock, cfg);
    auto& om = app->getOverlayManager();
    auto& peerManager = om.getPeerManager();
    auto record = [](int numFailures) {
        return PeerRecord{{}, numFailures, static_cast<int>(PeerType::INBOUND)};
    };

    peerManager.store(localhost(1), record(118), false);
    peerManager.store(localhost(2), record(119), false);
    peerManager.store(localhost(3), record(120), false);
    peerManager.store(localhost(4), record(121), false);
    peerManager.store(localhost(5), record(122), false);

    om.start();

    testutil::crankSome(clock);

    REQUIRE(peerManager.load(localhost(1)).second);
    REQUIRE(peerManager.load(localhost(2)).second);
    REQUIRE(!peerManager.load(localhost(3)).second);
    REQUIRE(!peerManager.load(localhost(4)).second);
    REQUIRE(!peerManager.load(localhost(5)).second);
}

TEST_CASE("peer numfailures resets after good connection",
          "[overlay][acceptance]")
{
    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    auto simulation =
        std::make_shared<Simulation>(Simulation::OVER_TCP, networkID);
    auto record = [](int numFailures) {
        return PeerRecord{{}, numFailures, static_cast<int>(PeerType::INBOUND)};
    };

    SIMULATION_CREATE_NODE(Node1);
    SIMULATION_CREATE_NODE(Node2);

    SCPQuorumSet qSet;
    qSet.threshold = 1;
    qSet.validators.push_back(vNode1NodeID);

    Config const& cfg1 = getTestConfig(1);
    Config const& cfg2 = getTestConfig(2);

    auto app1 = simulation->addNode(vNode1SecretKey, qSet, &cfg1);
    auto app2 = simulation->addNode(vNode2SecretKey, qSet, &cfg2);

    simulation->startAllNodes();

    auto& om = app1->getOverlayManager();
    auto& peerManager = om.getPeerManager();
    peerManager.store(localhost(cfg2.PEER_PORT), record(119), false);
    REQUIRE(peerManager.load(localhost(cfg2.PEER_PORT)).second);

    simulation->crankForAtLeast(std::chrono::seconds{4}, true);

    auto r = peerManager.load(localhost(cfg2.PEER_PORT));
    REQUIRE(r.second);
    REQUIRE(r.first.mNumFailures == 0);
}

TEST_CASE("peer is purged from database after few failures",
          "[overlay][acceptance]")
{
    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    auto simulation =
        std::make_shared<Simulation>(Simulation::OVER_TCP, networkID);
    auto record = [](int numFailures) {
        return PeerRecord{{}, numFailures, static_cast<int>(PeerType::INBOUND)};
    };

    SIMULATION_CREATE_NODE(Node1);

    SCPQuorumSet qSet;
    qSet.threshold = 1;
    qSet.validators.push_back(vNode1NodeID);

    Config cfg1 = getTestConfig(1);
    Config cfg2 = getTestConfig(2);

    cfg1.PEER_AUTHENTICATION_TIMEOUT = 1;

    cfg2.MAX_INBOUND_PENDING_CONNECTIONS = 0;
    cfg2.MAX_OUTBOUND_PENDING_CONNECTIONS = 4; // to prevent changes in adjust()

    auto app1 = simulation->addNode(vNode1SecretKey, qSet, &cfg1);

    simulation->startAllNodes();

    auto& om = app1->getOverlayManager();
    auto& peerManager = om.getPeerManager();
    peerManager.store(localhost(cfg2.PEER_PORT), record(119), false);
    REQUIRE(peerManager.load(localhost(cfg2.PEER_PORT)).second);

    simulation->crankForAtLeast(std::chrono::seconds{5}, true);

    REQUIRE(!peerManager.load(localhost(cfg2.PEER_PORT)).second);
}
}
