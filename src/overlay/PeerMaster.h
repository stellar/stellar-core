#ifndef __PEERMASTER__
#define __PEERMASTER__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "Peer.h"
#include "PeerDoor.h"
#include "overlay/ItemFetcher.h"
#include "overlay/Floodgate.h"
#include <vector>
#include <thread>
#include "generated/StellarXDR.h"
#include "overlay/OverlayGateway.h"
#include "util/Timer.h"

/*
Maintain the set of peers we are connected to
*/
namespace stellar
{

class PeerMaster : public OverlayGateway
{
    Application& mApp;
    // peers we are connected to
    std::vector<Peer::pointer> mPeers;
    
    PeerDoor mDoor;

    void tick();
    VirtualTimer mTimer;

    bool parseIPPort(const std::string& peerStr,std::string& retIP,int& retPort);
    void addConfigPeers();
    void addPeerList(const std::vector<std::string>& list,int rank);
    bool isPeerPreferred(Peer::pointer peer);

  public:
    Floodgate mFloodGate;

    PeerMaster(Application& app);
    ~PeerMaster();

    //////// GATEWAY FUNCTIONS
    void ledgerClosed(LedgerHeader& ledger);

    void recvFloodedMsg(uint256 const& messageID, 
                        StellarMessage const& msg, 
                        uint32_t ledgerIndex,
                        Peer::pointer peer);

    void broadcastMessage(StellarMessage const& msg,
                          Peer::pointer peer);
    //////

    void connectTo(const std::string& addr);
    void addPeer(Peer::pointer peer);
    void dropPeer(Peer::pointer peer);
    bool isPeerAccepted(Peer::pointer peer);
    std::vector<Peer::pointer>& getPeers() { return mPeers;  }

    Peer::pointer getPeer(const std::string& ip, int port);
    Peer::pointer getRandomPeer();
    // returns NULL if the passed peer isn't found
    Peer::pointer getNextPeer(Peer::pointer peer);

    void broadcastMessage(uint256 const& msgID);
    void broadcastMessage(StellarMessage const& msg,
                          std::vector<Peer::pointer> const& skip);

    static void createTable(Database &db);
    static const char *kSQLCreateStatement;
};
}

#endif
