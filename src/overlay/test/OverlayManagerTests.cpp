// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "main/ApplicationImpl.h"
#include "main/Config.h"

#include "database/Database.h"
#include "overlay/FlowControl.h"
#include "overlay/FlowControlCapacity.h"
#include "overlay/OverlayManager.h"
#include "overlay/OverlayManagerImpl.h"
#include "overlay/TxAdverts.h"
#include "test/Catch2.h"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/TransactionFrame.h"
#include "util/Timer.h"

#include <soci.h>

using namespace stellar;
using namespace std;
using namespace soci;
using namespace txtest;

namespace stellar
{

class PeerStub : public Peer
{
  public:
    int mSent = 0;
    PeerStub(Application& app, PeerBareAddress const& address,
             Peer::PeerRole role = WE_CALLED_REMOTE)
        : Peer(app, role)
    {
        mPeerID = SecretKey::pseudoRandomForTesting().getPublicKey();
        mState = GOT_AUTH;
        mAddress = address;
        mRemoteOverlayVersion = app.getConfig().OVERLAY_PROTOCOL_VERSION;
    }
    virtual void
    drop(std::string const&, DropDirection) override
    {
    }
    virtual void
    sendMessage(xdr::msg_ptr&& xdrBytes, ConstStellarMessagePtr msgPtr) override
    {
    }
    virtual void
    sendMessage(std::shared_ptr<StellarMessage const> msg,
                bool log = true) override
    {
        mSent += static_cast<int>(OverlayManager::isFloodMessage(*msg));
    }
    virtual void
    scheduleRead() override
    {
    }

    void
    setPeerID(NodeID const& nodeID)
    {
        mPeerID = nodeID;
    }

    void
    setMutualQsetPeer(bool isMutualQsetPeer)
    {
        mIsMutualQsetPeer = isMutualQsetPeer;
    }

    void
    setPullMode()
    {
        auto weakSelf = std::weak_ptr<Peer>(shared_from_this());
        mTxAdverts->start(
            [weakSelf](std::shared_ptr<StellarMessage const> msg) {
                auto self = weakSelf.lock();
                if (self)
                {
                    self->sendMessage(msg);
                }
            });
    }
};

class OverlayManagerStub : public OverlayManagerImpl
{
  public:
    std::map<PeerBareAddress, NodeID> mPeerIDs;
    std::set<PeerBareAddress> mMutualQsetAddresses;
    std::vector<PeerBareAddress> mConnectionAttempts;

    OverlayManagerStub(Application& app) : OverlayManagerImpl(app)
    {
    }

    virtual bool
    connectToImpl(PeerBareAddress const& address, bool) override
    {
        mConnectionAttempts.push_back(address);
        if (getConnectedPeer(address))
        {
            return false;
        }

        getPeerManager().update(address, PeerManager::BackOffUpdate::INCREASE);

        auto peerStub = std::make_shared<PeerStub>(mApp, address);
        auto idIt = mPeerIDs.find(address);
        if (idIt != std::end(mPeerIDs))
        {
            peerStub->setPeerID(idIt->second);
        }
        auto isDirectQset = isDirectQsetPeer(peerStub->getPeerID());
        peerStub->setMutualQsetPeer(isDirectQset &&
                                    mMutualQsetAddresses.find(address) !=
                                        std::end(mMutualQsetAddresses));
        if (!isDirectQset)
        {
            recordProbedNonQsetAddress(address);
        }
        peerStub->setPullMode();
        REQUIRE(addOutboundConnection(peerStub));
        return acceptAuthenticatedPeer(peerStub);
    }

    Peer::pointer
    authenticatePeer(PeerBareAddress const& address, Peer::PeerRole role,
                     bool isMutualQsetPeer = false,
                     NodeID const* nodeID = nullptr)
    {
        auto peerStub = std::make_shared<PeerStub>(mApp, address, role);
        if (nodeID)
        {
            peerStub->setPeerID(*nodeID);
        }
        peerStub->setMutualQsetPeer(isMutualQsetPeer);
        if (role == Peer::WE_CALLED_REMOTE)
        {
            REQUIRE(addOutboundConnection(peerStub));
        }
        else
        {
            maybeAddInboundConnection(peerStub);
        }
        REQUIRE(acceptAuthenticatedPeer(peerStub));
        return peerStub;
    }

