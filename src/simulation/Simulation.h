#ifndef __SIMULATION__
#define __SIMULATION__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "main/Application.h"
#include "overlay/LoopbackPeer.h"
#include "generated/StellarXDR.h"

namespace stellar
{
class Simulation
{
  private:
    VirtualClock mClock;
    std::map<stellarxdr::uint256, Application::pointer> mNodes;
    std::map<stellarxdr::uint256, Config::pointer> mConfigs;
    std::vector<std::shared_ptr<LoopbackPeerConnection>> mConnections;
  
  public:
    Simulation();
    ~Simulation();

    VirtualClock& getClock();

    stellarxdr::uint256 addNode(stellarxdr::uint256 validationSeed, 
                                VirtualClock& clock);
    Application::pointer getNode(stellarxdr::uint256 nodeID);

    std::shared_ptr<LoopbackPeerConnection> 
        addConnection(stellarxdr::uint256 initiator, 
                      stellarxdr::uint256 acceptor);

    std::size_t crankNode(stellarxdr::uint256 nodeID, int nbTicks=1);
    std::size_t crankAllNodes(int nbTicks=1);
};
}

#endif
