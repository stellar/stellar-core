// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include <main/Application.h>
#include "main/test.h"
#include "lib/catch.hpp"
#include "overlay/ItemFetcher.h"
#include "overlay/OverlayManager.h"
#include "overlay/LoopbackPeer.h"
#include <crypto/SHA.h>
#include <crypto/Hex.h>

namespace stellar
{

/* this wasn't in the VS project. So I removed IntTracker.

using IntFetcher = ItemFetcher<int, IntTracker>;

TEST_CASE("ItemFetcher fetches", "[overlay]")
{

    VirtualClock clock;
    auto app = Application::create(clock, getTestConfig(0));

    IntFetcher itemFetcher(*app, 2);

    std::string received;

    auto cb = [&](int const &item)
    {
        received += std::to_string(item) + " ";
    };

    Hash zero = sha256(ByteSlice("zero"));
    Hash ten = sha256(ByteSlice("ten"));
    Hash twelve = sha256(ByteSlice("twelve"));
    Hash fourteen = sha256(ByteSlice("fourteen"));

    auto tTen = itemFetcher.fetch(ten, cb);
    auto tTwelve = itemFetcher.fetch(twelve, cb);
    itemFetcher.fetch(twelve, cb); // tracker held by tTwelve


    REQUIRE(!itemFetcher.isNeeded(zero));
    REQUIRE(itemFetcher.isNeeded(ten));
    REQUIRE(itemFetcher.isNeeded(twelve));

    itemFetcher.recv(twelve, 12);
    itemFetcher.recv(ten, 10);

    REQUIRE(received == "12 12 10 ");

    REQUIRE(!itemFetcher.isNeeded(zero));
    REQUIRE(!itemFetcher.isNeeded(ten));
    REQUIRE(!itemFetcher.isNeeded(twelve));

    SECTION("stops fetching items whose tracker was released")
    {
        auto tFourteen = itemFetcher.fetch(fourteen, cb);
        tFourteen.reset(); // tracker released, won't be received

        REQUIRE(!itemFetcher.isNeeded(fourteen));

        itemFetcher.recv(fourteen, 14);
        REQUIRE(received == "12 12 10 ");
    }

    SECTION("caches")
    {
        tTen.reset();
        tTwelve.reset();
        itemFetcher.recv(fourteen, 14);
        REQUIRE(*itemFetcher.get(ten) == 10);
        REQUIRE(*itemFetcher.get(twelve) == 12);
        auto tZero = itemFetcher.fetch(zero, cb);
        itemFetcher.recv(zero, 0);
        itemFetcher.fetch(zero, cb); // from cache

        REQUIRE(!itemFetcher.get(ten));
        REQUIRE(*itemFetcher.get(twelve) == 12);
        REQUIRE(*itemFetcher.get(zero) == 0);
        REQUIRE(received == "12 12 10 0 0 ");
    }

    SECTION("asks peers in turn")
    {
        auto other = Application::create(clock, getTestConfig(1));
        LoopbackPeerConnection connection1(*app, *other);
        LoopbackPeerConnection connection2(*app, *other);
        auto peer1 = connection1.getInitiator();
        auto peer2 = connection2.getInitiator();

        IntTrackerPtr tZero;
        IntTrackerPtr tZero2;
        SECTION("fetching once works")
        {
            tZero = itemFetcher.fetch(zero, cb);
            tZero2 = tZero;
        }
        SECTION("fetching twice does not trigger any additional network
activity")
        {
            tZero = itemFetcher.fetch(zero, cb);
            tZero2 = itemFetcher.fetch(zero, cb);
        }
        REQUIRE(tZero->mAsked.size() == 1);

        while(tZero->mAsked.size() < 4)
        {
            clock.crank(true);
        }
        itemFetcher.recv(zero, 0);
        while (clock.crank(false) > 0) { }

        REQUIRE(tZero->mAsked.size() == 4);
        REQUIRE(tZero == tZero2);

        REQUIRE(std::count(tZero->mAsked.begin(), tZero->mAsked.end(), peer1) ==
2);
        REQUIRE(std::count(tZero->mAsked.begin(), tZero->mAsked.end(), peer2) ==
2);


    }
}
*/
}
