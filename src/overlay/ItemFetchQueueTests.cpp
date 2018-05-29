// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SHA.h"
#include "main/ApplicationImpl.h"
#include "overlay/ItemFetchQueue.h"
#include "test/test.h"

#include <lib/catch.hpp>

using namespace stellar;

TEST_CASE("ItemFetchQueue fetches", "[overlay][unit][ItemFetchQueue]")
{
    Config cfg(getTestConfig());
    VirtualClock clock;
    ApplicationImpl app{clock, cfg};
    app.initialize();

    auto h1 = ItemKey{ItemType::TX_SET, sha256(ByteSlice("h1"))};
    auto h2 = ItemKey{ItemType::TX_SET, sha256(ByteSlice("h2"))};
    auto h3 = ItemKey{ItemType::TX_SET, sha256(ByteSlice("h3"))};
    auto h4 = ItemKey{ItemType::TX_SET, sha256(ByteSlice("h4"))};

    std::vector<Hash> received;

    SECTION("empty")
    {
        ItemFetchQueue itemFetchQueue(app);

        REQUIRE(!itemFetchQueue.isFetching(h1));
        REQUIRE(!itemFetchQueue.isFetching(h2));
        REQUIRE(!itemFetchQueue.isFetching(h3));
        REQUIRE(!itemFetchQueue.isFetching(h4));
    }

    SECTION("fetch some items")
    {
        ItemFetchQueue itemFetchQueue(app);

        itemFetchQueue.startFetch(h2);
        itemFetchQueue.startFetch(h3);
        itemFetchQueue.startFetch(h3);

        REQUIRE(!itemFetchQueue.isFetching(h1));
        REQUIRE(itemFetchQueue.isFetching(h2));
        REQUIRE(itemFetchQueue.isFetching(h3));
        REQUIRE(!itemFetchQueue.isFetching(h4));
    }

    SECTION("stop fetching some items")
    {
        ItemFetchQueue itemFetchQueue(app);

        itemFetchQueue.startFetch(h2);
        itemFetchQueue.startFetch(h3);
        itemFetchQueue.startFetch(h3);
        itemFetchQueue.stopFetch(h3);

        REQUIRE(!itemFetchQueue.isFetching(h1));
        REQUIRE(itemFetchQueue.isFetching(h2));
        REQUIRE(!itemFetchQueue.isFetching(h3));
        REQUIRE(!itemFetchQueue.isFetching(h4));
    }
}
