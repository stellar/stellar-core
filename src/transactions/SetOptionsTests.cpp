// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "main/Application.h"
#include "overlay/LoopbackPeer.h"
#include "util/make_unique.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "crypto/Base58.h"
#include "lib/json/json.h"
#include "TxTests.h"

using namespace stellar;
using namespace stellar::txtest;


typedef std::unique_ptr<Application> appPtr;

// Try setting each option to make sure it works
// try setting all at once
// try setting high threshold ones without the correct sigs
TEST_CASE("set options", "[tx]")
{
    Config cfg;
    cfg.RUN_STANDALONE = true;
    cfg.START_NEW_NETWORK = true;
    cfg.DESIRED_BASE_FEE = 10;

    VirtualClock clock;
    Application app(clock, cfg);

    // set up world
    SecretKey root = getRoot();
    
    // set InflationDest
    // set flags
    // set transfer rate
    // set data
    // set thresholds
    // set signer

    // these are all tested by other tests
}
