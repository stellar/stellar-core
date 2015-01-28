// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "Simulation.h"

#include "main/test.h"
#include "util/Logging.h"
#include "util/types.h"

namespace stellar
{

using namespace std;

Simulation::Simulation()
{
}

Simulation::~Simulation()
{
    // tear down
    std::map<stellar::uint256, Application::pointer>::iterator it;
    for (it = mNodes.begin(); it != mNodes.end(); ++it) {
        it->second->getMainIOService().poll_one();
        it->second->getMainIOService().stop();
    }
}

VirtualClock& 
Simulation::getClock()
{
  return mClock;
}

stellar::uint256
Simulation::addNode(stellar::uint256 validationSeed, VirtualClock& clock)
{
    Config::pointer cfg = stellar::make_shared<Config>();
    cfg->LOG_FILE_PATH = getTestConfig().LOG_FILE_PATH;
    cfg->VALIDATION_KEY = SecretKey::fromSeed(validationSeed);
    cfg->RUN_STANDALONE = true;
    cfg->START_NEW_NETWORK = true;

    Application::pointer node = 
          std::make_shared<Application>(clock, *cfg);

    stellar::uint256 nodeID = makePublicKey(validationSeed);
    mConfigs[nodeID] = cfg;
    mNodes[nodeID] = node;

    return nodeID;
}

Application::pointer
Simulation::getNode(stellar::uint256 nodeID)
{
    return mNodes[nodeID];
}

std::shared_ptr<LoopbackPeerConnection>
Simulation::addConnection(stellar::uint256 initiator, 
                          stellar::uint256 acceptor)
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

void 
Simulation::startAllNodes()
{
    for(auto it : mNodes)
    {
        it.second->start();
    }
}

std::size_t
Simulation::crankNode(stellar::uint256 nodeID, int nbTicks)
{
    std::size_t count = 0;
    if (mNodes[nodeID])
    {
        for (int i = 0; i < nbTicks && nbTicks > 0; i ++)
            count += mNodes[nodeID]->crank(false);
    }
    return count;
}

std::size_t
Simulation::crankAllNodes(int nbTicks)
{
    std::size_t count = 0;
    for (int i = 0; i < nbTicks && nbTicks > 0; i ++)
    {
        std::map<stellar::uint256, Application::pointer>::iterator it;
        for (it = mNodes.begin(); it != mNodes.end(); ++it) {
            count += it->second->crank(false);
        }
    }
    return count;
}

}
