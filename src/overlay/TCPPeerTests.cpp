// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "TCPPeer.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/OverlayManager.h"
#include "overlay/PeerBareAddress.h"
#include "overlay/PeerDoor.h"
#include "simulation/Simulation.h"
#include "test/test.h"
#include "util/Logging.h"
#include "util/Timer.h"

namespace stellar
{

TEST_CASE("TCPPeer can communicate", "[overlay]")
{
    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    Simulation::pointer s =
        std::make_shared<Simulation>(Simulation::OVER_TCP, networkID);

    auto v10SecretKey = SecretKey::fromSeed(sha256("v10"));
    auto v11SecretKey = SecretKey::fromSeed(sha256("v11"));

    SCPQuorumSet n0_qset;
    n0_qset.threshold = 1;
    n0_qset.validators.push_back(v10SecretKey.getPublicKey());
    auto n0 = s->addNode(v10SecretKey, n0_qset);

    SCPQuorumSet n1_qset;
    n1_qset.threshold = 1;
    n1_qset.validators.push_back(v11SecretKey.getPublicKey());
    auto n1 = s->addNode(v11SecretKey, n1_qset);

    s->addPendingConnection(v10SecretKey.getPublicKey(),
                            v11SecretKey.getPublicKey());
    s->startAllNodes();
    s->crankForAtLeast(std::chrono::seconds(1), false);

    auto p0 = n0->getOverlayManager().getConnectedPeer(
        PeerBareAddress{"127.0.0.1", n1->getConfig().PEER_PORT});

    auto p1 = n1->getOverlayManager().getConnectedPeer(
        PeerBareAddress{"127.0.0.1", n0->getConfig().PEER_PORT});

    REQUIRE(p0);
    REQUIRE(p1);
    REQUIRE(p0->isAuthenticated());
    REQUIRE(p1->isAuthenticated());
    s->stopAllNodes();
}
}
