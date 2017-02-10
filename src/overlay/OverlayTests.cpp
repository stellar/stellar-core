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
#include "test/test.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include "util/make_unique.h"

#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "medida/timer.h"

using namespace stellar;

void
crankSome(VirtualClock& clock)
{
    auto start = clock.now();
    for (size_t i = 0;
         (i < 100 && clock.now() < (start + std::chrono::seconds(1)) &&
          clock.crank(false) > 0);
         ++i)
        ;
}

TEST_CASE("loopback peer hello", "[overlay]")
{
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(0);
    Config const& cfg2 = getTestConfig(1);
    auto app1 = Application::create(clock, cfg1);
    auto app2 = Application::create(clock, cfg2);

    LoopbackPeerConnection conn(*app1, *app2);
    crankSome(clock);

    REQUIRE(conn.getInitiator()->isAuthenticated());
    REQUIRE(conn.getAcceptor()->isAuthenticated());
}

TEST_CASE("loopback peer with 0 port", "[overlay]")
{
    VirtualClock clock;
    auto const& cfg1 = getTestConfig(0);
    auto cfg2 = getTestConfig(1);
    cfg2.PEER_PORT = 0;

    auto app1 = Application::create(clock, cfg1);
    auto app2 = Application::create(clock, cfg2);

    LoopbackPeerConnection conn(*app1, *app2);
    crankSome(clock);

    REQUIRE(!conn.getInitiator()->isAuthenticated());
    REQUIRE(!conn.getAcceptor()->isAuthenticated());
}

TEST_CASE("loopback peer send auth before hello", "[overlay]")
{
    VirtualClock clock;
    auto const& cfg1 = getTestConfig(0);
    auto const& cfg2 = getTestConfig(1);
    auto app1 = Application::create(clock, cfg1);
    auto app2 = Application::create(clock, cfg2);

    LoopbackPeerConnection conn(*app1, *app2);
    conn.getInitiator()->sendAuth();
    crankSome(clock);

    REQUIRE(!conn.getInitiator()->isAuthenticated());
    REQUIRE(!conn.getAcceptor()->isAuthenticated());
}

TEST_CASE("failed auth", "[overlay]")
{
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(0);
    Config const& cfg2 = getTestConfig(1);
    auto app1 = Application::create(clock, cfg1);
    auto app2 = Application::create(clock, cfg2);

    LoopbackPeerConnection conn(*app1, *app2);
    conn.getInitiator()->setDamageAuth(true);
    crankSome(clock);

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

    auto app1 = Application::create(clock, cfg1);
    auto app2 = Application::create(clock, cfg2);

    LoopbackPeerConnection conn(*app1, *app2);
    crankSome(clock);

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

    auto app1 = Application::create(clock, cfg1);
    auto app2 = Application::create(clock, cfg2);

    LoopbackPeerConnection conn(*app1, *app2);
    crankSome(clock);

    REQUIRE(conn.getInitiator()->isAuthenticated());
    REQUIRE(conn.getAcceptor()->isAuthenticated());
}

TEST_CASE("reject peers beyond max", "[overlay]")
{
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(0);
    Config cfg2 = getTestConfig(1);

    cfg2.MAX_PEER_CONNECTIONS = 0;

    auto app1 = Application::create(clock, cfg1);
    auto app2 = Application::create(clock, cfg2);

    LoopbackPeerConnection conn(*app1, *app2);
    crankSome(clock);

    REQUIRE(!conn.getInitiator()->isConnected());
    REQUIRE(!conn.getAcceptor()->isConnected());
    REQUIRE(app2->getMetrics()
                .NewMeter({"overlay", "drop", "recv-auth-reject"}, "drop")
                .count() != 0);
}

