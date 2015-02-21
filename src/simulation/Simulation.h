#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "main/Config.h"
#include "main/Application.h"
#include "overlay/LoopbackPeer.h"
#include "generated/StellarXDR.h"
#include "util/Timer.h"

#define SIMULATION_CREATE_NODE(N) \
    const Hash v##N##VSeed = sha256("SEED_VALIDATION_SEED_" #N);    \
    const SecretKey v##N##SecretKey = SecretKey::fromSeed(v##N##VSeed); \
    const Hash v##N##NodeID = v##N##SecretKey.getPublicKey();

namespace stellar
{
class Simulation
{
  private:
    VirtualClock mClock;
    int mConfigCount;
    std::map<uint256, Config::pointer> mConfigs;
    std::map<uint256, Application::pointer> mNodes;
    std::vector<std::shared_ptr<LoopbackPeerConnection>> mConnections;
  
  public:
    Simulation();
    ~Simulation();

    VirtualClock& getClock();

    uint256 addNode(uint256 validationSeed, 
                    FBAQuorumSet qSet,
                    VirtualClock& clock);
    Application::pointer getNode(uint256 nodeID);

    std::shared_ptr<LoopbackPeerConnection> 
        addConnection(uint256 initiator, 
                      uint256 acceptor);

    void startAllNodes();

    bool haveAllExternalized(int num);

    std::size_t crankNode(uint256 nodeID, int nbTicks=1);
    std::size_t crankAllNodes(int nbTicks=1);
};
}


