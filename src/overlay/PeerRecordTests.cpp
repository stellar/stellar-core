// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "PeerRecord.h"
#include "database/Database.h"
#include "lib/catch.hpp"
#include "main/Config.h"
#include "overlay/StellarXDR.h"
#include "main/Application.h"
#include "main/test.h"
#include "util/SociNoWarnings.h"

namespace stellar
{
using namespace std;

TEST_CASE("toXdr", "[overlay][PeerRecord]")
{
    VirtualClock clock;
    Application::pointer app = Application::create(clock, getTestConfig());
    PeerRecord pr;
    PeerRecord::parseIPPort("1.25.50.200:256", *app, pr);
    pr.mNumFailures = 2;

    SECTION("fromIPPort and toXdr")
    {
        REQUIRE(pr.mIP == "1.25.50.200");
        REQUIRE(pr.mPort == 256);

        PeerAddress xdr;
        pr.toXdr(xdr);
        REQUIRE(xdr.port == 256);
        REQUIRE(xdr.ip.ipv4()[0] == 1);
        REQUIRE(xdr.ip.ipv4()[1] == 25);
        REQUIRE(xdr.ip.ipv4()[2] == 50);
        REQUIRE(xdr.ip.ipv4()[3] == 200);
        REQUIRE(xdr.numFailures == 2);
    }
    SECTION("loadPeerRecord and storePeerRecord")
    {
        pr.mNextAttempt = pr.mNextAttempt + chrono::seconds(12);
        REQUIRE(pr.insertIfNew(app->getDatabase()));

        {
            // second time should return false and not modify it
            PeerRecord pr2(pr);
            pr2.mNumFailures++;
            REQUIRE(!pr2.insertIfNew(app->getDatabase()));

            auto actualPR = PeerRecord::loadPeerRecord(app->getDatabase(),
                                                       pr.mIP, pr.mPort);
            REQUIRE(*actualPR == pr);
        }

        PeerRecord other("1.2.3.4", 15, clock.now());
        other.storePeerRecord(app->getDatabase());

        pr.mNextAttempt = pr.mNextAttempt + chrono::seconds(12);
        pr.storePeerRecord(app->getDatabase());
        auto actual1 =
            PeerRecord::loadPeerRecord(app->getDatabase(), pr.mIP, pr.mPort);
        REQUIRE(*actual1 == pr);

        auto actual2 =
            PeerRecord::loadPeerRecord(app->getDatabase(), "1.2.3.4", 15);
        REQUIRE(*actual2 == other);
    }
}

TEST_CASE("private addresses", "[overlay][PeerRecord]")
{
    VirtualClock clock;
    PeerRecord pr("1.2.3.4", 15, clock.now());
    CHECK(!pr.isPrivateAddress());
    pr = PeerRecord("10.1.2.3", 15, clock.now());
    CHECK(pr.isPrivateAddress());
    pr = PeerRecord("172.17.1.2", 15, clock.now());
    CHECK(pr.isPrivateAddress());
    pr = PeerRecord("192.168.1.2", 15, clock.now());
    CHECK(pr.isPrivateAddress());
}
}