    // Like authenticatePeer, but reports rejection instead of asserting
    bool
    tryAuthenticatePeer(PeerBareAddress const& address, Peer::PeerRole role,
                        bool isMutualQsetPeer = false)
    {
        auto peerStub = std::make_shared<PeerStub>(mApp, address, role);
        peerStub->setMutualQsetPeer(isMutualQsetPeer);
        if (role == Peer::WE_CALLED_REMOTE)
        {
            REQUIRE(addOutboundConnection(peerStub));
        }
        else
        {
            maybeAddInboundConnection(peerStub);
        }
        return acceptAuthenticatedPeer(peerStub);
    }

    void
    addConfigPreferredAddressForTesting(PeerBareAddress const& address)
    {
        mConfigurationPreferredPeers.insert(address);
    }

    void
    addDirectQsetPeerForTesting(NodeID const& nodeID)
    {
        mDirectQsetPeers.insert(nodeID);
    }

    void
    setPeerIDForAddress(PeerBareAddress const& address, NodeID const& nodeID)
    {
        mPeerIDs[address] = nodeID;
    }

    void
    setMutualQsetAddress(PeerBareAddress const& address)
    {
        mMutualQsetAddresses.insert(address);
    }

    bool
    wasProbedNonQset(PeerBareAddress const& address) const
    {
        return mProbedNonQset.find(address) != std::end(mProbedNonQset);
    }
};

class OverlayManagerTests
{
    class ApplicationStub : public TestApplication
    {
      public:
        ApplicationStub(VirtualClock& clock, Config const& cfg)
            : TestApplication(clock, cfg)
        {
        }

        virtual OverlayManagerStub&
        getOverlayManager() override
        {
            auto& overlay = ApplicationImpl::getOverlayManager();
            return static_cast<OverlayManagerStub&>(overlay);
        }

      private:
        virtual std::unique_ptr<OverlayManager>
        createOverlayManager() override
        {
            return std::make_unique<OverlayManagerStub>(*this);
        }
    };

  protected:
    VirtualClock clock;
    std::shared_ptr<ApplicationStub> app;

    std::vector<string> fourPeers;
    std::vector<string> threePeers;

    OverlayManagerTests()
        : fourPeers(std::vector<string>{"127.0.0.1:2011", "127.0.0.1:2012",
                                        "127.0.0.1:2013", "127.0.0.1:2014"})
        , threePeers(std::vector<string>{"127.0.0.1:64000", "127.0.0.1:64001",
                                         "127.0.0.1:64002"})
    {
        auto cfg = getTestConfig();
        cfg.TARGET_PEER_CONNECTIONS = 5;
        cfg.KNOWN_PEERS = threePeers;
        cfg.PREFERRED_PEERS = fourPeers;
        cfg.ARTIFICIALLY_SKIP_CONNECTION_ADJUSTMENT_FOR_TESTING = true;
        app = createTestApplication<ApplicationStub>(clock, cfg);
    }

    void
    testAddPeerList(bool async = false)
    {
        OverlayManagerStub& pm = app->getOverlayManager();

        if (async)
        {
            pm.triggerPeerResolution();
            REQUIRE(pm.mResolvedPeers.valid());
            pm.mResolvedPeers.wait();

            // Start ticking to store resolved peers
            pm.tick();
        }
        else
        {
            pm.storeConfigPeers();
        }

        rowset<row> rs = app->getDatabase().getRawMiscSession().prepare
                         << "SELECT ip,port,type FROM peers ORDER BY ip, port";

        auto& ppeers = pm.mConfigurationPreferredPeers;
        size_t i = 0;
        for (auto it = rs.begin(); it != rs.end(); ++it, ++i)
        {

            PeerBareAddress pba{it->get<std::string>(0),
                                static_cast<unsigned short>(it->get<int>(1))};
            auto type = it->get<int>(2);
            if (i < fourPeers.size())
            {
                REQUIRE(fourPeers[i] == pba.toString());
                REQUIRE(ppeers.find(pba) != ppeers.end());
                REQUIRE(type == static_cast<int>(PeerType::PREFERRED));
            }
            else
            {
                REQUIRE(threePeers[i - fourPeers.size()] == pba.toString());
                REQUIRE(type == static_cast<int>(PeerType::OUTBOUND));
            }
        }
        REQUIRE(i == (threePeers.size() + fourPeers.size()));
    }

