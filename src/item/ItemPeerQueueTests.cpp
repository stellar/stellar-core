// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "item/ItemPeerQueue.h"
#include "main/ApplicationImpl.h"
// #include "overlay/OverlayManagerImpl.h"
#include "test/test.h"

#include <lib/catch.hpp>

using namespace stellar;

namespace
{

class PeerStub : public Peer
{
  public:
    PeerStub(Application& app) : Peer(app, REMOTE_CALLED_US)
    {
    }

  private:
    void
    sendMessage(xdr::msg_ptr&&) override
    {
    }
    PeerBareAddress
    makeAddress(int) const override
    {
    }
    AuthCert
    getAuthCert() override
    {
    }
    void
    drop(bool) override
    {
    }
};
}

TEST_CASE("ItemPeerQueue", "[overlay][unit][ItemPeerQueue]")
{
    Config cfg(getTestConfig());
    VirtualClock clock;
    ApplicationImpl app{clock, cfg};

    auto makePeer = [&app]() { return std::make_shared<PeerStub>(app); };

    SECTION("empty")
    {
        ItemPeerQueue queue;

        REQUIRE(queue.getLastPopped() == nullptr);
        REQUIRE(queue.pop() == nullptr);
        REQUIRE(queue.getLastPopped() == nullptr);

        queue.addKnowing(makePeer());
        REQUIRE(queue.pop() == nullptr);
        REQUIRE(queue.getLastPopped() == nullptr);
    }

    SECTION("setPeers counter")
    {
        ItemPeerQueue queue;

        REQUIRE(queue.getNumPeersSet() == 0);
        queue.setPeers({});
        REQUIRE(queue.getNumPeersSet() == 1);
        queue.setPeers({});
        REQUIRE(queue.getNumPeersSet() == 2);
    }

    SECTION("pops in reverse order when none is knowing")
    {
        ItemPeerQueue queue;
        auto peer1 = makePeer();
        auto peer2 = makePeer();
        auto peer3 = makePeer();

        queue.setPeers({peer1, peer2, peer3});
        REQUIRE(queue.pop() == peer3);
        REQUIRE(queue.getLastPopped() == peer3);
        REQUIRE(queue.pop() == peer2);
        REQUIRE(queue.getLastPopped() == peer2);
        REQUIRE(queue.pop() == peer1);
        REQUIRE(queue.getLastPopped() == peer1);
        REQUIRE(queue.pop() == nullptr);
        REQUIRE(queue.getLastPopped() == peer1);
    }

    SECTION("pops knowing first, still in reverse order")
    {
        ItemPeerQueue queue;
        auto peer1 = makePeer();
        auto peer2 = makePeer();
        auto peer3 = makePeer();
        auto peer4 = makePeer();
        auto peer5 = makePeer();
        auto peer6 = makePeer();

        queue.addKnowing(peer1);
        queue.addKnowing(peer2);
        queue.addKnowing(peer3);
        queue.setPeers({peer1, peer2, peer3, peer4, peer5, peer6});
        queue.removeKnowing(peer1);
        queue.removeKnowing(peer2);
        queue.removeKnowing(peer3);

        REQUIRE(queue.pop() == peer3);
        REQUIRE(queue.getLastPopped() == peer3);
        REQUIRE(queue.pop() == peer2);
        REQUIRE(queue.getLastPopped() == peer2);
        REQUIRE(queue.pop() == peer1);
        REQUIRE(queue.getLastPopped() == peer1);
        REQUIRE(queue.pop() == peer6);
        REQUIRE(queue.getLastPopped() == peer6);
        REQUIRE(queue.pop() == peer5);
        REQUIRE(queue.getLastPopped() == peer5);
        REQUIRE(queue.pop() == peer4);
        REQUIRE(queue.getLastPopped() == peer4);
        REQUIRE(queue.pop() == nullptr);
        REQUIRE(queue.getLastPopped() == peer4);
    }

    SECTION("ignores knowing set after setPeers")
    {
        ItemPeerQueue queue;
        auto peer1 = makePeer();
        auto peer2 = makePeer();
        auto peer3 = makePeer();
        auto peer4 = makePeer();
        auto peer5 = makePeer();
        auto peer6 = makePeer();

        queue.setPeers({peer1, peer2, peer3, peer4, peer5, peer6});
        queue.addKnowing(peer1);
        queue.addKnowing(peer2);
        queue.addKnowing(peer3);

        REQUIRE(queue.pop() == peer6);
        REQUIRE(queue.getLastPopped() == peer6);
        REQUIRE(queue.pop() == peer5);
        REQUIRE(queue.getLastPopped() == peer5);
        REQUIRE(queue.pop() == peer4);
        REQUIRE(queue.getLastPopped() == peer4);
        REQUIRE(queue.pop() == peer3);
        REQUIRE(queue.getLastPopped() == peer3);
        REQUIRE(queue.pop() == peer2);
        REQUIRE(queue.getLastPopped() == peer2);
        REQUIRE(queue.pop() == peer1);
        REQUIRE(queue.getLastPopped() == peer1);
        REQUIRE(queue.pop() == nullptr);
        REQUIRE(queue.getLastPopped() == peer1);

        queue.setPeers({peer1, peer2, peer3, peer4, peer5, peer6});

        REQUIRE(queue.pop() == peer3);
        REQUIRE(queue.getLastPopped() == peer3);
        REQUIRE(queue.pop() == peer2);
        REQUIRE(queue.getLastPopped() == peer2);
        REQUIRE(queue.pop() == peer1);
        REQUIRE(queue.getLastPopped() == peer1);
        REQUIRE(queue.pop() == peer6);
        REQUIRE(queue.getLastPopped() == peer6);
        REQUIRE(queue.pop() == peer5);
        REQUIRE(queue.getLastPopped() == peer5);
        REQUIRE(queue.pop() == peer4);
        REQUIRE(queue.getLastPopped() == peer4);
        REQUIRE(queue.pop() == nullptr);
        REQUIRE(queue.getLastPopped() == peer4);
    }
}
