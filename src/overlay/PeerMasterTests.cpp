#include "util/asio.h"
#include <cassert>
#include "main/Application.h"
#include "main/Config.h"

#include <cassert>
#include "util/make_unique.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "overlay/PeerMaster.h"
#include "util/Timer.h"
#include "database/Database.h"
#include "util/TmpDir.h"
#include <soci.h>
#include "TCPPeer.h"

using namespace stellar;
using namespace std;
using namespace soci;

namespace stellar 
{

class PeerStub : public Peer
{
public:
    int sent = 0;

    PeerStub(Application &app) : Peer(app, ACCEPTOR)
    {
        mState = GOT_HELLO;
    }
    virtual void drop() override {}
    virtual string getIP() override 
    {
        return "127.0.0.1";
    }
    virtual void sendMessage(xdr::msg_ptr&& xdrBytes) override
    {
        sent++;
    }


};

class PeerMasterStub : public PeerMaster
{
public:
    PeerMasterStub(Application &app) : PeerMaster(app) {}

    virtual void connectTo(PeerRecord &pr) override
    {
        if (!getConnectedPeer(pr.mIP, pr.mPort))
        {
            pr.backOff(mApp.getClock());
            pr.storePeerRecord(mApp.getDatabase());

            addConnectedPeer(Peer::pointer(new PeerStub(mApp)));
        }
    }

};


class PeerMasterTests
{
protected:
    VirtualClock clock;
    Application::pointer appPtr = Application::create(clock, getTestConfig());
    Application &app = *appPtr;

    vector<string> fourPeers;
    vector<string> threePeers;

    PeerMasterTests() :
        fourPeers(vector<string> { "127.0.0.1:2011", "127.0.0.1:2012", "127.0.0.1:2013", "127.0.0.1:2014" }),
        threePeers(vector<string> { "127.0.0.1:201", "127.0.0.1:202", "127.0.0.1:203", "127.0.0.1:204" })
    {
    }

    void test_addPeerList()
    {
        PeerMaster peerMaster{ app };

        peerMaster.storePeerList(fourPeers, 10);
        peerMaster.storePeerList(threePeers, 3);

        rowset<row> rs = app.getDatabase().getSession().prepare << "SELECT ip,port from Peers order by rank limit 5 ";
        vector<string> actual;
        for (auto it = rs.begin(); it != rs.end(); ++it)
            actual.push_back(it->get<string>(0) + ":" + to_string(it->get<int>(1)));

        vector<string> expected = fourPeers;
        expected.push_back(threePeers.front());
        REQUIRE(actual == expected);
    }

    vector<int> sentCounts(PeerMaster &pm)
    {
        vector<int> result;
        for (auto p : pm.mPeers)
            result.push_back(dynamic_pointer_cast<PeerStub>(p)->sent);
        return result;
    }

    void test_broadcast()
    {
        PeerMasterStub pm{ app };

        pm.storePeerList(fourPeers, 3);
        pm.storePeerList(threePeers, 2);
        pm.connectToMorePeers(5);
        REQUIRE(pm.mPeers.size() == 5);
        StellarMessage msg;
        pm.recvFloodedMsg(msg, pm.mPeers.front());
        pm.broadcastMessage(msg);
        vector<int> expected{ 0,1,1,1,1 };
        REQUIRE(sentCounts(pm) == expected);
        pm.broadcastMessage(msg);
        REQUIRE(sentCounts(pm) == expected);
    }
};

TEST_CASE_METHOD(PeerMasterTests, "addPeerList() adds", "[peer]") 
{
    test_addPeerList();
}

TEST_CASE_METHOD(PeerMasterTests, "broadcast() broadcasts", "[peer]")
{
    test_broadcast();
}

}