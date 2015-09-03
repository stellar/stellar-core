// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "main/Application.h"
#include "overlay/LoopbackPeer.h"
#include "util/make_unique.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "lib/json/json.h"
#include "TxTests.h"

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;

TEST_CASE("change trust", "[tx][changeTrust]")
{
    Config const& cfg = getTestConfig();

    VirtualClock clock;
    Application::pointer appPtr = Application::create(clock, cfg);
    Application& app = *appPtr;

    app.start();

    // set up world
    SecretKey root = getRoot(app.getNetworkID());
    SecretKey a1 = getAccount("A");

    SequenceNumber rootSeq = getAccountSeqNum(root, app) + 1;

    SECTION("issuer does not exist")
    {
        applyChangeTrust(app, root, a1, rootSeq, "USD", 100,
                         CHANGE_TRUST_NO_ISSUER);
    }
}