    void
    testAddPeerListUpdateType()
    {
        // This test case assumes peer was discovered prior to
        // resolution, and makes sure peer type is properly updated
        // (from INBOUND to OUTBOUND)

        OverlayManagerStub& pm = app->getOverlayManager();
        PeerBareAddress prefPba{"127.0.0.1", 2011};
        PeerBareAddress pba{"127.0.0.1", 64000};

        auto prefPr = pm.getPeerManager().load(prefPba);
        auto pr = pm.getPeerManager().load(pba);

        REQUIRE(prefPr.first.mType == static_cast<int>(PeerType::INBOUND));
        REQUIRE(pr.first.mType == static_cast<int>(PeerType::INBOUND));

        pm.triggerPeerResolution();
        REQUIRE(pm.mResolvedPeers.valid());
        pm.mResolvedPeers.wait();
        pm.tick();

        rowset<row> rs = app->getDatabase().getRawMiscSession().prepare
                         << "SELECT ip,port,type FROM peers ORDER BY ip, port";

        int found = 0;
        for (auto it = rs.begin(); it != rs.end(); ++it)
        {
            PeerBareAddress storedPba{
                it->get<std::string>(0),
                static_cast<unsigned short>(it->get<int>(1))};
            auto type = it->get<int>(2);
            if (storedPba == pba)
            {
                ++found;
                REQUIRE(type == static_cast<int>(PeerType::OUTBOUND));
            }
            else if (storedPba == prefPba)
            {
                ++found;
                REQUIRE(type == static_cast<int>(PeerType::PREFERRED));
            }
        }
        REQUIRE(found == 2);
    }

    std::vector<int>
    sentCounts(OverlayManagerImpl& pm)
    {
        auto getSent = [](Peer::pointer p) {
            auto peer = static_pointer_cast<PeerStub>(p);
            return peer->mSent;
        };
        std::vector<int> result;
        for (auto p : pm.mInboundPeers.mAuthenticated)
            result.push_back(getSent(p.second));
        for (auto p : pm.mOutboundPeers.mAuthenticated)
            result.push_back(getSent(p.second));
        return result;
    }

    void
    crank(size_t n)
    {
        while (n != 0)
        {
            clock.crank(false);
            n--;
        }
    }

    void
    testBroadcast()
    {
        OverlayManagerStub& pm = app->getOverlayManager();

        auto fourPeersAddresses = pm.resolvePeers(fourPeers).first;
        auto threePeersAddresses = pm.resolvePeers(threePeers).first;
        pm.storePeerList(fourPeersAddresses, false, true);
        pm.storePeerList(threePeersAddresses, false, true);

        // connect to peers, respecting TARGET_PEER_CONNECTIONS
        pm.tick();
        REQUIRE(pm.mInboundPeers.mAuthenticated.size() == 0);
        REQUIRE(pm.mOutboundPeers.mAuthenticated.size() == 5);
        auto a = TestAccount{*app, getAccount("a")};
        auto b = TestAccount{*app, getAccount("b")};
        auto c = TestAccount{*app, getAccount("c")};
        auto d = TestAccount{*app, getAccount("d")};

        auto AtoB = a.tx({payment(b, 10)})->toStellarMessage();
        auto i = 0;
        for (auto p : pm.mOutboundPeers.mAuthenticated)
        {
            if (i++ == 2)
            {
                pm.recvFloodedMsg(*AtoB, p.second);
            }
        }
        auto broadcastTxnMsg = [&](auto msg) {
            pm.broadcastMessage(msg, xdrSha256(msg->transaction()));
        };
        broadcastTxnMsg(AtoB);
        crank(10);
        std::vector<int> expected{1, 1, 0, 1, 1};
        REQUIRE(sentCounts(pm) == expected);
        broadcastTxnMsg(AtoB);
        crank(10);
        REQUIRE(sentCounts(pm) == expected);
        auto CtoD = c.tx({payment(d, 10)})->toStellarMessage();
        broadcastTxnMsg(CtoD);
        crank(10);
        std::vector<int> expectedFinal{2, 2, 1, 2, 2};
        REQUIRE(sentCounts(pm) == expectedFinal);
    }

