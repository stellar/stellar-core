// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/ByteSlice.h"
#include "crypto/SHA.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/test.h"
#include "overlay/Tracker.h"

namespace stellar
{

namespace
{

SCPEnvelope makeEnvelope(int slotIndex)
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
    auto app = Application::create(clock, cfg);

    auto hash = sha256(ByteSlice{"hash"});
    auto nullAskPeer = AskPeer{[](Peer::pointer, Hash){}};

    SECTION("empty tracker")
    {
        Tracker t{*app, hash, nullAskPeer};
        REQUIRE(t.size() == 0);
        REQUIRE(t.empty());
    }

    SECTION("can listen on envelope")
    {
        Tracker t{*app, hash, nullAskPeer};
        auto env0 = makeEnvelope(0);
        t.listen(env0);

        REQUIRE(t.size() == 1);
        REQUIRE(!t.empty());
        REQUIRE(env0 == t.pop());
        REQUIRE(t.size() == 0);
        REQUIRE(t.empty());
    }

    SECTION("can listen twice on the same envelope")
    {
        Tracker t{*app, hash, nullAskPeer};
        auto env0 = makeEnvelope(0);
        t.listen(env0);
        t.listen(env0);

        REQUIRE(env0 == t.pop());
        REQUIRE(env0 == t.pop());
    }

    SECTION("can listen on different envelopes")
    {
        Tracker t{*app, hash, nullAskPeer};
        auto env0 = makeEnvelope(0);
        auto env1 = makeEnvelope(1);
        t.listen(env0);
        t.listen(env1);

        REQUIRE(env1 == t.pop());
        REQUIRE(env0 == t.pop());
    }

    SECTION("properly removes old envelopes")
    {
        Tracker t{*app, hash, nullAskPeer};
        auto env0 = makeEnvelope(0);
        auto env1 = makeEnvelope(1);
        auto env2 = makeEnvelope(2);
        auto env3 = makeEnvelope(3);
        auto env4 = makeEnvelope(4);
        t.listen(env4);
        t.listen(env2);
        t.listen(env0);
        t.listen(env1);
        t.listen(env3);

        REQUIRE(t.size() == 5);
        REQUIRE(t.clearEnvelopesBelow(3));
        REQUIRE(t.size() == 2);
        REQUIRE(env3 == t.pop());
        REQUIRE(env4 == t.pop());

        t.listen(env4);
        t.listen(env2);
        t.listen(env0);
        t.listen(env1);
        t.listen(env3);
        REQUIRE(t.size() == 5);
        REQUIRE(!t.clearEnvelopesBelow(6));
        REQUIRE(t.empty());

    }
}

}
