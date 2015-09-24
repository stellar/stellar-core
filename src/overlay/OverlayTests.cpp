// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Application.h"
#include "overlay/LoopbackPeer.h"
#include "util/make_unique.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "util/Timer.h"
#include "crypto/SecretKey.h"
#include "main/Config.h"
#include "overlay/PeerRecord.h"
#include "overlay/OverlayManagerImpl.h"

#include "medida/metrics_registry.h"
#include "medida/timer.h"
#include "medida/meter.h"

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
        PubKeyUtils::toStrKey(cfg1.NODE_SEED.getPublicKey()));

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

TEST_CASE("reject peers with differing overlay versions", "[overlay]")
{
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(0);
    Config cfg2 = getTestConfig(1);

    cfg2.OVERLAY_PROTOCOL_VERSION = 0xdeadbeef;

    auto app1 = Application::create(clock, cfg1);
    auto app2 = Application::create(clock, cfg2);

    LoopbackPeerConnection conn(*app1, *app2);
    crankSome(clock);

    REQUIRE(!conn.getInitiator()->isConnected());
    REQUIRE(!conn.getAcceptor()->isConnected());
    REQUIRE(app2->getMetrics()
                .NewMeter({"overlay", "drop", "recv-hello-version"}, "drop")
                .count() != 0);
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
    while (clock.now() < (start + std::chrono::seconds(3)) &&
           conn.getInitiator()->isConnected() &&
           conn.getAcceptor()->isConnected())
    {
        LOG(INFO) << "clock.now() = " << clock.now().time_since_epoch().count();
        clock.crank(false);
    }
    REQUIRE(clock.now() < (start + std::chrono::seconds(5)));
    REQUIRE(!conn.getInitiator()->isConnected());
    REQUIRE(!conn.getAcceptor()->isConnected());
    REQUIRE(app2->getMetrics()
                .NewMeter({"overlay", "timeout", "read"}, "timeout")
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
