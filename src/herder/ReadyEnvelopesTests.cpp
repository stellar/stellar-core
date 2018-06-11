// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/ReadyEnvelopes.h"
#include "main/Application.h"
#include "test/TestPrinter.h"
#include "test/TestUtils.h"
#include "test/test.h"

#include <lib/catch.hpp>

using namespace stellar;

namespace
{

SCPEnvelope
makeEnvelope(uint64_t slotIndex)
{
    auto envelope = SCPEnvelope{};
    envelope.statement.slotIndex = slotIndex;
    return envelope;
};
}

TEST_CASE("ReadyEnvelopes", "[herder][unit][ReadyEnvelopes]")
{
    Config cfg{getTestConfig()};
    VirtualClock clock;
    ApplicationImpl app{clock, cfg};

    SECTION("empty")
    {
        ReadyEnvelopes readyEnvelopes{app};
        REQUIRE(!readyEnvelopes.seen(makeEnvelope(1)));
        REQUIRE(!readyEnvelopes.seen(makeEnvelope(2)));
        REQUIRE(!readyEnvelopes.seen(makeEnvelope(3)));
        REQUIRE(readyEnvelopes.readySlots() == std::vector<uint64_t>{});

        SCPEnvelope ret;
        REQUIRE(!readyEnvelopes.pop(0, ret));
        REQUIRE(!readyEnvelopes.pop(1, ret));
        REQUIRE(!readyEnvelopes.pop(2, ret));
        REQUIRE(!readyEnvelopes.pop(3, ret));
    }

    SECTION("add few items and then pop them")
    {
        ReadyEnvelopes readyEnvelopes{app};
        REQUIRE(readyEnvelopes.push(makeEnvelope(1)));
        REQUIRE(readyEnvelopes.push(makeEnvelope(2)));
        REQUIRE(readyEnvelopes.push(makeEnvelope(3)));
        REQUIRE(!readyEnvelopes.push(makeEnvelope(1)));
        REQUIRE(readyEnvelopes.seen(makeEnvelope(1)));
        REQUIRE(readyEnvelopes.seen(makeEnvelope(2)));
        REQUIRE(readyEnvelopes.seen(makeEnvelope(3)));
        auto slots = std::vector<uint64_t>{1, 2, 3};
        REQUIRE(readyEnvelopes.readySlots() == slots);

        SCPEnvelope ret;
        REQUIRE(!readyEnvelopes.pop(0, ret));
        REQUIRE(readyEnvelopes.pop(1, ret));
        REQUIRE(ret == makeEnvelope(1));
        REQUIRE(readyEnvelopes.pop(2, ret));
        REQUIRE(ret == makeEnvelope(2));
        REQUIRE(readyEnvelopes.pop(3, ret));
        REQUIRE(ret == makeEnvelope(3));

        REQUIRE(readyEnvelopes.seen(makeEnvelope(1)));
        REQUIRE(readyEnvelopes.seen(makeEnvelope(2)));
        REQUIRE(readyEnvelopes.seen(makeEnvelope(3)));
    }

    SECTION("add few items out of order and then pop them in order")
    {
        ReadyEnvelopes readyEnvelopes{app};
        REQUIRE(readyEnvelopes.push(makeEnvelope(3)));
        REQUIRE(readyEnvelopes.push(makeEnvelope(2)));
        REQUIRE(readyEnvelopes.push(makeEnvelope(1)));

        SCPEnvelope ret;
        REQUIRE(readyEnvelopes.pop(3, ret));
        REQUIRE(ret == makeEnvelope(1));
        REQUIRE(readyEnvelopes.pop(3, ret));
        REQUIRE(ret == makeEnvelope(2));
        REQUIRE(readyEnvelopes.pop(3, ret));
        REQUIRE(ret == makeEnvelope(3));
    }

    SECTION("forget and dont allow old items")
    {
        ReadyEnvelopes readyEnvelopes{app};
        REQUIRE(readyEnvelopes.push(makeEnvelope(1)));
        REQUIRE(readyEnvelopes.push(makeEnvelope(3)));
        REQUIRE(readyEnvelopes.push(makeEnvelope(4)));
        REQUIRE(readyEnvelopes.push(makeEnvelope(5)));
        readyEnvelopes.setMinimumSlotIndex(3);

        REQUIRE(readyEnvelopes.seen(makeEnvelope(1)));
        REQUIRE(readyEnvelopes.seen(makeEnvelope(2)));
        REQUIRE(readyEnvelopes.seen(makeEnvelope(3)));
        REQUIRE(readyEnvelopes.seen(makeEnvelope(4)));
        REQUIRE(readyEnvelopes.seen(makeEnvelope(5)));
        REQUIRE(!readyEnvelopes.push(makeEnvelope(1)));
        REQUIRE(!readyEnvelopes.push(makeEnvelope(2)));

        SCPEnvelope ret;
        REQUIRE(!readyEnvelopes.pop(2, ret));
        REQUIRE(readyEnvelopes.pop(5, ret));
        REQUIRE(ret == makeEnvelope(3));
        REQUIRE(readyEnvelopes.pop(5, ret));
        REQUIRE(ret == makeEnvelope(4));
        REQUIRE(readyEnvelopes.pop(5, ret));
        REQUIRE(ret == makeEnvelope(5));
    }
}