TEST_CASE("reject peers with differing network passphrases", "[overlay]")
{
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(0);
    Config cfg2 = getTestConfig(1);

    cfg2.NETWORK_PASSPHRASE = "nothing to see here";

    auto app1 = Application::create(clock, cfg1);
    auto app2 = Application::create(clock, cfg2);

    LoopbackPeerConnection conn(*app1, *app2);
    crankSome(clock);

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

    auto app1 = Application::create(clock, cfg1);
    auto app2 = Application::create(clock, cfg2);

    LoopbackPeerConnection conn(*app1, *app2);
    conn.getAcceptor()->setDamageCert(true);
    crankSome(clock);

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

    auto app1 = Application::create(clock, cfg1);
    auto app2 = Application::create(clock, cfg2);
    app1->getBanManager().banNode(cfg2.NODE_SEED.getPublicKey());

    LoopbackPeerConnection conn(*app1, *app2);
    crankSome(clock);

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
        auto app1 = Application::create(clock, cfg1);
        auto app2 = Application::create(clock, cfg2);

        LoopbackPeerConnection conn(*app1, *app2);
        crankSome(clock);

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
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(0);
    Config cfg2 = getTestConfig(1);

    auto app1 = Application::create(clock, cfg1);
    auto app2 = Application::create(clock, cfg2);

    LoopbackPeerConnection conn(*app1, *app2);
    conn.getInitiator()->setCorked(true);
    auto start = clock.now();
    while (clock.now() < (start + std::chrono::seconds(6)) &&
           conn.getInitiator()->isConnected() &&
           conn.getAcceptor()->isConnected())
    {
        LOG(INFO) << "clock.now() = " << clock.now().time_since_epoch().count();
        clock.crank(false);
    }
    REQUIRE(clock.now() < (start + std::chrono::seconds(8)));
    REQUIRE(!conn.getInitiator()->isConnected());
    REQUIRE(!conn.getAcceptor()->isConnected());
    REQUIRE(app2->getMetrics()
                .NewMeter({"overlay", "timeout", "idle"}, "timeout")
                .count() != 0);
}

TEST_CASE("reject peers with the same nodeid", "[overlay]")
{
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(0);
    Config const& cfg2 = getTestConfig(1);
    Config cfg3 = getTestConfig(2);

    cfg3.NODE_SEED = cfg1.NODE_SEED;

    auto app1 = Application::create(clock, cfg1);
    auto app2 = Application::create(clock, cfg2);
    auto app3 = Application::create(clock, cfg3);

    LoopbackPeerConnection conn(*app1, *app2);
    LoopbackPeerConnection conn2(*app3, *app2);
    crankSome(clock);

    REQUIRE(conn.getInitiator()->isAuthenticated());
    REQUIRE(conn.getAcceptor()->isAuthenticated());
    REQUIRE(!conn2.getInitiator()->isConnected());
    REQUIRE(!conn2.getAcceptor()->isConnected());
    REQUIRE(app2->getMetrics()
                .NewMeter({"overlay", "drop", "recv-hello-peerid"}, "drop")
                .count() != 0);
}

void
injectSendPeersAndReschedule(VirtualClock::time_point& end, VirtualClock& clock,
                             VirtualTimer& timer,
                             std::shared_ptr<LoopbackPeer> const& sendPeer)
{
    sendPeer->sendGetPeers();
    if (clock.now() < end && sendPeer->isConnected())
    {
        timer.expires_from_now(std::chrono::milliseconds(10));
        timer.async_wait([&](asio::error_code const& ec) {
            if (!ec)
            {
                injectSendPeersAndReschedule(end, clock, timer, sendPeer);
            }
        });
    }
}

TEST_CASE("disconnect peers when overloaded", "[overlay]")
{
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(0);
    Config cfg2 = getTestConfig(1);
    Config const& cfg3 = getTestConfig(2);

    cfg2.RUN_STANDALONE = false;
    cfg2.MINIMUM_IDLE_PERCENT = 99;
    cfg2.TARGET_PEER_CONNECTIONS = 0;

    auto app1 = Application::create(clock, cfg1);
    auto app2 = Application::create(clock, cfg2);
    auto app3 = Application::create(clock, cfg3);

    LoopbackPeerConnection conn(*app1, *app2);
    LoopbackPeerConnection conn2(*app3, *app2);

    crankSome(clock);
    app2->getOverlayManager().start();

    // app1 and app3 are both connected to app2. app1 will hammer on the
    // connection, app3 will do nothing. app2 should disconnect app1.
    // but app3 should remain connected since the i/o timeout is 30s.
    auto start = clock.now();
    auto end = start + std::chrono::seconds(10);
    VirtualTimer timer(clock);

    injectSendPeersAndReschedule(end, clock, timer, conn.getInitiator());

    for (size_t i = 0;
         (i < 1000 && clock.now() < end && conn.getInitiator()->isConnected() &&
          clock.crank(false) > 0);
         ++i)
        ;

    REQUIRE(!conn.getInitiator()->isConnected());
    REQUIRE(!conn.getAcceptor()->isConnected());
    REQUIRE(conn2.getInitiator()->isConnected());
    REQUIRE(conn2.getAcceptor()->isConnected());
    REQUIRE(app2->getMetrics()
                .NewMeter({"overlay", "drop", "load-shed"}, "drop")
                .count() != 0);
}
