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
  typedef std::unique_ptr<Application> appPtr;

  private:
    VirtualClock mClock;
    std::map<stellarxdr::uint256, Application::pointer> mNodes;
    std::vector<std::shared_ptr<LoopbackPeerConnection>> mConnections;
  
  public:
    Simulation();
    ~Simulation();

    Application::pointer addNode(stellarxdr::uint256 nodeID);
    Application::pointer getNode(stellarxdr::uint256 nodeID);

    std::shared_ptr<LoopbackPeerConnection> 
        addConnection(stellarxdr::uint256 initiator, 
                      stellarxdr::uint256 acceptor);

    std::size_t advanceNode(stellarxdr::uint256 nodeID, int nbTicks);
    std::size_t advanceAllNodes(int nbTicks);
};
}

#endif
