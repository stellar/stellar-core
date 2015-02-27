// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "Simulation.h"

#include "main/test.h"
#include "util/Logging.h"
#include "util/types.h"
#include "ledger/LedgerMaster.h"
#include "overlay/PeerRecord.h"
#include "main/Application.h"
#include "overlay/PeerMaster.h"

namespace stellar
{

using namespace std;

Simulation::Simulation(bool isStandalone) :
    mIsStandAlone(isStandalone)
  , mConfigCount(0)
  , mIdleApp(Application::create(mClock, getTestConfig(++mConfigCount)))
{
}

Simulation::~Simulation()
{
    // tear down
    std::map<uint256, Application::pointer>::iterator it;
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

uint256
Simulation::addNode(uint256 validationSeed, 
                    FBAQuorumSet qSet,
                    VirtualClock& clock)
{
    Config::pointer cfg = std::make_shared<Config>(getTestConfig(++mConfigCount));

    cfg->VALIDATION_KEY = SecretKey::fromSeed(validationSeed);
    cfg->QUORUM_THRESHOLD = qSet.threshold;
    cfg->RUN_STANDALONE = mIsStandAlone;

    for (auto q : qSet.validators)
    {
        cfg->QUORUM_SET.push_back(q);
    }

    Application::pointer result = Application::create(clock, *cfg);

    if (!mIsStandAlone) 
        result->enableRealTimer();

    uint256 nodeID = makePublicKey(validationSeed);
    mConfigs[nodeID] = cfg;
    mNodes[nodeID] = result;

    return nodeID;
}

Application::pointer
Simulation::getNode(uint256 nodeID)
{
    return mNodes[nodeID];
}

std::shared_ptr<LoopbackPeerConnection>
Simulation::addLoopbackConnection(uint256 initiator, 
                          uint256 acceptor)
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
Simulation::addTCPConnection(uint256 initiator,
                             uint256 acceptor)
{
    if (mIsStandAlone)
    {
        throw new runtime_error("Cannot add a TCP connection to a standalone network");
    }
    auto from = getNode(initiator);
    auto to = getNode(acceptor);
    PeerRecord pr{"127.0.0.1", to->getConfig().PEER_PORT, from->getClock().now(), 0, 10};
    from->getPeerMaster().connectTo(pr);
}

void 
Simulation::startAllNodes()
{
    // We wait for the connections to set up (HELLO).
    while(crankAllNodes() > 0);

    for(auto it : mNodes)
    {
        it.second->start();
    }
}

std::size_t
Simulation::crankNode(uint256 nodeID, int nbTicks)
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
        std::map<uint256, Application::pointer>::iterator it;
        for (it = mNodes.begin(); it != mNodes.end(); ++it) {
            count += it->second->crank(false);
        }
    }
    return count;
}

bool Simulation::haveAllExternalized(int num)
{
    uint64_t min = INT_MAX;
    for(auto it = mNodes.begin(); it != mNodes.end(); ++it) 
    {
        auto n = it->second->getLedgerMaster().getLedgerNum();
        LOG(DEBUG) << "Ledger#: " << n;

        if (n < min)
            min = n;
    }
    return num <= min;
}

void
Simulation::crankForAtMost(VirtualClock::duration seconds)
{
    bool stop = false;
    auto stopIt = [&](const asio::error_code& error)
    {
        stop = true;
    };

    VirtualTimer checkTimer(*mIdleApp);

    checkTimer.expires_from_now(seconds);
    checkTimer.async_wait(stopIt);

    while (!stop && crankAllNodes() > 0);

    if (stop)
        LOG(INFO) << "Simulation timed out";
    else LOG(INFO) << "Simulation complete";
}

void
Simulation::crankForAtLeast(VirtualClock::duration seconds)
{
    bool stop = false;
    auto stopIt = [&](const asio::error_code& error)
    {
        stop = true;
    };

    VirtualTimer checkTimer(*mIdleApp);

    checkTimer.expires_from_now(seconds);
    checkTimer.async_wait(stopIt);

    while (!stop)
    {
        if (crankAllNodes() == 0)
            this_thread::sleep_for(chrono::milliseconds(50));
    }

    if (stop)
        LOG(INFO) << "Simulation timed out";
    else LOG(INFO) << "Simulation complete";
}

}
