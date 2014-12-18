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

    stellarxdr::uint256 n1 = fromBase58("1");
    stellarxdr::uint256 n2 = fromBase58("2");
    stellarxdr::uint256 n3 = fromBase58("3");
    stellarxdr::uint256 n4 = fromBase58("4");

    simulation.addNode(n1);
    simulation.addNode(n2);
    simulation.addNode(n3);
    simulation.addNode(n4);
    
    std::shared_ptr<LoopbackPeerConnection> n1n2 = 
      simulation.addConnection(n1, n2);
    std::shared_ptr<LoopbackPeerConnection> n2n3 = 
      simulation.addConnection(n2, n3);
    std::shared_ptr<LoopbackPeerConnection> n3n4 = 
      simulation.addConnection(n3, n4);
    std::shared_ptr<LoopbackPeerConnection> n4n1 = 
      simulation.addConnection(n4, n1);

    simulation.advanceAllNodes(100);
    
}
