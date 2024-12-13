
#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SHA.h"
#include "main/Application.h"
#include "main/Config.h"
#include "medida/medida.h"
#include "overlay/OverlayManagerImpl.h"
#include "overlay/StellarXDR.h"
#include "overlay/test/LoopbackPeer.h"
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

    // Add new node to the simulation. This function does not start the node.
    // Callers are expected to call `start` or `startAllNodes` manually.
    Application::pointer addNode(SecretKey nodeKey, SCPQuorumSet qSet,
                                 Config const* cfg = nullptr,
                                 bool newDB = true);
    Application::pointer getNode(NodeID nodeID);
    std::vector<Application::pointer> getNodes();
    std::vector<NodeID> getNodeIDs();

    // Add a pending connection to an unstarted node. Typically called after
    // `addNode`, but before `startAllNodes`. No-op if the simulation is already
    // started.
    void addPendingConnection(NodeID const& initiator, NodeID const& acceptor);
    // Returns LoopbackPeerConnection given initiator, acceptor pair or nullptr
    std::shared_ptr<LoopbackPeerConnection>
    getLoopbackConnection(NodeID const& initiator, NodeID const& acceptor);
    void startAllNodes();
    void stopAllNodes();
    void removeNode(NodeID const& id);

    Application::pointer getAppFromPeerMap(unsigned short peerPort);

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

    // Add a real (not pending) connection to the simulation. Works even if the
    // simulation has started.
    void addConnection(NodeID initiator, NodeID acceptor);
    void dropConnection(NodeID initiator, NodeID acceptor);
    Config newConfig(); // generates a new config
    // prevent overlay from automatically re-connecting to peers
    void stopOverlayTick();

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

    // Map PEER_PORT to Application
    std::unordered_map<unsigned short, std::weak_ptr<Application>> mPeerMap;

    bool mSetupForSorobanUpgrade{false};
};

class LoopbackOverlayManager : public OverlayManagerImpl
{
  public:
    LoopbackOverlayManager(Application& app) : OverlayManagerImpl(app)
    {
    }
    virtual bool connectToImpl(PeerBareAddress const& address,
                               bool forceoutbound) override;
};

class ApplicationLoopbackOverlay : public TestApplication
{
    Simulation& mSim;

  public:
    ApplicationLoopbackOverlay(VirtualClock& clock, Config const& cfg,
                               Simulation& sim)
        : TestApplication(clock, cfg), mSim(sim)
    {
    }

    virtual LoopbackOverlayManager&
    getOverlayManager() override
    {
        auto& overlay = ApplicationImpl::getOverlayManager();
        return static_cast<LoopbackOverlayManager&>(overlay);
    }

    Simulation&
    getSim()
    {
        return mSim;
    }

  private:
    virtual std::unique_ptr<OverlayManager>
    createOverlayManager() override
    {
        return std::make_unique<LoopbackOverlayManager>(*this);
    }
};
}
