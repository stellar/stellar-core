#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/TxSetFrame.h"
#include "overlay/Floodgate.h"
#include "overlay/OverlayManager.h"
#include "overlay/QSetCache.h"
#include "overlay/StellarXDR.h"
#include "overlay/TxSetCache.h"
#include "transport/LoadManager.h"
#include "transport/Peer.h"
#include "transport/PeerDoor.h"
#include "transport/PeerRecord.h"
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

    // pending peers - connected, but not authenticated
    std::vector<Peer::pointer> mPendingPeers;
    // authenticated and connected peers
    std::map<NodeID, Peer::pointer> mAuthenticatedPeers;
    PeerDoor mDoor;
    LoadManager mLoad;
    QSetCache mQSetCache;
    TxSetCache mTxSetCache;
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
    void connectTo(PeerRecord& pr) override;
    void connectTo(PeerBareAddress const& address) override;

    void addPendingPeer(Peer::pointer peer) override;
    void dropPeer(Peer* peer) override;
    bool acceptAuthenticatedPeer(Peer::pointer peer) override;
    std::vector<Peer::pointer> const& getPendingPeers() const override;
    int getPendingPeersCount() const override;
    std::map<NodeID, Peer::pointer> const&
    getAuthenticatedPeers() const override;
    int getAuthenticatedPeersCount() const override;

    // returns nullptr if the passed peer isn't found
    Peer::pointer getConnectedPeer(PeerBareAddress const& address) override;

    void connectToMorePeers(vector<PeerRecord>& peers);
    std::vector<Peer::pointer> getRandomAuthenticatedPeers() override;

    LoadManager& getLoadManager() override;
    QSetCache& getQSetCache() override;
    TxSetCache& getTxSetCache() override;

    void start() override;
    void shutdown() override;

    bool isShuttingDown() const override;

  private:
    std::vector<PeerRecord> getPreferredPeersFromConfig();
    std::vector<PeerRecord> getPeersToConnectTo(int maxNum);

    bool moveToAuthenticated(Peer::pointer peer);
    void updateSizeCounters();
};
}
