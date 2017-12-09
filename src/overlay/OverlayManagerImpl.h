#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "LoadManager.h"
#include "Peer.h"
#include "PeerAuth.h"
#include "PeerDoor.h"
#include "PeerRecord.h"
#include "herder/TxSetFrame.h"
#include "overlay/Floodgate.h"
#include "overlay/ItemFetcher.h"
#include "overlay/OverlayManager.h"
#include "overlay/StellarXDR.h"
#include "util/Timer.h"
#include <set>
#include <vector>

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
    std::set<std::string> mPreferredPeers;

    // pending peers - connected, but not authenticated
    std::vector<Peer::pointer> mPendingPeers;
    // authenticated and connected peers
    std::map<NodeID, Peer::pointer> mAuthenticatedPeers;
    PeerDoor mDoor;
    PeerAuth mAuth;
    LoadManager mLoad;
    bool mShuttingDown;

    medida::Meter& mMessagesReceived;
    medida::Meter& mMessagesBroadcast;
    medida::Meter& mConnectionsAttempted;
    medida::Meter& mConnectionsEstablished;
    medida::Meter& mConnectionsDropped;
    medida::Meter& mConnectionsRejected;
    medida::Counter& mPendingPeersSize;
    medida::Counter& mAuthenticatedPeersSize;

    void tick();
    VirtualTimer mTimer;

    void storePeerList(std::vector<std::string> const& list, bool resetBackOff,
                       bool preferred);
    void storeConfigPeers();

    friend class OverlayManagerTests;

    Floodgate mFloodGate;

  public:
    OverlayManagerImpl(Application& app);
    ~OverlayManagerImpl();

    void ledgerClosed(uint32_t lastClosedledgerSeq) override;
    void recvFloodedMsg(StellarMessage const& msg, Peer::pointer peer) override;
    void broadcastMessage(StellarMessage const& msg,
                          bool force = false) override;
    void connectTo(std::string const& addr) override;
    virtual void connectTo(PeerRecord& pr) override;

    void addPendingPeer(Peer::pointer peer) override;
    void dropPeer(Peer* peer) override;
    bool acceptAuthenticatedPeer(Peer::pointer peer) override;
    bool isPreferred(Peer* peer) override;
    std::vector<Peer::pointer> const& getPendingPeers() const override;
    size_t getPendingPeersCount() const override;
    std::map<NodeID, Peer::pointer> const&
    getAuthenticatedPeers() const override;
    size_t getAuthenticatedPeersCount() const override;

    // returns NULL if the passed peer isn't found
    Peer::pointer getConnectedPeer(std::string const& ip,
                                   unsigned short port) override;

    void connectToMorePeers(int max);
    std::vector<Peer::pointer> getRandomAuthenticatedPeers() override;

    std::set<Peer::pointer> getPeersKnows(Hash const& h) override;

    PeerAuth& getPeerAuth() override;

    LoadManager& getLoadManager() override;

    void start() override;
    void shutdown() override;

    bool isShuttingDown() const override;

  private:
    void orderByPreferredPeers(vector<PeerRecord>& peers);
    bool moveToAuthenticated(Peer::pointer peer);
    void updateSizeCounters();
};
}
