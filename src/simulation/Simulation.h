// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "crypto/SHA.h"
#include "main/Application.h"
#include "main/Config.h"
#include "medida/medida.h"
#include "overlay/StellarXDR.h"
#include "simulation/LoadGenerator.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "util/Timer.h"
#include "util/XDROperators.h"
#include "xdr/Stellar-types.h"

#define SIMULATION_CREATE_NODE(N) \
    const Hash v##N##VSeed = sha256("NODE_SEED_" #N); \
    const SecretKey v##N##SecretKey = SecretKey::fromSeed(v##N##VSeed); \
    const PublicKey v##N##NodeID = v##N##SecretKey.getPublicKey();

namespace stellar
{
/**
 * Simulation manages a cluster of stellar-core nodes for testing.
 *
 * All nodes use RustOverlayManager with real networking via QUIC.
 * Peer discovery happens through Kademlia DHT - nodes find each other
 * via KNOWN_PEERS configuration.
 */
class Simulation
{
  public:
    using pointer = std::shared_ptr<Simulation>;
    using ConfigGen = std::function<Config(int i)>;
    using QuorumSetAdjuster = std::function<SCPQuorumSet(SCPQuorumSet const&)>;
    using QuorumSetSpec =
        std::variant<SCPQuorumSet, std::vector<ValidatorEntry>>;

    Simulation(Hash const& networkID, ConfigGen = nullptr,
               QuorumSetAdjuster = nullptr);
    ~Simulation();

    // updates all clocks in the simulation to the same time_point
    void setCurrentVirtualTime(VirtualClock::time_point t);
    void setCurrentVirtualTime(VirtualClock::system_time_point t);

    // Add new node to the simulation. This function does not start the node.
    // Callers are expected to call `start` or `startAllNodes` manually.
    // QuorumSetSpec can be either an explicit SCPQuorumSet, or a vector of
    // ValidatorEntry for automatic quorum set generation. Automatic quorum set
    // configuration is incompatible with QuorumSetAdjuster.
    Application::pointer addNode(SecretKey nodeKey, QuorumSetSpec qSet,
                                 Config const* cfg = nullptr,
                                 bool newDB = true);
    Application::pointer getNode(NodeID nodeID);
    std::vector<Application::pointer> getNodes();
    std::vector<NodeID> getNodeIDs();
    void
    addPendingConnection(NodeID const& initiator, NodeID const& acceptor)
    {
    }

    void startAllNodes();
    void stopAllNodes();
    void removeNode(NodeID const& id);

    // returns true if all nodes have externalized
    // triggers and exception if a node externalized higher than num+maxSpread
    bool haveAllExternalized(uint32 num, uint32 maxSpread,
                             bool validatorsOnly = false);

    size_t crankNode(NodeID const& id, VirtualClock::time_point timeout);
    size_t crankAllNodes(int nbTicks = 1);
    void crankForAtMost(VirtualClock::duration seconds, bool finalCrank);
    void crankForAtLeast(VirtualClock::duration seconds, bool finalCrank);
    void crankUntil(std::function<bool()> const& fn,
                    VirtualClock::duration timeout, bool finalCrank);
    void crankUntil(VirtualClock::time_point timePoint, bool finalCrank);
    void crankUntil(VirtualClock::system_time_point timePoint, bool finalCrank);
    std::string metricsSummary(std::string domain = "");

    Config newConfig(); // generates a new config

    std::chrono::milliseconds getExpectedLedgerCloseTime() const;

    bool
    isSetUpForSorobanUpgrade() const
    {
        return mSetupForSorobanUpgrade;
    }

    void
    markReadyForSorobanUpgrade()
    {
        mSetupForSorobanUpgrade = true;
    }

  private:
    // Configure KNOWN_PEERS on all nodes so they can discover each other
    void configureKnownPeers();

    VirtualClock mClock;
    int mConfigCount;
    Application::pointer mIdleApp;

    struct Node
    {
        std::shared_ptr<VirtualClock> mClock;
        Application::pointer mApp;

        ~Node()
        {
            // app must be destroyed before its clock
            mApp.reset();
        }
    };
    std::map<NodeID, Node> mNodes;

    ConfigGen mConfigGen; // config generator

    QuorumSetAdjuster mQuorumSetAdjuster;

    std::chrono::milliseconds const quantum = std::chrono::milliseconds(100);

    bool mSetupForSorobanUpgrade{false};
};
}
