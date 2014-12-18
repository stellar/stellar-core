// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "simulation/Simulation.h"
#include "main/test.h"
#include "util/Logging.h"

namespace stellar
{

Simulation::Simulation()
{
}

Simulation::~Simulation()
{
    // tear down
    std::map<stellarxdr::uint256, Application::pointer>::iterator it;
    for (it = mNodes.begin(); it != mNodes.end(); ++it) {
        it->second->getMainIOService().poll_one();
        it->second->getMainIOService().stop();
    }
}

Application::pointer
Simulation::addNode(stellarxdr::uint256 nodeID)
{
    if (!mNodes[nodeID]) 
    {
        Config const& cfg = getTestConfig();
        mNodes[nodeID] = std::make_shared<Application>(cfg);
    }
    return mNodes[nodeID];
}

Application::pointer
Simulation::getNode(stellarxdr::uint256 nodeID)
{
    return mNodes[nodeID];
}

std::shared_ptr<LoopbackPeerConnection>
Simulation::addConnection(stellarxdr::uint256 initiator, 
                          stellarxdr::uint256 acceptor)
{
    std::shared_ptr<LoopbackPeerConnection> connection;
    if (mNodes[initiator] && mNodes[acceptor]) 
    {
        connection = std::make_shared<LoopbackPeerConnection>(
            *getNode(initiator), *getNode(acceptor));
        mConnections.emplace_back(connection);
    }
    return connection;
}


std::size_t
Simulation::advanceNode(stellarxdr::uint256 nodeID, int nbTicks)
{
    std::size_t count = 0;
    if (mNodes[nodeID])
    {
        for (int i = 0; i < nbTicks && nbTicks > 0; i ++)
            count += mNodes[nodeID]->getMainIOService().poll_one();
    }
    return count;
}

std::size_t
Simulation::advanceAllNodes(int nbTicks)
{
    std::size_t count = 0;
    for (int i = 0; i < nbTicks && nbTicks > 0; i ++)
    {
        std::map<stellarxdr::uint256, Application::pointer>::iterator it;
        for (it = mNodes.begin(); it != mNodes.end(); ++it) {
            count += it->second->getMainIOService().poll_one();
        }
    }
    return count;
}

}
