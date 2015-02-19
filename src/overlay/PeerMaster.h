#pragma once

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

class PeerMasterTests;

class PeerMaster : public OverlayGateway
{
    Application& mApp;
    // peers we are connected to
    std::vector<Peer::pointer> mPeers;
    
    PeerDoor mDoor;

    void tick();
    VirtualTimer mTimer;

    void addPeerList(const std::vector<std::string>& list,int rank);
    void addConfigPeers();
    bool isPeerPreferred(Peer::pointer peer);

    friend class PeerMasterTests;

  public:
    Floodgate mFloodGate;

    PeerMaster(Application& app);
    ~PeerMaster();

    //////// GATEWAY FUNCTIONS
    void ledgerClosed(LedgerHeader& ledger);

    void recvFloodedMsg(StellarMessage const& msg, 
                        Peer::pointer peer);

    void broadcastMessage(StellarMessage const& msg);
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

    static void createTable(Database &db);

};
}


