// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include <cassert>
#include "main/ApplicationImpl.h"
#include "main/Config.h"

#include <cassert>
#include "util/make_unique.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "overlay/OverlayManager.h"
#include "overlay/OverlayManagerImpl.h"
#include "util/Timer.h"
#include "database/Database.h"
#include "util/TmpDir.h"
#include <soci.h>
#include "TCPPeer.h"
#include "transactions/TxTests.h"

using namespace stellar;
using namespace std;
using namespace soci;
using namespace txtest;

namespace stellar
{

class PeerStub : public Peer
{
  public:
    int sent = 0;

    PeerStub(Application& app) : Peer(app, ACCEPTOR)
    {
        mState = GOT_HELLO;
    }
    virtual void
    drop() override
    {
    }
    virtual string
    getIP() override
    {
        return "127.0.0.1";
    }
    virtual void
    sendMessage(xdr::msg_ptr&& xdrBytes) override
    {
        sent++;
    }
};

class OverlayManagerStub : public OverlayManagerImpl
{
  public:
    OverlayManagerStub(Application& app) : OverlayManagerImpl(app)
    {
    }

    virtual void
    connectTo(PeerRecord& pr) override
    {
        if (!getConnectedPeer(pr.mIP, pr.mPort))
        {
            pr.backOff(mApp.getClock());
            pr.storePeerRecord(mApp.getDatabase());

            addConnectedPeer(Peer::pointer(new PeerStub(mApp)));
        }
    }
};

class OverlayManagerTests
{
    class ApplicationStub : public ApplicationImpl
    {
      public:
        shared_ptr<OverlayManagerStub> mOverlayManager;
        ApplicationStub(VirtualClock& clock, Config const& cfg)
            : ApplicationImpl(clock, cfg)
        {
            mOverlayManager = make_shared<OverlayManagerStub>(*this);
        }
        virtual OverlayManagerStub&
        getOverlayManager() override
        {
            return *mOverlayManager;
        }
    };

  protected:
    VirtualClock clock;
    ApplicationStub app{clock, getTestConfig()};

    vector<string> fourPeers;
    vector<string> threePeers;

    OverlayManagerTests()
        : fourPeers(vector<string>{"127.0.0.1:2011", "127.0.0.1:2012",
                                   "127.0.0.1:2013", "127.0.0.1:2014"})
        , threePeers(vector<string>{"127.0.0.1:201", "127.0.0.1:202",
                                    "127.0.0.1:203", "127.0.0.1:204"})
    {
    }

    void
    test_addPeerList()
    {
        OverlayManagerStub& pm = app.getOverlayManager();

        pm.storePeerList(fourPeers, 10);
        pm.storePeerList(threePeers, 3);

        rowset<row> rs = app.getDatabase().getSession().prepare
                         << "SELECT ip,port from Peers order by rank limit 5 ";
        vector<string> actual;
        for (auto it = rs.begin(); it != rs.end(); ++it)
            actual.push_back(it->get<string>(0) + ":" +
                             to_string(it->get<int>(1)));

        vector<string> expected = fourPeers;
        expected.push_back(threePeers.front());
        REQUIRE(actual == expected);
    }

    vector<int>
    sentCounts(OverlayManagerImpl& pm)
    {
        vector<int> result;
        for (auto p : pm.mPeers)
            result.push_back(dynamic_pointer_cast<PeerStub>(p)->sent);
        return result;
    }

    void
    test_broadcast()
    {
        OverlayManagerStub& pm = app.getOverlayManager();

        pm.storePeerList(fourPeers, 3);
        pm.storePeerList(threePeers, 2);
        pm.connectToMorePeers(5);
        REQUIRE(pm.mPeers.size() == 5);
        SecretKey a = getAccount("a");
        SecretKey b = getAccount("b");
        SecretKey c = getAccount("c");
        SecretKey d = getAccount("d");

        StellarMessage AtoC = createPaymentTx(a, b, 1, 10)->toStellarMessage();
        pm.recvFloodedMsg(AtoC, *(pm.mPeers.begin() + 2));
        pm.broadcastMessage(AtoC);
        vector<int> expected{1, 1, 0, 1, 1};
        REQUIRE(sentCounts(pm) == expected);
        pm.broadcastMessage(AtoC);
        REQUIRE(sentCounts(pm) == expected);
        StellarMessage CtoD = createPaymentTx(c, d, 1, 10)->toStellarMessage();
        pm.broadcastMessage(CtoD);
        vector<int> expectedFinal{2, 2, 1, 2, 2};
        REQUIRE(sentCounts(pm) == expectedFinal);
    }
};

TEST_CASE_METHOD(OverlayManagerTests, "addPeerList() adds", "[overlay]")
{
    test_addPeerList();
}

TEST_CASE_METHOD(OverlayManagerTests, "broadcast() broadcasts", "[overlay]")
{
    test_broadcast();
}
}