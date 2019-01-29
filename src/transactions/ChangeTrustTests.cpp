// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/catch.hpp"
#include "lib/json/json.h"
#include "main/Application.h"
#include "test/TestAccount.h"
#include "test/TestExceptions.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "util/Logging.h"
#include "util/make_unique.h"

using namespace stellar;
using namespace stellar::txtest;

TEST_CASE("change trust", "[tx][changetrust]")
{
    Config const& cfg = getTestConfig();

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);
    Database& db = app->getDatabase();

    app->start();

    // set up world
    auto root = TestAccount::createRoot(*app);
    auto const minBalance2 = app->getLedgerManager().getMinBalance(2);
    auto gateway = root.create("gw", minBalance2);
    Asset idr = makeAsset(gateway, "IDR");

    SECTION("disabled CHANGE_TRUST check")
    {
        for_all_versions(*app, [&] {
            // create a trustline with a limit of 0
            REQUIRE_THROWS_AS(root.changeTrust(idr, 100),
                              ex_CHANGE_TRUST_MALFORMED);
        });
    }

    SECTION("disabled ALLOW_TRUST check")
    {
        for_all_versions(*app, [&] {
            // create a trustline with a limit of 0
            REQUIRE_THROWS_AS(root.allowTrust(idr, gateway),
                              ex_ALLOW_TRUST_MALFORMED);
        });
    }
}
