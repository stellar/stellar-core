// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "main/ApplicationImpl.h"
#include "main/Config.h"

#include "database/Database.h"
#include "lib/catch.hpp"
#include "overlay/FlowControl.h"
#include "overlay/FlowControlCapacity.h"
#include "overlay/OverlayManager.h"
#include "overlay/OverlayManagerImpl.h"
#include "overlay/TxAdverts.h"
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
    PeerStub(Application& app, PeerBareAddress const& address)
        : Peer(app, WE_CALLED_REMOTE)
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
    sendMessage(xdr::msg_ptr&& xdrBytes) override
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

        rowset<row> rs = app->getDatabase().getRawSession().prepare
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

        rowset<row> rs = app->getDatabase().getRawSession().prepare
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