    NodeID
    randomNodeID()
    {
        return SecretKey::pseudoRandomForTesting().getPublicKey();
    }

    PeerBareAddress
    localhost(unsigned short port)
    {
        return PeerBareAddress{"127.0.0.1", port};
    }

    void
    testQsetPeerAuthenticatedCapExemption()
    {
        OverlayManagerStub& pm = app->getOverlayManager();

        for (auto i = 0; i < app->getConfig().TARGET_PEER_CONNECTIONS; ++i)
        {
            pm.authenticatePeer(localhost(3000 + i), Peer::WE_CALLED_REMOTE);
        }
        REQUIRE(pm.mOutboundPeers.mAuthenticated.size() ==
                app->getConfig().TARGET_PEER_CONNECTIONS);

        pm.authenticatePeer(localhost(4000), Peer::WE_CALLED_REMOTE,
                            /* isMutualQsetPeer */ true);
        REQUIRE(pm.mOutboundPeers.mAuthenticated.size() ==
                app->getConfig().TARGET_PEER_CONNECTIONS + 1);

        for (auto i = 0; i < app->getConfig().MAX_ADDITIONAL_PEER_CONNECTIONS;
             ++i)
        {
            pm.authenticatePeer(localhost(5000 + i), Peer::REMOTE_CALLED_US);
        }
        REQUIRE(pm.mInboundPeers.mAuthenticated.size() ==
                app->getConfig().MAX_ADDITIONAL_PEER_CONNECTIONS);

        pm.authenticatePeer(localhost(6000), Peer::REMOTE_CALLED_US,
                            /* isMutualQsetPeer */ true);
        REQUIRE(pm.mInboundPeers.mAuthenticated.size() ==
                app->getConfig().MAX_ADDITIONAL_PEER_CONNECTIONS + 1);
    }

    void
    testQsetPeersDoNotConsumeOutboundSlots()
    {
        OverlayManagerStub& pm = app->getOverlayManager();
        auto target = app->getConfig().TARGET_PEER_CONNECTIONS;

        for (auto i = 0; i < target; ++i)
        {
            pm.authenticatePeer(localhost(7000 + i), Peer::WE_CALLED_REMOTE);
        }
        REQUIRE(pm.availableOutboundAuthenticatedSlots() == 0);
        REQUIRE(pm.nonPreferredAuthenticatedCount() == target);

        for (auto i = 0; i < target + 3; ++i)
        {
            pm.authenticatePeer(localhost(8000 + i), Peer::WE_CALLED_REMOTE,
                                /* isMutualQsetPeer */ true);
        }
        REQUIRE(pm.availableOutboundAuthenticatedSlots() == 0);
        REQUIRE(pm.nonPreferredAuthenticatedCount() == target);
        REQUIRE(pm.mOutboundPeers.mAuthenticated.size() ==
                static_cast<size_t>(target * 2 + 3));
    }

