
#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SHA.h"
#include "main/Application.h"
#include "main/Config.h"
#include "medida/medida.h"
#include "overlay/StellarXDR.h"
#include "overlay/test/LoopbackPeer.h"
#include "simulation/LoadGenerator.h"
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
class Simulation
{
  public:
    enum Mode
    {
        OVER_TCP,
        OVER_LOOPBACK
    };

    using pointer = std::shared_ptr<Simulation>;
    using ConfigGen = std::function<Config(int i)>;
    using QuorumSetAdjuster = std::function<SCPQuorumSet(SCPQuorumSet const&)>;

    Simulation(Mode mode, Hash const& networkID, ConfigGen = nullptr,
               QuorumSetAdjuster = nullptr);
    ~Simulation();

    // updates all clocks in the simulation to the same time_point
    void setCurrentVirtualTime(VirtualClock::time_point t);
    void setCurrentVirtualTime(VirtualClock::system_time_point t);

    Application::pointer addNode(SecretKey nodeKey, SCPQuorumSet qSet,
                                 Config const* cfg = nullptr, bool newDB = true,
                                 uint32_t startAtLedger = 0,
                                 std::string const& startAtHash = "");
    Application::pointer getNode(NodeID nodeID);
    std::vector<Application::pointer> getNodes();
    std::vector<NodeID> getNodeIDs();

    void addPendingConnection(NodeID const& initiator, NodeID const& acceptor);
    // Returns LoopbackPeerConnection given initiator, acceptor pair or nullptr
    std::shared_ptr<LoopbackPeerConnection>
    getLoopbackConnection(NodeID const& initiator, NodeID const& acceptor);
    void startAllNodes();
    void stopAllNodes();
    void removeNode(NodeID const& id);

    // returns true if all nodes have externalized
    // triggers and exception if a node externalized higher than num+maxSpread
    bool haveAllExternalized(uint32 num, uint32 maxSpread);

    size_t crankNode(NodeID const& id, VirtualClock::time_point timeout);
    size_t crankAllNodes(int nbTicks = 1);
    void crankForAtMost(VirtualClock::duration seconds, bool finalCrank);
    void crankForAtLeast(VirtualClock::duration seconds, bool finalCrank);
    void crankUntil(std::function<bool()> const& fn,
                    VirtualClock::duration timeout, bool finalCrank);
    void crankUntil(VirtualClock::time_point timePoint, bool finalCrank);
    void crankUntil(VirtualClock::system_time_point timePoint, bool finalCrank);
    std::string metricsSummary(std::string domain = "");

    void addConnection(NodeID initiator, NodeID acceptor);
    void dropConnection(NodeID initiator, NodeID acceptor);
    Config newConfig(); // generates a new config

  private:
    void addLoopbackConnection(NodeID initiator, NodeID acceptor);
    void dropLoopbackConnection(NodeID initiator, NodeID acceptor);
    void addTCPConnection(NodeID initiator, NodeID acception);
    void dropAllConnections(NodeID const& id);

    bool mVirtualClockMode;
    VirtualClock mClock;
    Mode mMode;
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
    std::vector<std::pair<NodeID, NodeID>> mPendingConnections;
    std::vector<std::shared_ptr<LoopbackPeerConnection>> mLoopbackConnections;

    ConfigGen mConfigGen; // config generator

    QuorumSetAdjuster mQuorumSetAdjuster;

    std::chrono::milliseconds const quantum = std::chrono::milliseconds(100);
};
}
