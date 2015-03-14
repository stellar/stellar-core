// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "PeerRecord.h"
#include <soci.h>
#include "database/Database.h"
#include "lib/catch.hpp"
#include "main/Config.h"
#include "generated/StellarXDR.h"
#include "main/Application.h"
#include "main/test.h"

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
        REQUIRE(pr.mRank == 1);

        PeerAddress xdr;
        pr.toXdr(xdr);
        REQUIRE(xdr.port == 256);
        REQUIRE(xdr.ip[0] == 1);
        REQUIRE(xdr.ip[1] == 25);
        REQUIRE(xdr.ip[2] == 50);
        REQUIRE(xdr.ip[3] == 200);
        REQUIRE(xdr.numFailures == 2);
    }
    SECTION("loadPeerRecord and storePeerRecord")
    {
        pr.mNextAttempt = pr.mNextAttempt + chrono::seconds(12);
        pr.storePeerRecord(app->getDatabase());
        PeerRecord other;
        PeerRecord::fromIPPort("1.2.3.4", 15, clock, other);
        other.storePeerRecord(app->getDatabase());

        pr.mNextAttempt = pr.mNextAttempt + chrono::seconds(12);
        pr.storePeerRecord(app->getDatabase());
        auto actual1 = PeerRecord::loadPeerRecord(app->getDatabase(), pr.mIP, pr.mPort);
        REQUIRE(*actual1 == pr);

        auto actual2 = PeerRecord::loadPeerRecord(app->getDatabase(), "1.2.3.4", 15);
        REQUIRE(*actual2 == other);
    }
    
}


}