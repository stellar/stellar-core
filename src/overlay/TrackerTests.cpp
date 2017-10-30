// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/ByteSlice.h"
#include "crypto/SHA.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "overlay/Tracker.h"
#include "test/TestUtils.h"
#include "test/test.h"

namespace stellar
{

namespace
{

SCPEnvelope
makeEnvelope(int slotIndex)
{
    auto result = SCPEnvelope{};
    result.statement.slotIndex = slotIndex;
    result.statement.pledges.type(SCP_ST_CONFIRM);
    result.statement.pledges.confirm().nPrepared = slotIndex;
    return result;
}
}

TEST_CASE("Tracker works", "[overlay][Tracker]")
{
    VirtualClock clock;
    auto const& cfg = getTestConfig();
    auto app = createTestApplication(clock, cfg);

    auto hash = sha256(ByteSlice{"hash"});
    auto nullAskPeer = AskPeer{[](Peer::pointer, Hash) {}};

    SECTION("empty tracker")
    {
        Tracker t{*app, hash, nullAskPeer};
        REQUIRE(t.size() == 0);
        REQUIRE(t.empty());
        REQUIRE(t.getLastSeenSlotIndex() == 0);
    }

    SECTION("can listen on envelope")
    {
        Tracker t{*app, hash, nullAskPeer};
        auto env1 = makeEnvelope(1);
        t.listen(env1);

        REQUIRE(t.size() == 1);
        REQUIRE(!t.empty());
        REQUIRE(t.getLastSeenSlotIndex() == 1);
        REQUIRE(env1 == t.pop());
        REQUIRE(t.size() == 0);
        REQUIRE(t.empty());
        REQUIRE(t.getLastSeenSlotIndex() == 1);
        t.resetLastSeenSlotIndex();
        REQUIRE(t.getLastSeenSlotIndex() == 0);
    }

    SECTION("can listen twice on the same envelope")
    {
        Tracker t{*app, hash, nullAskPeer};
        auto env1 = makeEnvelope(1);
        t.listen(env1);
        t.listen(env1);
        REQUIRE(t.getLastSeenSlotIndex() == 1);

        REQUIRE(env1 == t.pop());
        REQUIRE(env1 == t.pop());
    }

    SECTION("can listen on different envelopes")
    {
        Tracker t{*app, hash, nullAskPeer};
        auto env1 = makeEnvelope(1);
        auto env2 = makeEnvelope(2);
        t.listen(env1);
        REQUIRE(t.getLastSeenSlotIndex() == 1);
        t.listen(env2);
        REQUIRE(t.getLastSeenSlotIndex() == 2);

        REQUIRE(env2 == t.pop());
        REQUIRE(env1 == t.pop());
    }

    SECTION("properly removes old envelopes")
    {
        Tracker t{*app, hash, nullAskPeer};
        auto env1 = makeEnvelope(1);
        auto env2 = makeEnvelope(2);
        auto env3 = makeEnvelope(3);
        auto env4 = makeEnvelope(4);
        auto env5 = makeEnvelope(5);
        t.listen(env5);
        t.listen(env3);
        t.listen(env1);
        t.listen(env2);
        t.listen(env4);

        REQUIRE(t.size() == 5);
        REQUIRE(t.getLastSeenSlotIndex() == 5);

        SECTION("properly removes some old envelopes")
        {
            REQUIRE(t.clearEnvelopesBelow(4));
            REQUIRE(t.size() == 2);
            REQUIRE(env4 == t.pop());
            REQUIRE(env5 == t.pop());
        }

        SECTION("properly removes all old envelopes")
        {
            REQUIRE(!t.clearEnvelopesBelow(6));
            REQUIRE(t.empty());
        }
    }
}
}
