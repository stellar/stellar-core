// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "simulation/Topologies.h"
#include "crypto/SHA.h"

namespace stellar
{
using namespace std;

Simulation::pointer
Topologies::pair(Simulation::Mode mode)
{
    Simulation::pointer simulation = make_shared<Simulation>(mode);

    SIMULATION_CREATE_NODE(10);
    SIMULATION_CREATE_NODE(11);

    SCPQuorumSet qSet0;
    qSet0.threshold = 2;
    qSet0.validators.push_back(v10NodeID);
    qSet0.validators.push_back(v11NodeID);

    auto n0 = simulation->addNode(v10VSeed, qSet0, simulation->getClock());
    auto n1 = simulation->addNode(v11VSeed, qSet0, simulation->getClock());

    simulation->addConnection(n0, n1);
    return simulation;
}

Simulation::pointer
Topologies::cycle4()
{
    Simulation::pointer simulation =
        make_shared<Simulation>(Simulation::OVER_LOOPBACK);

    SIMULATION_CREATE_NODE(0);
    SIMULATION_CREATE_NODE(1);
    SIMULATION_CREATE_NODE(2);
    SIMULATION_CREATE_NODE(3);

    SCPQuorumSet qSet0;
    qSet0.threshold = 1;
    qSet0.validators.push_back(v1NodeID);
    SCPQuorumSet qSet1;
    qSet1.threshold = 1;
    qSet1.validators.push_back(v2NodeID);
    SCPQuorumSet qSet2;
    qSet2.threshold = 1;
    qSet2.validators.push_back(v3NodeID);
    SCPQuorumSet qSet3;
    qSet3.threshold = 1;
    qSet3.validators.push_back(v0NodeID);

    auto n0 = simulation->addNode(v0VSeed, qSet0, simulation->getClock());
    auto n1 = simulation->addNode(v1VSeed, qSet1, simulation->getClock());
    auto n2 = simulation->addNode(v2VSeed, qSet2, simulation->getClock());
    auto n3 = simulation->addNode(v3VSeed, qSet3, simulation->getClock());

    std::shared_ptr<LoopbackPeerConnection> n0n1 =
        simulation->addLoopbackConnection(n0, n1);
    std::shared_ptr<LoopbackPeerConnection> n1n2 =
        simulation->addLoopbackConnection(n1, n2);
    std::shared_ptr<LoopbackPeerConnection> n2n3 =
        simulation->addLoopbackConnection(n2, n3);
    std::shared_ptr<LoopbackPeerConnection> n3n0 =
        simulation->addLoopbackConnection(n3, n0);

    std::shared_ptr<LoopbackPeerConnection> n0n2 =
        simulation->addLoopbackConnection(n0, n2);
    std::shared_ptr<LoopbackPeerConnection> n1n3 =
        simulation->addLoopbackConnection(n1, n3);

    return simulation;
}

Simulation::pointer
Topologies::core3(Simulation::Mode mode)
{
    Simulation::pointer simulation =
        make_shared<Simulation>(mode);

    SIMULATION_CREATE_NODE(0);
    SIMULATION_CREATE_NODE(1);
    SIMULATION_CREATE_NODE(2);

    SCPQuorumSet qSet;
    qSet.threshold = 2;
    qSet.validators.push_back(v0NodeID);
    qSet.validators.push_back(v1NodeID);
    qSet.validators.push_back(v2NodeID);

    auto n0 = simulation->addNode(v0VSeed, qSet, simulation->getClock());
    auto n1 = simulation->addNode(v1VSeed, qSet, simulation->getClock());
    auto n2 = simulation->addNode(v2VSeed, qSet, simulation->getClock());

    simulation->addConnection(n0, n1);
    simulation->addConnection(n0, n2);
    simulation->addConnection(n1, n2);

    return simulation;
}

Simulation::pointer
Topologies::core4(Simulation::Mode mode)
{
    Simulation::pointer simulation =
        make_shared<Simulation>(mode);

    SIMULATION_CREATE_NODE(0);
    SIMULATION_CREATE_NODE(1);
    SIMULATION_CREATE_NODE(2);
    SIMULATION_CREATE_NODE(3);

    SCPQuorumSet qSet;
    qSet.threshold = 3;
    qSet.validators.push_back(v0NodeID);
    qSet.validators.push_back(v1NodeID);
    qSet.validators.push_back(v2NodeID);
    qSet.validators.push_back(v3NodeID);

    auto n0 = simulation->addNode(v0VSeed, qSet, simulation->getClock());
    auto n1 = simulation->addNode(v1VSeed, qSet, simulation->getClock());
    auto n2 = simulation->addNode(v2VSeed, qSet, simulation->getClock());
    auto n3 = simulation->addNode(v3VSeed, qSet, simulation->getClock());

    simulation->addConnection(n0, n1);
    simulation->addConnection(n0, n2);
    simulation->addConnection(n0, n3);
    simulation->addConnection(n1, n2);
    simulation->addConnection(n1, n3);
    simulation->addConnection(n2, n3);

    return simulation;
}
}