    void
    testQsetPeerDiscoveryProbesInboundCandidates()
    {
        OverlayManagerStub& pm = app->getOverlayManager();

        auto qsetNode = randomNodeID();
        auto qsetAddress = localhost(9000);
        auto nonQsetAddress = localhost(9001);
        pm.addDirectQsetPeerForTesting(qsetNode);
        pm.setPeerIDForAddress(qsetAddress, qsetNode);
        pm.setMutualQsetAddress(qsetAddress);
        pm.getPeerManager().ensureExists(qsetAddress);
        pm.getPeerManager().ensureExists(nonQsetAddress);

        int availablePendingSlots = 10;
        pm.connectToQsetPeers(availablePendingSlots);

        REQUIRE(pm.getAuthenticatedPeers().find(qsetNode) !=
                std::end(pm.getAuthenticatedPeers()));
        REQUIRE(pm.mOutboundPeers.mAuthenticated.size() >= 1);
        REQUIRE(availablePendingSlots < 10);

        auto attemptsAfterFirstProbe = pm.mConnectionAttempts.size();
        pm.connectToQsetPeers(availablePendingSlots);
        REQUIRE(pm.mConnectionAttempts.size() == attemptsAfterFirstProbe);
    }

    void
    testNonQsetProbesAreRecorded()
    {
        OverlayManagerStub& pm = app->getOverlayManager();

        auto missingQsetNode = randomNodeID();
        auto nonQsetAddress = localhost(9100);
        pm.addDirectQsetPeerForTesting(missingQsetNode);
        pm.getPeerManager().ensureExists(nonQsetAddress);

        int availablePendingSlots = 10;
        pm.connectToQsetPeers(availablePendingSlots);

        REQUIRE(pm.wasProbedNonQset(nonQsetAddress));

        auto attemptsAfterFirstProbe = pm.mConnectionAttempts.size();
        pm.connectToQsetPeers(availablePendingSlots);
        REQUIRE(pm.mConnectionAttempts.size() == attemptsAfterFirstProbe);
    }

    void
    testKnownNonMutualQsetPeersAreNotProbed()
    {
        OverlayManagerStub& pm = app->getOverlayManager();

        auto nonMutualQsetNode = randomNodeID();
        auto nonMutualQsetAddress = localhost(9200);
        pm.addDirectQsetPeerForTesting(nonMutualQsetNode);
        pm.getQuorumPeerState().recordHandshake(
            nonMutualQsetNode, RemoteQsetRole::None, nonMutualQsetAddress,
            static_cast<uint64_t>(
                VirtualClock::to_time_t(app->getClock().system_now())));
        pm.getPeerManager().ensureExists(nonMutualQsetAddress);

        int availablePendingSlots = 10;
        pm.connectToQsetPeers(availablePendingSlots);

        REQUIRE(pm.mConnectionAttempts.empty());
        REQUIRE(availablePendingSlots == 10);
    }

    void
    testKnownAddressQsetPeersAreNotProbed()
    {
        OverlayManagerStub& pm = app->getOverlayManager();

        // A disconnected mutual qset peer with a known address is dialed
        // directly through its (preferred) peer record; random discovery
        // probing is only for peers with unknown addresses.
        auto qsetNode = randomNodeID();
        auto qsetAddress = localhost(9250);
        pm.addDirectQsetPeerForTesting(qsetNode);
        pm.getQuorumPeerState().recordHandshake(
            qsetNode, RemoteQsetRole::Direct, qsetAddress,
            static_cast<uint64_t>(
                VirtualClock::to_time_t(app->getClock().system_now())));
        pm.getPeerManager().ensureExists(localhost(9251));

        int availablePendingSlots = 10;
        pm.connectToQsetPeers(availablePendingSlots);

        REQUIRE(pm.mConnectionAttempts.empty());
        REQUIRE(availablePendingSlots == 10);
    }

    int
    storedPeerType(PeerBareAddress const& address)
    {
        auto record =
            app->getOverlayManager().getPeerManager().load(address);
        REQUIRE(record.second);
        return record.first.mType;
    }

