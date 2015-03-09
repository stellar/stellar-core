#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "Peer.h"
#include "PeerDoor.h"
#include "PeerRecord.h"
#include "overlay/ItemFetcher.h"
#include "overlay/Floodgate.h"
#include <vector>
#include <thread>
#include "generated/StellarXDR.h"
#include "overlay/OverlayGateway.h"
#include "util/Timer.h"

namespace medida { class Meter; }

/*
Maintain the set of peers we are connected to
*/
namespace stellar
{

class PeerMaster : public OverlayGateway
{
protected:
    Application& mApp;
    // peers we are connected to
    std::vector<Peer::pointer> mPeers;
    PeerDoor::pointer mDoor;

    medida::Meter& mMessagesReceived;
    medida::Meter& mMessagesBroadcast;
    medida::Meter& mConnectionsAttempted;
    medida::Meter& mConnectionsEstablished;
    medida::Meter& mConnectionsDropped;

    void tick();
    VirtualTimer mTimer;

    void storePeerList(const std::vector<std::string>& list,int rank);
    void storeConfigPeers();
    bool isPeerPreferred(Peer::pointer peer);

    friend class PeerMasterTests;

  public:
    Floodgate mFloodGate;

    PeerMaster(Application& app);
    ~PeerMaster();

    //////// GATEWAY FUNCTIONS
    void ledgerClosed(LedgerHeaderHistoryEntry const& ledger);

    void recvFloodedMsg(StellarMessage const& msg, 
                        Peer::pointer peer);

    void broadcastMessage(StellarMessage const& msg, bool force=false);
    //////

    void connectTo(const std::string& addr);
    virtual void connectTo(PeerRecord &pr);
    void connectToMorePeers(int max);
    void addConnectedPeer(Peer::pointer peer);
    void dropPeer(Peer::pointer peer);
    bool isPeerAccepted(Peer::pointer peer);
    std::vector<Peer::pointer>& getPeers() { return mPeers;  }

    Peer::pointer getConnectedPeer(const std::string& ip, int port);
    Peer::pointer getRandomPeer();
    // returns NULL if the passed peer isn't found
    Peer::pointer getNextPeer(Peer::pointer peer);

    static void dropAll(Database &db);

};
}


