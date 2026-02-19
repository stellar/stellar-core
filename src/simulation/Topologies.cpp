// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "simulation/Topologies.h"
#include "crypto/SHA.h"

namespace stellar
{
using namespace std;

Simulation::pointer
Topologies::pair(Hash const& networkID, Simulation::ConfigGen confGen,
                 Simulation::QuorumSetAdjuster qSetAdjust)
{
    Simulation::pointer simulation =
        make_shared<Simulation>(networkID, confGen, qSetAdjust);

    SIMULATION_CREATE_NODE(10);
    SIMULATION_CREATE_NODE(11);

    SCPQuorumSet qSet0;
    qSet0.threshold = 2;
    qSet0.validators.push_back(v10NodeID);
    qSet0.validators.push_back(v11NodeID);

    simulation->addNode(v10SecretKey, qSet0);
    simulation->addNode(v11SecretKey, qSet0);

    simulation->addPendingConnection(v10SecretKey.getPublicKey(),
                                     v11SecretKey.getPublicKey());
    return simulation;
}

Simulation::pointer
Topologies::cycle4(Hash const& networkID, Simulation::ConfigGen confGen,
                   Simulation::QuorumSetAdjuster qSetAdjust)
{
    Simulation::pointer simulation =
        make_shared<Simulation>(networkID, confGen, qSetAdjust);

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

    simulation->addNode(v0SecretKey, qSet0);
    simulation->addNode(v1SecretKey, qSet1);
    simulation->addNode(v2SecretKey, qSet2);
    simulation->addNode(v3SecretKey, qSet3);

    simulation->addPendingConnection(v0SecretKey.getPublicKey(),
                                     v1SecretKey.getPublicKey());
    simulation->addPendingConnection(v1SecretKey.getPublicKey(),
                                     v2SecretKey.getPublicKey());
    simulation->addPendingConnection(v2SecretKey.getPublicKey(),
                                     v3SecretKey.getPublicKey());
    simulation->addPendingConnection(v3SecretKey.getPublicKey(),
                                     v0SecretKey.getPublicKey());

    simulation->addPendingConnection(v0SecretKey.getPublicKey(),
                                     v2SecretKey.getPublicKey());
    simulation->addPendingConnection(v1SecretKey.getPublicKey(),
                                     v3SecretKey.getPublicKey());

    return simulation;
}

Simulation::pointer
Topologies::separate(int nNodes, double quorumThresoldFraction,
                     Hash const& networkID, int numWatchers,
                     Simulation::ConfigGen confGen,
                     Simulation::QuorumSetAdjuster qSetAdjust)
{
    Simulation::pointer simulation =
        make_shared<Simulation>(networkID, confGen, qSetAdjust);

    vector<SecretKey> keys;
    for (int i = 0; i < nNodes; i++)
    {
        keys.push_back(
            SecretKey::fromSeed(sha256("NODE_SEED_" + to_string(i))));
    }

    SCPQuorumSet qSet;
    assert(quorumThresoldFraction >= 0.5);
    qSet.threshold =
        min(nNodes, static_cast<int>(ceil(nNodes * quorumThresoldFraction)));
    for (auto const& k : keys)
    {
        qSet.validators.push_back(k.getPublicKey());
    }

    for (auto const& k : keys)
    {
        simulation->addNode(k, qSet);
    }

    for (int i = 0; i < numWatchers; i++)
    {
        simulation->addNode(
            SecretKey::fromSeed(sha256("NODE_SEED_WATCHER_" + to_string(i))),
            qSet);
    }
    return simulation;
}

Simulation::pointer
Topologies::separateAllHighQuality(int nNodes, Hash const& networkID,
                                   Simulation::ConfigGen confGen)
{
    Simulation::pointer simulation =
        make_shared<Simulation>(networkID, confGen);

    vector<SecretKey> keys;
    vector<ValidatorEntry> validatorEntries;
    for (int i = 0; i < nNodes; i++)
    {
        SecretKey const& key = keys.emplace_back(
            SecretKey::fromSeed(sha256("NODE_SEED_" + to_string(i))));
        ValidatorEntry& ve = validatorEntries.emplace_back();
        ve.mName = "validator" + to_string(i);
        ve.mHomeDomain = "hd" + to_string(i);
        ve.mQuality = ValidatorQuality::VALIDATOR_HIGH_QUALITY;
        ve.mKey = key.getPublicKey();
        ve.mHasHistory = false;
    }

    for (auto const& k : keys)
    {
        simulation->addNode(k, validatorEntries);
    }

    return simulation;
}

Simulation::pointer
Topologies::core(int nNodes, double quorumThresoldFraction,
                 Hash const& networkID, Simulation::ConfigGen confGen,
                 Simulation::QuorumSetAdjuster qSetAdjust)
{
    auto simulation = Topologies::separate(nNodes, quorumThresoldFraction,
                                           networkID, 0, confGen, qSetAdjust);

    auto nodes = simulation->getNodeIDs();
    assert(static_cast<int>(nodes.size()) == nNodes);

    return simulation;
}

Simulation::pointer
Topologies::cycle(int nNodes, double quorumThresoldFraction,
                  Hash const& networkID, Simulation::ConfigGen confGen,
                  Simulation::QuorumSetAdjuster qSetAdjust)
{
    auto simulation = Topologies::separate(nNodes, quorumThresoldFraction,
                                           networkID, 0, confGen, qSetAdjust);

    auto nodes = simulation->getNodeIDs();
    assert(static_cast<int>(nodes.size()) == nNodes);

    return simulation;
}

Simulation::pointer
Topologies::branchedcycle(int nNodes, double quorumThresoldFraction,
                          Hash const& networkID, Simulation::ConfigGen confGen,
                          Simulation::QuorumSetAdjuster qSetAdjust)
{
    auto simulation = Topologies::separate(nNodes, quorumThresoldFraction,
                                           networkID, 0, confGen, qSetAdjust);

    auto nodes = simulation->getNodeIDs();
    assert(static_cast<int>(nodes.size()) == nNodes);

    return simulation;
}

Simulation::pointer
Topologies::hierarchicalQuorum(
    int nBranches, Hash const& networkID, Simulation::ConfigGen confGen,
    int connectionsToCore,
    Simulation::QuorumSetAdjuster qSetAdjust) // Figure 3 from the paper
{
    auto sim = Topologies::core(4, 0.75, networkID, confGen, qSetAdjust);
    vector<NodeID> coreNodeIDs;
    for (auto const& coreNodeID : sim->getNodeIDs())
    {
        coreNodeIDs.push_back(coreNodeID);
    }

    SCPQuorumSet qSetTopTier;
    qSetTopTier.threshold = 2;
    for (auto const& coreNodeID : coreNodeIDs)
    {
        qSetTopTier.validators.push_back(coreNodeID);
    }

    for (int i = 0; i < nBranches; i++)
    {
        // middle tier nodes
        vector<SecretKey> middletierKeys;
        for (int j = 0; j < 1; j++)
        {
            middletierKeys.push_back(SecretKey::fromSeed(sha256(
                "NODE_SEED_" + to_string(i) + "_middle_" + to_string(j))));
        }

        //// the leaf node
        // SCPQuorumSet leafQSet;
        // leafQSet.threshold = 3;
        // SecretKey leafKey =
        // SecretKey::fromSeed(sha256("NODE_SEED_" + to_string(i) +
        // "_leaf"));
        // leafQSet.validators.push_back(leafKey.getPublicKey());
        // for(auto const& key : middletierKeys)
        //{
        //    leafQSet.validators.push_back(key.getPublicKey());
        //}
        // sim->addNode(leafKey, leafQSet);
    }
    return sim;
}

Simulation::pointer
Topologies::hierarchicalQuorumSimplified(
    int coreSize, int nbOuterNodes, Hash const& networkID,
    Simulation::ConfigGen confGen, int connectionsToCore,
    Simulation::QuorumSetAdjuster qSetAdjust)
{
    // outer nodes are independent validators that point to a [core network]
    auto sim = Topologies::core(coreSize, 0.75, networkID, confGen, qSetAdjust);

    // each additional node considers themselves as validator
    // with a quorum set that also includes the core
    int n = coreSize + 1;
    SCPQuorumSet qSetBuilder;
    qSetBuilder.threshold = n - (n - 1) / 3;
    vector<NodeID> coreNodeIDs;
    for (auto const& coreNodeID : sim->getNodeIDs())
    {
        qSetBuilder.validators.push_back(coreNodeID);
        coreNodeIDs.emplace_back(coreNodeID);
    }
    qSetBuilder.validators.emplace_back();

    return sim;
}

Simulation::pointer
Topologies::customA(Hash const& networkID, Simulation::ConfigGen confGen,
                    int connections, Simulation::QuorumSetAdjuster qSetAdjust)
{
    Simulation::pointer s =
        make_shared<Simulation>(networkID, confGen, qSetAdjust);

    enum kIDs
    {
        A = 0,
        B,
        C,
        T,
        I,
        E,
        S
    };
    vector<SecretKey> keys;
    for (int i = 0; i < 7; i++)
    {
        keys.push_back(
            SecretKey::fromSeed(sha256("NODE_SEED_" + to_string(i))));
    }
    // A,B,C have the same qset, with all validators
    {
        SCPQuorumSet q;
        q.threshold = 4;
        for (auto& k : keys)
        {
            q.validators.emplace_back(k.getPublicKey());
        }
        s->addNode(keys[A], q);
        s->addNode(keys[B], q);
        s->addNode(keys[C], q);
    }
    // T
    {
        SCPQuorumSet q;
        q.threshold = 4;
        q.validators.emplace_back(keys[B].getPublicKey());
        q.validators.emplace_back(keys[A].getPublicKey());
        q.validators.emplace_back(keys[T].getPublicKey());
        q.validators.emplace_back(keys[E].getPublicKey());
        q.validators.emplace_back(keys[S].getPublicKey());
        s->addNode(keys[T], q);
    }
    // E
    {
        SCPQuorumSet q;
        q.threshold = 3;
        q.validators.emplace_back(keys[E].getPublicKey());
        q.validators.emplace_back(keys[A].getPublicKey());
        q.validators.emplace_back(keys[B].getPublicKey());
        q.validators.emplace_back(keys[C].getPublicKey());
        s->addNode(keys[E], q);
    }
    // S
    {
        SCPQuorumSet q;
        q.threshold = 4;
        q.validators.emplace_back(keys[S].getPublicKey());
        q.validators.emplace_back(keys[E].getPublicKey());
        q.validators.emplace_back(keys[A].getPublicKey());
        q.validators.emplace_back(keys[B].getPublicKey());
        q.validators.emplace_back(keys[C].getPublicKey());
        s->addNode(keys[S], q);
    }

    return s;
}

Simulation::pointer
Topologies::asymmetric(Hash const& networkID, Simulation::ConfigGen confGen,
                       int connections,
                       Simulation::QuorumSetAdjuster qSetAdjust)
{

    Simulation::pointer s =
        Topologies::core(10, 0.7, networkID, confGen, qSetAdjust);
    auto node = s->getNodes()[0];

    enum kIDs
    {
        A = 0,
        B,
        C,
        D,
    };

    vector<SecretKey> keys;
    for (int i = 0; i < 4; i++)
    {
        keys.push_back(
            SecretKey::fromSeed(sha256("TIER_1_NODE_SEED_" + to_string(i))));
    }
    keys.push_back(node->getConfig().NODE_SEED);

    // A,B,C,D have the same qset, with all validators
    {
        SCPQuorumSet q;
        q.threshold = 5;
        for (auto& k : keys)
        {
            q.validators.emplace_back(k.getPublicKey());
        }
        s->addNode(keys[A], q);
        s->addNode(keys[B], q);
        s->addNode(keys[C], q);
        s->addNode(keys[D], q);
    }

    return s;
}
}
