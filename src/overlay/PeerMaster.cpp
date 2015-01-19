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
#include "overlay/TCPPeer.h"

namespace stellar
{

using namespace soci;
using namespace std;

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

// LATER: verify ip and port are valid
bool PeerMaster::parseIPPort(std::string& peerStr, std::string& retIP, int& retPort)
{
    std::string::iterator splitPoint =
        std::find(peerStr.rbegin(), peerStr.rend(), ':').base();
    if(splitPoint == peerStr.end())
    {
        retIP = peerStr;
        retPort = DEFAULT_PEER_PORT;
    }else
    {
        retIP.assign(peerStr.begin(), splitPoint);
        std::string portStr;
        splitPoint++;
        portStr.assign(splitPoint, peerStr.end());
        retPort = atoi(portStr.c_str());
        if(!retPort) return false;
    }
    return true;
}

void PeerMaster::addPeerList(const std::vector<std::string>& list, int rank)
{
    for(auto peerStr : mApp.getConfig().KNOWN_PEERS)
    {
        std::string ip;
        int port;
        if(parseIPPort(peerStr, ip, port))
        {
            mApp.getDatabase().addPeer(ip, port, rank);
        } else
        {
            CLOG(ERROR, "overlay") << "couldn't parse peer: " << peerStr;
        }
    }
}

void PeerMaster::addConfigPeers()
{
    addPeerList(mApp.getConfig().KNOWN_PEERS, 2);
    addPeerList(mApp.getConfig().PREFERRED_PEERS, 10);
}

// called every 2 seconds
void
PeerMaster::tick()
{
    // if we have too few peers try to connect to more
    LOG(DEBUG) << "PeerMaster tick";
    if (mPeers.size() < mApp.getConfig().TARGET_PEER_CONNECTIONS)
    {
        // make some outbound connections if we can
        int num = mApp.getConfig().TARGET_PEER_CONNECTIONS - mPeers.size();
        vector<PeerRecord> retList;
        mApp.getDatabase().loadPeers(num, retList);
        for(auto addr : retList)
        {
            addPeer(Peer::pointer(new TCPPeer(mApp,addr.mIP,addr.mPort)));
            // TODO.3 update DB
        }
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
    int count = 0;
    int port = peer->getRemoteListeningPort();
    std::string const& ip = peer->getIP();
   
    mApp.getDatabase().getSession() <<
        "SELECT count(*) from Peers where rank>9 and ip=:v1 and port=:v2",
        into(count), use(ip), use(port);

    return count;
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
