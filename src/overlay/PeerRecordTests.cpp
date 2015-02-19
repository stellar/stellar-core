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
    Application app{ clock, getTestConfig() };
    PeerRecord pr;
    REQUIRE(PeerRecord::fromIPPort("1.25.50.200:256", 15, clock, pr));
    pr.mNumFailures = 2;

    SECTION("fromIPPort and toXdr")
    {
        REQUIRE(pr.mIP == "1.25.50.200");
        REQUIRE(pr.mPort == 256);
        REQUIRE(pr.mRank == 2);

        PeerAddress xdr;
        REQUIRE(pr.toXdr(xdr));
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
        pr.storePeerRecord(app.getDatabase());
        PeerRecord actual;
        pr.loadPeerRecord(app.getDatabase(), pr.mIP, pr.mPort, actual);
        REQUIRE(actual == pr);
    }
    
}


}