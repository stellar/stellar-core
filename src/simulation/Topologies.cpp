// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

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

    auto n0 = simulation->addNode(v10SecretKey, qSet0, simulation->getClock());
    auto n1 = simulation->addNode(v11SecretKey, qSet0, simulation->getClock());

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
    qSet0.threshold = 2;
    qSet0.validators.push_back(v1NodeID);
    qSet0.validators.push_back(v0NodeID);
    SCPQuorumSet qSet1;
    qSet1.threshold = 2;
    qSet1.validators.push_back(v1NodeID);
    qSet1.validators.push_back(v2NodeID);
    SCPQuorumSet qSet2;
    qSet2.threshold = 2;
    qSet2.validators.push_back(v2NodeID);
    qSet2.validators.push_back(v3NodeID);
    SCPQuorumSet qSet3;
    qSet3.threshold = 2;
    qSet3.validators.push_back(v3NodeID);
    qSet3.validators.push_back(v0NodeID);

    auto n0 = simulation->addNode(v0SecretKey, qSet0, simulation->getClock());
    auto n1 = simulation->addNode(v1SecretKey, qSet1, simulation->getClock());
    auto n2 = simulation->addNode(v2SecretKey, qSet2, simulation->getClock());
    auto n3 = simulation->addNode(v3SecretKey, qSet3, simulation->getClock());

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
Topologies::core(int nNodes, float quorumThresoldFraction,
                 Simulation::Mode mode)
{
    Simulation::pointer simulation = make_shared<Simulation>(mode);

    vector<SecretKey> keys;
    for (int i = 0; i < nNodes; i++)
    {
        keys.push_back(SecretKey::fromSeed(
            sha256("SEED_VALIDATION_SEED_" + to_string(i))));
    }

    SCPQuorumSet qSet;
    assert(quorumThresoldFraction >= 0.5);
    qSet.threshold = min(
        nNodes, static_cast<int>(ceil(nNodes * quorumThresoldFraction)) + 1);
    for (auto const& k : keys)
    {
        qSet.validators.push_back(k.getPublicKey());
    }

    for (auto const& k : keys)
    {
        simulation->addNode(k, qSet, simulation->getClock());
    }
    for (int from = 0; from < nNodes - 1; from++)
    {
        for (int to = from + 1; to < nNodes; to++)
        {
            simulation->addConnection(keys[from].getPublicKey(),
                                      keys[to].getPublicKey());
        }
    }

    return simulation;
}

Simulation::pointer
Topologies::hierarchicalQuorum(int nBranches,
                               Simulation::Mode mode) // Figure 2 from the paper
{
    auto sim = Topologies::core(4, 1.0, mode);
    vector<uint256> coreNodeIDs;
    for (auto const& coreNodeID : sim->getNodeIDs())
    {
        coreNodeIDs.push_back(coreNodeID);
    }

    for (int i = 0; i < nBranches; i++)
    {
        // middle tier nodes
        vector<SecretKey> middletierKeys;
        for (int j = 0; j < 1; j++)
        {
            middletierKeys.push_back(SecretKey::fromSeed(
                sha256("SEED_VALIDATION_SEED_" + to_string(i) + "_middle_" +
                       to_string(j))));
        }

        SCPQuorumSet qSet;
        qSet.threshold = 3;
        for (auto const& coreNodeID : coreNodeIDs)
        {
            qSet.validators.push_back(coreNodeID);
        }
        for (auto const& key : middletierKeys)
        {
            SCPQuorumSet qSetHere = qSet;
            qSetHere.validators.push_back(key.getPublicKey());
            sim->addNode(key, qSet, sim->getClock());
        }

        //// the leaf node
        // SCPQuorumSet leafQSet;
        // leafQSet.threshold = 3;
        // SecretKey leafKey =
        // SecretKey::fromSeed(sha256("SEED_VALIDATION_SEED_" + to_string(i) +
        // "_leaf"));
        // leafQSet.validators.push_back(leafKey.getPublicKey());
        // for(auto const& key : middletierKeys)
        //{
        //    leafQSet.validators.push_back(key.getPublicKey());
        //}
        // sim->addNode(leafKey, leafQSet, sim->getClock());

        // connections
        for (auto const& middle : middletierKeys)
        {
            for (auto const& core : coreNodeIDs)
                sim->addConnection(middle.getPublicKey(), core);

            // sim->addConnection(leafKey.getPublicKey(),
            // middle.getPublicKey());
        }
    }
    return sim;
}
}
