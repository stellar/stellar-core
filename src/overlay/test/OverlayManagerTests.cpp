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

#include <atomic>
#include <future>
#include <limits>
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
    std::atomic<int> mReadsScheduled{0};
    using Peer::recvAuthenticatedMessage;

    PeerStub(Application& app, PeerBareAddress const& address)
        : Peer(app, WE_CALLED_REMOTE)
    {
        mPeerID = SecretKey::pseudoRandomForTesting().getPublicKey();
        mState = GOT_AUTH;
        mAddress = address;
        mRemoteOverlayVersion = app.getConfig().OVERLAY_PROTOCOL_VERSION;
    }

    // Rewind to the point where HELLO has been exchanged and the remote's MAC
    // key is known, so an AUTH message can be received.
    void
    prepareForAuth(HmacSha256Key const& key)
    {
        RecursiveLockGuard guard(mStateMutex);
        setState(guard, GOT_HELLO);
        REQUIRE(mHmac.setRecvMackey(key));
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
        ++mReadsScheduled;
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

namespace
{
// Run `work` on the overlay thread and wait for it without cranking the main
// thread, so message receipt stays separate from main-thread processing.
template <typename F>
auto
onOverlayThread(Application& app, F&& work)
{
    releaseAssert(app.getConfig().BACKGROUND_OVERLAY_PROCESSING);
    auto task = std::make_shared<std::packaged_task<decltype(work())()>>(
        std::forward<F>(work));
    auto result = task->get_future();
    app.postOnOverlayThread([task]() { (*task)(); }, "overlay thread test");
    REQUIRE(result.wait_for(std::chrono::seconds(5)) ==
            std::future_status::ready);
    return result.get();
}
} // namespace

TEST_CASE("handshake messages reserve peer read capacity",
          "[overlay][flowcontrol]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.BACKGROUND_OVERLAY_PROCESSING = false;
    cfg.PEER_READING_CAPACITY =
        GENERATE(2u, std::numeric_limits<uint32_t>::max());
    cfg.PEER_FLOOD_READING_CAPACITY = cfg.PEER_READING_CAPACITY - 1;
    cfg.FLOW_CONTROL_SEND_MORE_BATCH_SIZE = cfg.PEER_FLOOD_READING_CAPACITY;
    CAPTURE(cfg.PEER_READING_CAPACITY);
    auto app = createTestApplication(clock, cfg);
    auto peer =
        std::make_shared<PeerStub>(*app, PeerBareAddress{"127.0.0.1", 2011});
    auto flowControl = peer->getFlowControl();
    // Compact info captures all local and peer message/byte budgets.
    auto const initialCapacity = flowControl->getFlowControlJsonInfo(true);

    StellarMessage handshake;
    handshake.type(GENERATE(HELLO, AUTH));
    REQUIRE(flowControl->canRead());
    {
        // Follow the transport's read path: lock capacity, then throttle.
        CapacityTrackedMessage tracked(peer, handshake);
        REQUIRE(tracked.isCapacityLocked());
        REQUIRE(flowControl->getCapacity().getCapacity().mTotalCapacity == 0);
        REQUIRE(flowControl->getCapacity().getCapacity().mFloodCapacity ==
                cfg.PEER_FLOOD_READING_CAPACITY);
        REQUIRE_FALSE(flowControl->canRead());
        REQUIRE(flowControl->maybeThrottleRead());
        // No other handshake message can be admitted while one is pending.
        REQUIRE_FALSE(
            flowControl->getCapacity().canLockLocalCapacity(handshake));
        REQUIRE(peer->mReadsScheduled.load() == 0);
    }

    // Releasing the message restores all capacity and resumes the throttled
    // read loop exactly once.
    testutil::crankSome(clock);
    REQUIRE(peer->mReadsScheduled.load() == 1);
    REQUIRE_FALSE(flowControl->isThrottled());
    REQUIRE(flowControl->canRead());
    REQUIRE(flowControl->getFlowControlJsonInfo(true) == initialCapacity);

    // Ordinary messages still reserve exactly one message slot.
    StellarMessage normal;
    normal.type(GET_SCP_STATE);
    {
        CapacityTrackedMessage tracked(peer, normal);
        REQUIRE(tracked.isCapacityLocked());
        REQUIRE(flowControl->getCapacity().getCapacity().mTotalCapacity ==
                cfg.PEER_READING_CAPACITY - 1);
    }
    REQUIRE(flowControl->getFlowControlJsonInfo(true) == initialCapacity);
}

TEST_CASE("background reads pause until main thread processes AUTH",
          "[overlay][connections]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.BACKGROUND_OVERLAY_PROCESSING = true;
    auto app = createTestApplication(clock, cfg);
    auto peer =
        std::make_shared<PeerStub>(*app, PeerBareAddress{"127.0.0.1", 2011});

    HmacSha256Key key;
    key.key[0] = 1;
    peer->prepareForAuth(key);
    REQUIRE(app->getOverlayManager().addOutboundConnection(peer));
    Hmac senderHmac;
    REQUIRE(senderHmac.setSendMackey(key));

    StellarMessage auth;
    auth.type(AUTH);
    auth.auth().flags = AUTH_MSG_FLAG_FLOW_CONTROL_BYTES_REQUESTED;
    AuthenticatedMessage authenticated;
    senderHmac.setAuthenticatedMessageBody(authenticated, auth);

    // Receive AUTH the way the transport does: on the overlay thread, followed
    // by the read-throttling check.
    REQUIRE(onOverlayThread(
        *app, [peer, authenticated = std::move(authenticated)]() mutable {
            auto valid =
                peer->recvAuthenticatedMessage(std::move(authenticated));
            if (valid)
            {
                peer->getFlowControl()->maybeThrottleRead();
            }
            return valid;
        }));
    REQUIRE_FALSE(peer->isAuthenticatedForTesting());
    REQUIRE(peer->getFlowControl()->isThrottled());
    REQUIRE_FALSE(peer->getFlowControl()->canRead());
    REQUIRE(peer->mReadsScheduled.load() == 0);

    // The transport may read again only once the main thread has processed
    // AUTH, at which point the peer is authenticated.
    testutil::crankUntil(
        app, [&]() { return peer->mReadsScheduled.load() == 1; },
        std::chrono::seconds(5));
    REQUIRE(peer->isAuthenticatedForTesting());
    REQUIRE_FALSE(peer->getFlowControl()->isThrottled());
    REQUIRE(peer->getFlowControl()->canRead());
}

class OverlayManagerStub : public OverlayManagerImpl
{
  public:
    OverlayManagerStub(Application& app) : OverlayManagerImpl(app)
    {
    }

    virtual bool
    connectToImpl(PeerBareAddress const& address, bool) override
    {
        if (getConnectedPeer(address))
        {
            return false;
        }

        getPeerManager().update(address, PeerManager::BackOffUpdate::INCREASE);

        auto peerStub = std::make_shared<PeerStub>(mApp, address);
        peerStub->setPullMode();
        REQUIRE(addOutboundConnection(peerStub));
        return acceptAuthenticatedPeer(peerStub);
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
}
