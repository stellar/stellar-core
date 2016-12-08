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
#include "test/TestAccount.h"
#include "test/TestExceptions.h"
#include "test/TxTests.h"

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;

TEST_CASE("change trust", "[tx][changetrust]")
{
    Config const& cfg = getTestConfig();

    VirtualClock clock;
    Application::pointer appPtr = Application::create(clock, cfg);
    Application& app = *appPtr;
    Database& db = app.getDatabase();

    app.start();

    // set up world
    auto root = TestAccount::createRoot(app);
    auto const minBalance2 = app.getLedgerManager().getMinBalance(2);
    auto gateway = root.create("gw", minBalance2);
    Asset idrCur = makeAsset(gateway, "IDR");

    SECTION("basic tests")
    {
        // create a trustline with a limit of 0
        REQUIRE_THROWS_AS(root.changeTrust(idrCur, 0), ex_CHANGE_TRUST_INVALID_LIMIT);

        // create a trustline with a limit of 100
        root.changeTrust(idrCur, 100);

        // fill it to 90
        gateway.pay(root, idrCur, 90);

        // can't lower the limit below balance
        REQUIRE_THROWS_AS(root.changeTrust(idrCur, 89), ex_CHANGE_TRUST_INVALID_LIMIT);

        // can't delete if there is a balance
        REQUIRE_THROWS_AS(root.changeTrust(idrCur, 0), ex_CHANGE_TRUST_INVALID_LIMIT);

        // lower the limit at the balance
        root.changeTrust(idrCur, 90);

        // clear the balance
        root.pay(gateway, idrCur, 90);

        // delete the trust line
        root.changeTrust(idrCur, 0);
        REQUIRE(!(TrustFrame::loadTrustLine(root.getPublicKey(), idrCur, db)));
    }
    SECTION("issuer does not exist")
    {
        SECTION("new trust line")
        {
            Asset usdCur = makeAsset(getAccount("non-existing"), "IDR");
            REQUIRE_THROWS_AS(root.changeTrust(usdCur, 100), ex_CHANGE_TRUST_NO_ISSUER);
        }
        SECTION("edit existing")
        {
            root.changeTrust(idrCur, 100);
            // Merge gateway back into root (the trustline still exists)
            gateway.merge(root);

            REQUIRE_THROWS_AS(root.changeTrust(idrCur, 99), ex_CHANGE_TRUST_NO_ISSUER);
        }
    }
}
