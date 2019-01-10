#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "LoadManager.h"
#include "Peer.h"
#include "PeerAuth.h"
#include "PeerDoor.h"
#include "PeerManager.h"
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
    std::set<PeerBareAddress> mPreferredPeers;

    struct PeersList
    {
        explicit PeersList(OverlayManagerImpl& overlayManager,
                           unsigned short maxAuthenticatedCount);

        OverlayManagerImpl& mOverlayManager;
        unsigned short mMaxAuthenticatedCount;

        std::vector<Peer::pointer> mPending;
        std::map<NodeID, Peer::pointer> mAuthenticated;

        Peer::pointer byAddress(PeerBareAddress const& address) const;
        void removePeer(Peer* peer);
        bool moveToAuthenticated(Peer::pointer peer);
        bool acceptAuthenticatedPeer(Peer::pointer peer);
        void shutdown();
    };

    PeersList mInboundPeers;
    PeersList mOutboundPeers;

    template <typename PeerT>
    PeersList&
    getPeersList(PeerT peer)
    {
        switch (peer->getRole())
        {
        case Peer::WE_CALLED_REMOTE:
            return mOutboundPeers;
        case Peer::REMOTE_CALLED_US:
            return mInboundPeers;
        default:
            abort();
        }
    }

    PeerDoor mDoor;
    PeerAuth mAuth;
    LoadManager mLoad;
    PeerManager mPeerManager;
    bool mShuttingDown;

    medida::Meter& mMessagesBroadcast;
    medida::Meter& mConnectionsAttempted;
    medida::Meter& mConnectionsEstablished;
    medida::Meter& mConnectionsDropped;
    medida::Meter& mConnectionsRejected;
    medida::Counter& mPendingPeersSize;
    medida::Counter& mAuthenticatedPeersSize;

    void tick();
    VirtualTimer mTimer;

    void storePeerList(std::vector<std::string> const& list, bool setPreferred);
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

    void addInboundConnection(Peer::pointer peer) override;
    void addOutboundConnection(Peer::pointer peer) override;
    void removePeer(Peer* peer) override;

    bool acceptAuthenticatedPeer(Peer::pointer peer) override;
    bool isPreferred(Peer* peer) override;
    std::vector<Peer::pointer> getPendingPeers() const override;
    int getPendingPeersCount() const override;
    std::map<NodeID, Peer::pointer> getAuthenticatedPeers() const override;
    int getAuthenticatedPeersCount() const override;

    // returns nullptr if the passed peer isn't found
    Peer::pointer getConnectedPeer(PeerBareAddress const& address) override;

    std::vector<Peer::pointer> getRandomAuthenticatedPeers() override;

    std::set<Peer::pointer> getPeersKnows(Hash const& h) override;

    PeerAuth& getPeerAuth() override;

    LoadManager& getLoadManager() override;
    PeerManager& getPeerManager() override;

    void start() override;
    void shutdown() override;

    bool isShuttingDown() const override;

  private:
    std::vector<PeerBareAddress> getPreferredPeersFromConfig();
    std::vector<PeerBareAddress> getPeersToConnectTo(int maxNum, bool outbound);
    virtual void connectToImpl(PeerBareAddress const& address,
                               bool forceoutbound);
    void connectTo(std::vector<PeerBareAddress> const& peers,
                   bool forceoutbound);

    void orderByPreferredPeers(std::vector<PeerBareAddress>& peers);
    bool moveToAuthenticated(Peer::pointer peer);

    int missingAuthenticatedCount() const;
    void connectToOutboundPeers();
    void connectToNotOutboundPeers();

    void updateSizeCounters();
};
}
