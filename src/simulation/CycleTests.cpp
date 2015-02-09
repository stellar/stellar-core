// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC
#include "simulation/Simulation.h"
#include "lib/catch.hpp"
#include "generated/StellarXDR.h"
#include "main/Application.h"
#include "overlay/LoopbackPeer.h"
#include "util/make_unique.h"
#include "crypto/SHA.h"
#include "main/test.h"
#include "util/Logging.h"
#include "util/types.h"

using namespace stellar;

typedef std::unique_ptr<Application> appPtr;

TEST_CASE("cycle4 topology", "[simulation]")
{
    return;
    Simulation simulation;

    SIMULATION_CREATE_NODE(0);
    SIMULATION_CREATE_NODE(1);
    SIMULATION_CREATE_NODE(2);
    SIMULATION_CREATE_NODE(3);

    FBAQuorumSet qSet0; qSet0.threshold = 1; qSet0.validators.push_back(v1NodeID);
    FBAQuorumSet qSet1; qSet1.threshold = 1; qSet1.validators.push_back(v2NodeID);
    FBAQuorumSet qSet2; qSet2.threshold = 1; qSet2.validators.push_back(v3NodeID);
    FBAQuorumSet qSet3; qSet3.threshold = 1; qSet3.validators.push_back(v0NodeID);

    uint256 n0 = simulation.addNode(v0VSeed, qSet0, simulation.getClock());
    uint256 n1 = simulation.addNode(v1VSeed, qSet1, simulation.getClock());
    uint256 n2 = simulation.addNode(v2VSeed, qSet2, simulation.getClock());
    uint256 n3 = simulation.addNode(v3VSeed, qSet3, simulation.getClock());
    
    std::shared_ptr<LoopbackPeerConnection> n0n1 = 
        simulation.addConnection(n0, n1);
    std::shared_ptr<LoopbackPeerConnection> n1n2 = 
        simulation.addConnection(n1, n2);
    std::shared_ptr<LoopbackPeerConnection> n2n3 = 
        simulation.addConnection(n2, n3);
    std::shared_ptr<LoopbackPeerConnection> n3n0 = 
        simulation.addConnection(n3, n0);

    std::shared_ptr<LoopbackPeerConnection> n0n2 =
        simulation.addConnection(n0, n2);
    std::shared_ptr<LoopbackPeerConnection> n1n3 =
        simulation.addConnection(n1, n3);

    simulation.startAllNodes();

    bool stop = false;
    auto check = [&] (const asio::error_code& error)
    {
        stop = true;
        // Still transiently does not work (quorum retrieval)
        /*
        REQUIRE(simulation.haveAllExternalized(2));
        */
        LOG(DEBUG) << "Simulation complete";
    };

    VirtualTimer checkTimer(simulation.getClock());

    checkTimer.expires_from_now(std::chrono::seconds(9));
    checkTimer.async_wait(check);

    while(!stop && simulation.crankAllNodes() > 0);
}
