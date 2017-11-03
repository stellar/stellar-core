// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/LoopbackPeer.h"
#include "overlay/OverlayManager.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Timer.h"
#include <lib/catch.hpp>
#include <medida/metrics_registry.h>

using namespace stellar;

TEST_CASE("disconnect peer when overloaded", "[overlay][LoadManager]")
{
    VirtualClock clock;
    auto const& cfg1 = getTestConfig(0);
    auto cfg2 = getTestConfig(1);
    auto const& cfg3 = getTestConfig(2);

    cfg2.RUN_STANDALONE = false;
    cfg2.MINIMUM_IDLE_PERCENT = 90;
    cfg2.TARGET_PEER_CONNECTIONS = 0;

    auto app1 = createTestApplication(clock, cfg1);
    auto app2 = createTestApplication(clock, cfg2);
    auto app3 = createTestApplication(clock, cfg3);

    LoopbackPeerConnection conn(*app1, *app2);
    LoopbackPeerConnection conn2(*app3, *app2);

    testutil::crankSome(clock);
    app2->getOverlayManager().start();

    // app1 and app3 are both connected to app2. app1 will hammer on the
    // connection, app3 will do nothing. app2 should disconnect app1.
    // but app3 should remain connected since the i/o timeout is 30s.
    auto start = clock.now();
    auto end = start + std::chrono::seconds(10);
    VirtualTimer timer(clock);

    testutil::injectSendPeersAndReschedule(end, clock, timer, conn);

    for (size_t i = 0;
         (i < 1000 && clock.now() < end && clock.crank(false) > 0); ++i)
        ;

    REQUIRE(!conn.getInitiator()->isConnected());
    REQUIRE(!conn.getAcceptor()->isConnected());
    REQUIRE(conn2.getInitiator()->isConnected());
    REQUIRE(conn2.getAcceptor()->isConnected());
    REQUIRE(app2->getMetrics()
                .NewMeter({"overlay", "drop", "load-shed"}, "drop")
                .count() != 0);
}
