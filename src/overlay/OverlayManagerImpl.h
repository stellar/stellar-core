#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "Peer.h"
#include "PeerDoor.h"
#include "PeerRecord.h"
#include "overlay/ItemFetcher.h"
#include "overlay/Floodgate.h"
#include <vector>
#include <thread>
#include "generated/StellarXDR.h"
#include "overlay/OverlayManager.h"
#include "util/Timer.h"

namespace medida
{
class Meter;
class Counter;
}

/*
Maintain the set of peers we are connected to
*/
namespace stellar
{

class OverlayManagerImpl : public OverlayManager
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
    medida::Meter& mConnectionsRejected;
    medida::Counter& mPeersSize;

    void tick();
    VirtualTimer mTimer;

    void storePeerList(std::vector<std::string> const& list, int rank);
    void storeConfigPeers();
    bool isPeerPreferred(Peer::pointer peer);

    friend class OverlayManagerTests;

  public:
    Floodgate mFloodGate;

    OverlayManagerImpl(Application& app);
    ~OverlayManagerImpl();

    void ledgerClosed(uint32_t lastClosedledgerSeq) override;
    void recvFloodedMsg(StellarMessage const& msg, Peer::pointer peer) override;
    void broadcastMessage(StellarMessage const& msg,
                          bool force = false) override;

    void connectTo(std::string const& addr) override;
    virtual void connectTo(PeerRecord& pr) override;

    void addConnectedPeer(Peer::pointer peer) override;
    void dropPeer(Peer::pointer peer) override;
    bool isPeerAccepted(Peer::pointer peer) override;
    std::vector<Peer::pointer>& getPeers() override;

    // returns NULL if the passed peer isn't found
    Peer::pointer getNextPeer(Peer::pointer peer) override;
    Peer::pointer getConnectedPeer(std::string const& ip,
                                   unsigned short port) override;

    void connectToMorePeers(int max);
    Peer::pointer getRandomPeer() override;
};
}
