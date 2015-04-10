// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Timer.h"
#include "TCPPeer.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/test.h"
#include "overlay/PeerDoor.h"
#include "main/Config.h"
#include "util/Logging.h"
#include "simulation/Simulation.h"
#include "overlay/OverlayManager.h"

namespace stellar
{

TEST_CASE("TCPPeer can communicate", "[overlay]")
{
    Simulation::pointer s = std::make_shared<Simulation>(Simulation::OVER_TCP);

    auto v10SecretKey = SecretKey::fromSeed(sha256("v10"));
    auto v11SecretKey = SecretKey::fromSeed(sha256("v11"));

    auto n0 = s->getNode(s->addNode(v10SecretKey, SCPQuorumSet(), s->getClock()));
    auto n1 = s->getNode(s->addNode(v11SecretKey, SCPQuorumSet(), s->getClock()));
    auto b = TCPPeer::initiate(*n0, "127.0.0.1", n1->getConfig().PEER_PORT);

    s->crankForAtLeast(std::chrono::seconds(3));

    REQUIRE(n0->getOverlayManager()
                .getConnectedPeer("127.0.0.1", n1->getConfig().PEER_PORT)
                ->getState() == Peer::GOT_HELLO);
    REQUIRE(n1->getOverlayManager()
                .getConnectedPeer("127.0.0.1", n0->getConfig().PEER_PORT)
                ->getState() == Peer::GOT_HELLO);
}
}
