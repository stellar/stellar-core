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


using namespace soci;

#define SECONDS_PER_BACKOFF 10

// TODO.3 some tests
// TODO.3 flood older msgs to people that connect to you

/*
Connection process:
A wants to connect to B
A initiates a tcp connection to B
connection is established
A sends HELLO to B
B now has IP and listening port of A
B either:
    sends HELLO back, or 
    sends list of other peers to connect to and disconnects
*/

namespace stellar
{

using namespace soci;
using namespace std;

PeerMaster::PeerMaster(Application& app)
    : mApp(app)
    , mDoor(mApp)
    , mTimer(app.getClock())
    , mFloodGate(app)
{
    mTimer.expires_from_now(std::chrono::seconds(2));
    
    if (!mApp.getConfig().RUN_STANDALONE)
    {
        mTimer.async_wait([this](asio::error_code const& ec)
                          {
                              if (!ec)
                              {
                                  addConfigPeers();
                                  this->tick();
                              }
                          });
    }
}

PeerMaster::~PeerMaster()
{
}

// LATER: verify ip and port are valid
bool PeerMaster::parseIPPort(const std::string& peerStr, std::string& retIP, int& retPort)
{
    std::string const innerStr(peerStr);
    std::string::const_iterator splitPoint =
        std::find(innerStr.rbegin(), innerStr.rend(), ':').base();
    if(splitPoint == innerStr.end())
    {
        retIP = innerStr;
        retPort = DEFAULT_PEER_PORT;
    }else
    {
        splitPoint--;
        retIP.assign(innerStr.begin(), splitPoint);
        std::string portStr;
        splitPoint++;
        portStr.assign(splitPoint, innerStr.end());
        retPort = atoi(portStr.c_str());
        if(!retPort) return false;
    }
    return true;
}

void PeerMaster::connectTo(const std::string& peerStr)
{
    std::string ip;
    int port;
    if(parseIPPort(peerStr, ip, port))
    {
        mApp.getDatabase().addPeer(ip, port, 0, 2);
        if(!getPeer(ip, port))
        {
            auto now = mApp.getClock().now();
            now += 2 * std::chrono::seconds(2 * SECONDS_PER_BACKOFF);
            std::tm nextAttempt = VirtualClock::pointToTm(now);

            mApp.getDatabase().getSession() << "UPDATE Peers set numFailures=numFailures+1 and nextAttempt=:v1 where ip=:v2 and port=:v3",
                use(nextAttempt), use(ip), use(port);
            addPeer(Peer::pointer(new TCPPeer(mApp, ip, port)));
        }
    } else
    {
        CLOG(ERROR, "overlay") << "couldn't parse peer: " << peerStr;
    }
}

void PeerMaster::addPeerList(const std::vector<std::string>& list, int rank)
{
    for(auto peerStr : mApp.getConfig().KNOWN_PEERS)
    {
        std::string ip;
        int port;
        if(parseIPPort(peerStr, ip, port))
        {
            mApp.getDatabase().addPeer(ip, port, 0, rank);
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
        vector<PeerRecord> retList;
        mApp.getDatabase().loadPeers(100, retList);
        for(auto peerRecord : retList)
        {
            if(!getPeer(peerRecord.mIP, peerRecord.mPort))
            {
                auto now = mApp.getClock().now();
                now += std::chrono::seconds(
                    static_cast<int64_t>(
                        pow(2, peerRecord.mNumFailures + 1) * SECONDS_PER_BACKOFF));
                std::tm nextAttempt = VirtualClock::pointToTm(now);
                mApp.getDatabase().getSession() << "UPDATE Peers set numFailures=numFailures+1 and nextAttempt=:v1 where peerID=:v2",
                    use(nextAttempt), use(peerRecord.mPeerID);
                addPeer(Peer::pointer(new TCPPeer(mApp, peerRecord.mIP, peerRecord.mPort)));
                if (mPeers.size() >= mApp.getConfig().TARGET_PEER_CONNECTIONS)
                {
                    break;
                }
            }
        }
    }


    mTimer.expires_from_now(std::chrono::seconds(2));
    mTimer.async_wait([this](asio::error_code const& ec)
                      {
                          if (!ec)
                          {
                              this->tick();
                          }
                      });
}

Peer::pointer PeerMaster::getPeer(const std::string& ip, int port)
{
    for(auto peer : mPeers)
    {
        if(peer->getIP() == ip && peer->getRemoteListeningPort() == port)
        {
            return peer;
        }
    }
    return Peer::pointer();
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
PeerMaster::recvFloodedMsg(StellarMessage const& msg,Peer::pointer peer)
{
    mFloodGate.addRecord(msg, peer);
}


void
PeerMaster::broadcastMessage(StellarMessage const& msg)
{
    mFloodGate.broadcast(msg);
}




void PeerMaster::createTable(Database &db)
{
    if (db.isSqlite())
    {
        // Horrendous hack: replace "SERIAL" with "INTEGER" when
        // on SQLite:
        std::string q(kSQLCreateStatement);
        auto p = q.find("SERIAL");
        assert(p != std::string::npos);
        q.replace(p, 6, "INTEGER");
        db.getSession() << q.c_str();
    }
    else
    {
        db.getSession() << kSQLCreateStatement;
    }
}

const char* PeerMaster::kSQLCreateStatement = "CREATE TABLE IF NOT EXISTS Peers (						\
	peerID	SERIAL PRIMARY KEY,	\
    ip	    CHARACTER(11),		        \
    port   	INT DEFAULT 0 CHECK (port >= 0),		\
    nextAttempt   	TIMESTAMP,	    	\
    numFailures     INT DEFAULT 0 CHECK (numFailures >= 0),      \
    lastConnect   	TIMESTAMP,	    	\
	rank	INT DEFAULT 0 CHECK (rank >= 0)  	\
);";

}
