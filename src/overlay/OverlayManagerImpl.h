#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "Peer.h"
#include "PeerAuth.h"
#include "PeerDoor.h"
#include "PeerManager.h"
#include "herder/TxSetFrame.h"
#include "overlay/Floodgate.h"
#include "overlay/ItemFetcher.h"
#include "overlay/OverlayManager.h"
#include "overlay/OverlayMetrics.h"
#include "overlay/StellarXDR.h"
#include "overlay/SurveyManager.h"
#include "util/Logging.h"
#include "util/Timer.h"

#include "medida/metrics_registry.h"
#include "util/RandomEvictionCache.h"

#include <future>
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
    std::set<PeerBareAddress> mConfigurationPreferredPeers;

    struct PeersList
    {
        explicit PeersList(OverlayManagerImpl& overlayManager,
                           medida::MetricsRegistry& metricsRegistry,
                           std::string const& directionString,
                           std::string const& cancelledName,
                           int maxAuthenticatedCount);

        medida::Meter& mConnectionsAttempted;
        medida::Meter& mConnectionsEstablished;
        medida::Meter& mConnectionsDropped;
        medida::Meter& mConnectionsCancelled;

        OverlayManagerImpl& mOverlayManager;
        std::string mDirectionString;
        int mMaxAuthenticatedCount;

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

    PeersList& getPeersList(Peer* peer);

    PeerManager mPeerManager;
    PeerDoor mDoor;
    PeerAuth mAuth;
    bool mShuttingDown;

    OverlayMetrics mOverlayMetrics;

    // NOTE: bool is used here as a placeholder, since no ValueType is needed.
    RandomEvictionCache<uint64_t, bool> mMessageCache;

    void tick();
    VirtualTimer mTimer;
    VirtualTimer mPeerIPTimer;

    friend class OverlayManagerTests;

    Floodgate mFloodGate;

    std::shared_ptr<SurveyManager> mSurveyManager;

  public:
    OverlayManagerImpl(Application& app);
    ~OverlayManagerImpl();

    void clearLedgersBelow(uint32_t ledgerSeq, uint32_t lclSeq) override;
    bool recvFloodedMsgID(StellarMessage const& msg, Peer::pointer peer,
                          Hash& msgID) override;
    void forgetFloodedMsg(Hash const& msgID) override;
    bool broadcastMessage(StellarMessage const& msg,
                          bool force = false) override;
    void connectTo(PeerBareAddress const& address) override;

    void addInboundConnection(Peer::pointer peer) override;
    bool addOutboundConnection(Peer::pointer peer) override;
    void removePeer(Peer* peer) override;
    void storeConfigPeers();
    void purgeDeadPeers();

    bool acceptAuthenticatedPeer(Peer::pointer peer) override;
    bool isPreferred(Peer* peer) const override;
    std::vector<Peer::pointer> const& getInboundPendingPeers() const override;
    std::vector<Peer::pointer> const& getOutboundPendingPeers() const override;
    std::vector<Peer::pointer> getPendingPeers() const override;
    int getPendingPeersCount() const override;
    std::map<NodeID, Peer::pointer> const&
    getInboundAuthenticatedPeers() const override;
    std::map<NodeID, Peer::pointer> const&
    getOutboundAuthenticatedPeers() const override;
    std::map<NodeID, Peer::pointer> getAuthenticatedPeers() const override;
    int getAuthenticatedPeersCount() const override;

    // returns nullptr if the passed peer isn't found
    Peer::pointer getConnectedPeer(PeerBareAddress const& address) override;

    std::vector<Peer::pointer> getRandomAuthenticatedPeers() override;
    std::vector<Peer::pointer> getRandomInboundAuthenticatedPeers() override;
    std::vector<Peer::pointer> getRandomOutboundAuthenticatedPeers() override;

    std::set<Peer::pointer> getPeersKnows(Hash const& h) override;

    OverlayMetrics& getOverlayMetrics() override;
    PeerAuth& getPeerAuth() override;

    PeerManager& getPeerManager() override;

    SurveyManager& getSurveyManager() override;

    void start() override;
    void shutdown() override;

    bool isShuttingDown() const override;

    void recordMessageMetric(StellarMessage const& stellarMsg,
                             Peer::pointer peer) override;

    void updateFloodRecord(StellarMessage const& oldMsg,
                           StellarMessage const& newMsg) override;

  private:
    struct ResolvedPeers
    {
        std::vector<PeerBareAddress> known;
        std::vector<PeerBareAddress> preferred;
    };

    std::map<PeerType, std::unique_ptr<RandomPeerSource>> mPeerSources;
    std::future<ResolvedPeers> mResolvedPeers;

    void triggerPeerResolution();
    std::vector<PeerBareAddress>
    resolvePeers(std::vector<std::string> const& peers);
    void storePeerList(std::vector<PeerBareAddress> const& addresses,
                       bool setPreferred, bool startup);

    virtual bool connectToImpl(PeerBareAddress const& address,
                               bool forceoutbound);
    int connectTo(int maxNum, PeerType peerType);
    int connectTo(std::vector<PeerBareAddress> const& peers,
                  bool forceoutbound);
    std::vector<PeerBareAddress> getPeersToConnectTo(int maxNum,
                                                     PeerType peerType);

    bool moveToAuthenticated(Peer::pointer peer);

    int availableOutboundPendingSlots() const;
    int availableOutboundAuthenticatedSlots() const;
    int nonPreferredAuthenticatedCount() const;

    bool isPossiblyPreferred(std::string const& ip);

    void updateSizeCounters();

    void extractPeersFromMap(std::map<NodeID, Peer::pointer> const& peerMap,
                             std::vector<Peer::pointer>& result);
    void shufflePeerList(std::vector<Peer::pointer>& peerList);
};
}
