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
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include "util/make_unique.h"

#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "medida/timer.h"

using namespace stellar;

TEST_CASE("loopback peer hello", "[overlay]")
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

TEST_CASE("loopback peer with 0 port", "[overlay]")
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

TEST_CASE("loopback peer send auth before hello", "[overlay]")
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

TEST_CASE("failed auth", "[overlay]")
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
    REQUIRE(app1->getMetrics()
                .NewMeter({"overlay", "drop", "recv-message-mac"}, "drop")
                .count() != 0);
}

TEST_CASE("reject non-preferred peer", "[overlay]")
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
    REQUIRE(app2->getMetrics()
                .NewMeter({"overlay", "drop", "recv-auth-reject"}, "drop")
                .count() != 0);
}

TEST_CASE("accept preferred peer even when strict", "[overlay]")
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

TEST_CASE("reject peers beyond max", "[overlay]")
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
    REQUIRE(app2->getMetrics()
                .NewMeter({"overlay", "drop", "recv-auth-reject"}, "drop")
                .count() == 1);
}

TEST_CASE("allow pending peers beyond max", "[overlay]")
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

TEST_CASE("reject pending beyond max", "[overlay]")
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

TEST_CASE("reject peers with differing network passphrases", "[overlay]")
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
    REQUIRE(app2->getMetrics()
                .NewMeter({"overlay", "drop", "recv-hello-cert"}, "drop")
                .count() != 0);
}

TEST_CASE("reject peers with invalid cert", "[overlay]")
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
    REQUIRE(app1->getMetrics()
                .NewMeter({"overlay", "drop", "recv-hello-cert"}, "drop")
                .count() != 0);
}

TEST_CASE("reject banned peers", "[overlay]")
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
    REQUIRE(app1->getMetrics()
                .NewMeter({"overlay", "drop", "recv-hello-ban"}, "drop")
                .count() != 0);
}

TEST_CASE("reject peers with incompatible overlay versions", "[overlay]")
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
        REQUIRE(app2->getMetrics()
                    .NewMeter({"overlay", "drop", "recv-hello-version"}, "drop")
                    .count() != 0);
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

TEST_CASE("reject peers who don't handshake quickly", "[overlay]")
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

TEST_CASE("reject peers with the same nodeid", "[overlay]")
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
    REQUIRE(app2->getMetrics()
                .NewMeter({"overlay", "drop", "recv-hello-peerid"}, "drop")
                .count() != 0);
}
