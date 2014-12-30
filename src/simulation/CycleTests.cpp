// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "lib/catch.hpp"
#include "generated/StellarXDR.h"
#include "main/Application.h"
#include "overlay/LoopbackPeer.h"
#include "util/make_unique.h"
#include "util/types.h"
#include "main/test.h"
#include "util/Logging.h"
#include "simulation/Simulation.h"

using namespace stellar;

typedef std::unique_ptr<Application> appPtr;

TEST_CASE("cycle4 topology", "[simulation]")
{
    Simulation simulation;

    stellar::uint256 n1Seed = fromBase58("SEED_1");
    stellar::uint256 n2Seed = fromBase58("SEED_2");
    stellar::uint256 n3Seed = fromBase58("SEED_3");
    stellar::uint256 n4Seed = fromBase58("SEED_4");

    stellar::uint256 n1 = simulation.addNode(n1Seed, simulation.getClock());
    stellar::uint256 n2 = simulation.addNode(n2Seed, simulation.getClock());
    stellar::uint256 n3 = simulation.addNode(n3Seed, simulation.getClock());
    stellar::uint256 n4 = simulation.addNode(n4Seed, simulation.getClock());
    
    std::shared_ptr<LoopbackPeerConnection> n1n2 = 
      simulation.addConnection(n1, n2);
    std::shared_ptr<LoopbackPeerConnection> n2n3 = 
      simulation.addConnection(n2, n3);
    std::shared_ptr<LoopbackPeerConnection> n3n4 = 
      simulation.addConnection(n3, n4);
    std::shared_ptr<LoopbackPeerConnection> n4n1 = 
      simulation.addConnection(n4, n1);

    stellar::SlotBallot ballot;
    ballot.ledgerIndex = 0;
    ballot.ballot.index = 1;
    ballot.ballot.closeTime = time(nullptr) + NUM_SECONDS_IN_CLOSE;
    simulation.getNode(n1)->getFBAGateway().startNewRound(ballot);

    while(simulation.crankAllNodes() > 0);
}