    void
    testInboundQsetPeersDoNotConsumeInboundSlots()
    {
        OverlayManagerStub& pm = app->getOverlayManager();
        auto maxInbound = app->getConfig().MAX_ADDITIONAL_PEER_CONNECTIONS;

        // Mutual qset peers connect first, well beyond the inbound cap
        for (auto i = 0; i < maxInbound + 2; ++i)
        {
            pm.authenticatePeer(localhost(10000 + i), Peer::REMOTE_CALLED_US,
                                /* isMutualQsetPeer */ true);
        }
        REQUIRE(pm.mInboundPeers.mAuthenticated.size() ==
                static_cast<size_t>(maxInbound + 2));

        // Ordinary inbound watchers must still get their full allowance
        for (auto i = 0; i < maxInbound; ++i)
        {
            REQUIRE(pm.tryAuthenticatePeer(localhost(10100 + i),
                                           Peer::REMOTE_CALLED_US));
        }
        // ...and only the allowance
        REQUIRE(!pm.tryAuthenticatePeer(localhost(10200),
                                        Peer::REMOTE_CALLED_US));
        REQUIRE(pm.mInboundPeers.mAuthenticated.size() ==
                static_cast<size_t>(2 * maxInbound + 2));
    }

    void
    testExpirySkipsConnectedQsetPeers()
    {
        OverlayManagerStub& pm = app->getOverlayManager();

        auto connectedNode = randomNodeID();
        auto connectedAddress = localhost(9300);
        auto offlineNode = randomNodeID();
        auto offlineAddress = localhost(9301);
        pm.addDirectQsetPeerForTesting(connectedNode);
        pm.addDirectQsetPeerForTesting(offlineNode);

        pm.authenticatePeer(connectedAddress, Peer::WE_CALLED_REMOTE,
                            /* isMutualQsetPeer */ true, &connectedNode);

        // Both peers last handshaked at t=1, far beyond the TTL
        pm.getQuorumPeerState().recordHandshake(
            connectedNode, RemoteQsetRole::Direct, connectedAddress, 1);
        pm.getQuorumPeerState().recordHandshake(
            offlineNode, RemoteQsetRole::Direct, offlineAddress, 1);
        pm.getPeerManager().update(offlineAddress, PeerType::PREFERRED,
                                   /* preferredTypeKnown */ true);

        clock.setCurrentVirtualTime(
            VirtualClock::time_point(std::chrono::hours(48)));
        pm.expireStaleQuorumPeerAddresses();

        // The live connection kept its state fresh
        auto connectedInfo = pm.getQuorumPeerState().getInfo(connectedNode);
        REQUIRE(connectedInfo);
        REQUIRE(connectedInfo->remoteRole == RemoteQsetRole::Direct);
        REQUIRE(connectedInfo->address);

        // The offline peer's stale address expired and was demoted
        auto offlineInfo = pm.getQuorumPeerState().getInfo(offlineNode);
        REQUIRE(offlineInfo);
        REQUIRE(offlineInfo->remoteRole == RemoteQsetRole::Unknown);
        REQUIRE(!offlineInfo->address);
        REQUIRE(storedPeerType(offlineAddress) ==
                static_cast<int>(PeerType::OUTBOUND));
    }

    void
    testQsetHandshakeDemotions()
    {
        OverlayManagerStub& pm = app->getOverlayManager();

        auto node = randomNodeID();
        auto address1 = localhost(9500);
        auto address2 = localhost(9501);
        pm.addDirectQsetPeerForTesting(node);

        // A mutual handshake marks the address preferred
        pm.recordQsetPeerHandshake(node, RemoteQsetRole::Direct, address1);
        REQUIRE(storedPeerType(address1) ==
                static_cast<int>(PeerType::PREFERRED));

        // The peer moves: the new address becomes preferred, the old one is
        // demoted
        pm.recordQsetPeerHandshake(node, RemoteQsetRole::Direct, address2);
        REQUIRE(storedPeerType(address2) ==
                static_cast<int>(PeerType::PREFERRED));
        REQUIRE(storedPeerType(address1) ==
                static_cast<int>(PeerType::OUTBOUND));

        // The peering stops being mutual: the address is demoted so we back
        // off gracefully
        pm.recordQsetPeerHandshake(node, RemoteQsetRole::None, address2);
        REQUIRE(storedPeerType(address2) ==
                static_cast<int>(PeerType::OUTBOUND));
        auto info = pm.getQuorumPeerState().getInfo(node);
        REQUIRE(info);
        REQUIRE(info->remoteRole == RemoteQsetRole::None);

        // Operator-configured preferred addresses are never demoted
        auto configPreferred = localhost(9502);
        pm.addConfigPreferredAddressForTesting(configPreferred);
        pm.getPeerManager().update(configPreferred, PeerType::PREFERRED,
                                   /* preferredTypeKnown */ true);
        pm.recordQsetPeerHandshake(node, RemoteQsetRole::None,
                                   configPreferred);
        REQUIRE(storedPeerType(configPreferred) ==
                static_cast<int>(PeerType::PREFERRED));
    }

