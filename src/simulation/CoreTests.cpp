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

TEST_CASE("core4 topology", "[simulation]")
{
    Simulation simulation;

    SIMULATION_CREATE_NODE(0);
    SIMULATION_CREATE_NODE(1);
    SIMULATION_CREATE_NODE(2);
    SIMULATION_CREATE_NODE(3);

    FBAQuorumSet qSet; 
    qSet.threshold = 3; 
    qSet.validators.push_back(v0NodeID);
    qSet.validators.push_back(v1NodeID);
    qSet.validators.push_back(v2NodeID);
    qSet.validators.push_back(v3NodeID);

    uint256 n0 = simulation.addNode(v0VSeed, qSet, simulation.getClock());
    uint256 n1 = simulation.addNode(v1VSeed, qSet, simulation.getClock());
    uint256 n2 = simulation.addNode(v2VSeed, qSet, simulation.getClock());
    uint256 n3 = simulation.addNode(v3VSeed, qSet, simulation.getClock());
    
    std::shared_ptr<LoopbackPeerConnection> n0n1 = 
        simulation.addConnection(n0, n1);
    std::shared_ptr<LoopbackPeerConnection> n0n2 = 
        simulation.addConnection(n0, n2);
    std::shared_ptr<LoopbackPeerConnection> n0n3 = 
        simulation.addConnection(n0, n3);
    std::shared_ptr<LoopbackPeerConnection> n1n2 = 
        simulation.addConnection(n1, n2);
    std::shared_ptr<LoopbackPeerConnection> n1n3 = 
        simulation.addConnection(n1, n3);
    std::shared_ptr<LoopbackPeerConnection> n2n3 = 
        simulation.addConnection(n2, n3);

    simulation.startAllNodes();

    bool stop = false;
    auto check = [&] (const asio::error_code& error)
    {
        stop = true;
        REQUIRE(simulation.haveAllExternalized(3));
        LOG(DEBUG) << "Simulation complete";
    };

    VirtualTimer checkTimer(simulation.getClock());

    checkTimer.expires_from_now(std::chrono::seconds(2));
    checkTimer.async_wait(check);

    while(!stop && simulation.crankAllNodes() > 0);
}
