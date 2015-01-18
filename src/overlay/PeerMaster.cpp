// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "PeerMaster.h"
#include "main/Application.h"
#include "main/Config.h"
#include <thread>
#include <random>
#include "util/Logging.h"
#include "database/Database.h"

namespace stellar
{



PeerMaster::PeerMaster(Application& app)
    : mApp(app)
    , mDoor(mApp)
    , mTimer(app.getClock())
{
    mTimer.expires_from_now(std::chrono::seconds(2));
    
    if (!mApp.getConfig().RUN_STANDALONE)
    {
        addConfigPeers();
        mTimer.async_wait([this](asio::error_code const& ec)
                          {
                              this->tick();
                          });
    }
}

PeerMaster::~PeerMaster()
{
}

void PeerMaster::addConfigPeers()
{
    for(auto peerStr : mApp.getConfig().KNOWN_PEERS)
    {
        // TODO.3
    }

    for(auto peerStr : mApp.getConfig().PREFERRED_PEERS)
    {
        // TODO.3
    }
}

// called every 2 seconds
// If we have less than the target number of peers 
// we will try to connect to one out there
void
PeerMaster::tick()
{
    // if we have too few peers try to connect to more
    LOG(DEBUG) << "PeerMaster tick";
    if (mPeers.size() < mApp.getConfig().TARGET_PEER_CONNECTIONS)
    {
        // TODO.3 make some outbound connections if we can
        int num = mApp.getConfig().TARGET_PEER_CONNECTIONS - mPeers.size();

        // SELECT * from Peers where nextAttempt<now() order by rank limit :v1
    }
    

    mTimer.expires_from_now(std::chrono::seconds(2));
    mTimer.async_wait([this](asio::error_code const& ec)
                      {
                          this->tick();
                      });
}

void
PeerMaster::ledgerClosed(LedgerHeader& ledger)
{
    mFloodGate.clearBelow(ledger.ledgerSeq);
}

void
PeerMaster::addPeer(Peer::pointer peer)
{
    mPeers.push_back(peer);
}

void
PeerMaster::dropPeer(Peer::pointer peer)
{
    auto iter = find(mPeers.begin(), mPeers.end(), peer);
    if (iter != mPeers.end())
        mPeers.erase(iter);
    else
        CLOG(WARNING, "Overlay") << "Dropping unlisted peer";
}

bool
PeerMaster::isPeerAccepted(Peer::pointer peer)
{
    if (mPeers.size() < mApp.getConfig().MAX_PEER_CONNECTIONS)
        return true;
    return isPeerPreferred(peer);
}

bool PeerMaster::isPeerPreferred(Peer::pointer peer)
{
    int port = peer->getRemoteListeningPort();
    std::string const& ip = peer->getIP();
    // TODO.3
    // SELECT count(*) from Peers where rank>9 and ip=:v1 and port=:v2;
    return false;
}

Peer::pointer
PeerMaster::getRandomPeer()
{
    if (mPeers.size())
    {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<size_t> dis(0, mPeers.size() - 1);
        return mPeers[dis(gen)];
    }

    return Peer::pointer();
}

// returns NULL if the passed peer isn't found
Peer::pointer
PeerMaster::getNextPeer(Peer::pointer peer)
{
    for (unsigned int n = 0; n < mPeers.size(); n++)
    {
        if (mPeers[n] == peer)
        {
            if (n == mPeers.size() - 1)
                return mPeers[0];
            return (mPeers[n + 1]);
        }
    }
    return Peer::pointer();
}



void 
PeerMaster::recvFloodedMsg(uint256 const& messageID, 
                           StellarMessage const& msg, 
                           uint32_t ledgerIndex,
                           Peer::pointer peer)
{
    mFloodGate.addRecord(messageID, msg, ledgerIndex, peer);
}

void
PeerMaster::broadcastMessage(uint256 const& msgID)
{
    mFloodGate.broadcast(msgID, this);
}

void
PeerMaster::broadcastMessage(StellarMessage const& msg,
                             Peer::pointer peer)
{
    vector<Peer::pointer> tempList;
    tempList.push_back(peer);
    broadcastMessage(msg, tempList);
}

// send message to anyone you haven't gotten it from
void
PeerMaster::broadcastMessage(StellarMessage const& msg,
                             vector<Peer::pointer> const& skip)
{
    for (auto peer : mPeers)
    {
        if (find(skip.begin(), skip.end(), peer) == skip.end())
        {
            peer->sendMessage(msg);
        }
    }
}

void PeerMaster::createTable(Database &db)
{
    db.getSession() << kSQLCreateStatement;
}

const char* PeerMaster::kSQLCreateStatement = "CREATE TABLE IF NOT EXISTS Peers (						\
	peerID	INT UNSIGNED PRIMARY KEY,	\
    ip	    CHARACTER(11),		        \
    port   	INT UNSIGNED default 0,		\
    nextAttempt   	TIMESTAMP,	    	\
    numFailures     INT default 0,      \
    lastConnect   	TIMESTAMP,	    	\
	rank	INT UNSIGNED default 0  	\
);";

}