    void
    testReconcileDemotesRemovedQsetPeers()
    {
        OverlayManagerStub& pm = app->getOverlayManager();

        // A peer from a previous run's qset, no longer in our config
        auto removedNode = randomNodeID();
        auto removedAddress = localhost(9600);
        pm.getQuorumPeerState().recordHandshake(
            removedNode, RemoteQsetRole::Direct, removedAddress, 1);
        pm.getPeerManager().update(removedAddress, PeerType::PREFERRED,
                                   /* preferredTypeKnown */ true);
        pm.persistQuorumPeerState();

        pm.reconcileQuorumPeerState();

        REQUIRE(!pm.getQuorumPeerState().getInfo(removedNode));
        REQUIRE(storedPeerType(removedAddress) ==
                static_cast<int>(PeerType::OUTBOUND));
    }
};

TEST_CASE_METHOD(OverlayManagerTests, "storeConfigPeers() adds", "[overlay]")
{
    testAddPeerList(false);
}

TEST_CASE_METHOD(OverlayManagerTests,
                 "triggerPeerResolution() async resolution", "[overlay]")
{
    testAddPeerList(true);
}

TEST_CASE_METHOD(OverlayManagerTests, "storeConfigPeers() update type",
                 "[overlay]")
{
    testAddPeerListUpdateType();
}

TEST_CASE_METHOD(OverlayManagerTests, "broadcast() broadcasts", "[overlay]")
{
    testBroadcast();
}

TEST_CASE_METHOD(OverlayManagerTests,
                 "mutual qset peers bypass authenticated caps",
                 "[overlay][OverlayManager]")
{
    testQsetPeerAuthenticatedCapExemption();
}

TEST_CASE_METHOD(OverlayManagerTests,
                 "mutual qset peers do not consume outbound slots",
                 "[overlay][OverlayManager]")
{
    testQsetPeersDoNotConsumeOutboundSlots();
}

TEST_CASE_METHOD(OverlayManagerTests,
                 "connectToQsetPeers probes inbound candidates",
                 "[overlay][OverlayManager]")
{
    testQsetPeerDiscoveryProbesInboundCandidates();
}

TEST_CASE_METHOD(OverlayManagerTests,
                 "connectToQsetPeers records non-qset probes",
                 "[overlay][OverlayManager]")
{
    testNonQsetProbesAreRecorded();
}

TEST_CASE_METHOD(OverlayManagerTests,
                 "connectToQsetPeers skips known non-mutual qset peers",
                 "[overlay][OverlayManager]")
{
    testKnownNonMutualQsetPeersAreNotProbed();
}

TEST_CASE_METHOD(OverlayManagerTests,
                 "connectToQsetPeers skips qset peers with known addresses",
                 "[overlay][OverlayManager]")
{
    testKnownAddressQsetPeersAreNotProbed();
}

TEST_CASE_METHOD(OverlayManagerTests,
                 "inbound qset peers do not consume inbound slots",
                 "[overlay][OverlayManager]")
{
    testInboundQsetPeersDoNotConsumeInboundSlots();
}

TEST_CASE_METHOD(OverlayManagerTests,
                 "expiry skips connected qset peers",
                 "[overlay][OverlayManager]")
{
    testExpirySkipsConnectedQsetPeers();
}

TEST_CASE_METHOD(OverlayManagerTests,
                 "qset handshake promotes and demotes peer records",
                 "[overlay][OverlayManager]")
{
    testQsetHandshakeDemotions();
}

TEST_CASE_METHOD(OverlayManagerTests,
                 "reconcile demotes peers removed from the qset",
                 "[overlay][OverlayManager]")
{
    testReconcileDemotesRemovedQsetPeers();
}
}
