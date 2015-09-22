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

void crankSome(VirtualClock& clock)
{
    for (size_t i = 0; i < 100 && clock.crank(false) > 0; ++i)
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
    REQUIRE(app1->getMetrics().NewMeter(
                {"overlay", "drop", "recv-message-mac"},
                "drop").count() != 0);
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
    REQUIRE(app2->getMetrics().NewMeter(
                {"overlay", "drop", "recv-auth-reject"},
                "drop").count() != 0);
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
    REQUIRE(app2->getMetrics().NewMeter(
                {"overlay", "drop", "recv-auth-reject"},
                "drop").count() != 0);
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
    REQUIRE(app2->getMetrics().NewMeter(
                {"overlay", "drop", "recv-hello-net"},
                "drop").count() != 0);
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
    REQUIRE(app1->getMetrics().NewMeter(
                {"overlay", "drop", "recv-hello-cert"},
                "drop").count() != 0);
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
    REQUIRE(app2->getMetrics().NewMeter(
                {"overlay", "drop", "recv-hello-version"},
                "drop").count() != 0);
}
