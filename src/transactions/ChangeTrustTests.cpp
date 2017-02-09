// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/catch.hpp"
#include "lib/json/json.h"
#include "main/Application.h"
#include "overlay/LoopbackPeer.h"
#include "test/TestAccount.h"
#include "test/TestExceptions.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "util/Logging.h"
#include "util/make_unique.h"

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;

TEST_CASE("change trust", "[tx][changetrust]")
{
    Config const& cfg = getTestConfig();

    VirtualClock clock;
    ApplicationEditableVersion app{clock, cfg};
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
        REQUIRE_THROWS_AS(root.changeTrust(idrCur, 0),
                          ex_CHANGE_TRUST_INVALID_LIMIT);

        // create a trustline with a limit of 100
        root.changeTrust(idrCur, 100);

        // fill it to 90
        gateway.pay(root, idrCur, 90);

        // can't lower the limit below balance
        REQUIRE_THROWS_AS(root.changeTrust(idrCur, 89),
                          ex_CHANGE_TRUST_INVALID_LIMIT);

        // can't delete if there is a balance
        REQUIRE_THROWS_AS(root.changeTrust(idrCur, 0),
                          ex_CHANGE_TRUST_INVALID_LIMIT);

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
            REQUIRE_THROWS_AS(root.changeTrust(usdCur, 100),
                              ex_CHANGE_TRUST_NO_ISSUER);
        }
        SECTION("edit existing")
        {
            root.changeTrust(idrCur, 100);
            // Merge gateway back into root (the trustline still exists)
            gateway.merge(root);

            REQUIRE_THROWS_AS(root.changeTrust(idrCur, 99),
                              ex_CHANGE_TRUST_NO_ISSUER);
        }
    }
    SECTION("trusting self")
    {
        SECTION("protocol version 2")
        {
            app.getLedgerManager().setCurrentLedgerVersion(2);

            auto idrCur = makeAsset(gateway, "IDR");
            auto loadTrustLine = [&]() {
                return TrustFrame::loadTrustLine(gateway.getPublicKey(), idrCur,
                                                 db);
            };
            auto validateTrustLineIsConst = [&]() {
                auto trustLine = loadTrustLine();
                REQUIRE(trustLine);
                REQUIRE(trustLine->getBalance() == INT64_MAX);
            };

            validateTrustLineIsConst();

            // create a trustline with a limit of INT64_MAX - 1 wil lfail
            REQUIRE_THROWS_AS(gateway.changeTrust(idrCur, INT64_MAX - 1),
                              ex_CHANGE_TRUST_INVALID_LIMIT);
            validateTrustLineIsConst();

            // create a trustline with a limit of INT64_MAX
            gateway.changeTrust(idrCur, INT64_MAX);
            validateTrustLineIsConst();

            auto gatewayAccountBefore = loadAccount(gateway, app);
            gateway.pay(gateway, idrCur, 50);
            validateTrustLineIsConst();
            auto gatewayAccountAfter = loadAccount(gateway, app);
            REQUIRE(gatewayAccountAfter->getBalance() ==
                    (gatewayAccountBefore->getBalance() -
                     app.getLedgerManager().getTxFee()));

            // lower the limit will fail, because it is still INT64_MAX
            REQUIRE_THROWS_AS(gateway.changeTrust(idrCur, 50),
                              ex_CHANGE_TRUST_INVALID_LIMIT);
            validateTrustLineIsConst();

            // delete the trust line will fail
            REQUIRE_THROWS_AS(gateway.changeTrust(idrCur, 0),
                              ex_CHANGE_TRUST_INVALID_LIMIT);
            validateTrustLineIsConst();
        }
        SECTION("protocol version 3")
        {
            app.getLedgerManager().setCurrentLedgerVersion(3);

            auto idrCur = makeAsset(gateway, "IDR");
            auto loadTrustLine = [&]() {
                return TrustFrame::loadTrustLine(gateway.getPublicKey(), idrCur,
                                                 db);
            };
            auto validateTrustLineIsConst = [&]() {
                auto trustLine = loadTrustLine();
                REQUIRE(trustLine);
                REQUIRE(trustLine->getBalance() == INT64_MAX);
            };

            validateTrustLineIsConst();

            REQUIRE_THROWS_AS(gateway.changeTrust(idrCur, INT64_MAX - 1),
                              ex_CHANGE_TRUST_SELF_NOT_ALLOWED);
            validateTrustLineIsConst();

            REQUIRE_THROWS_AS(gateway.changeTrust(idrCur, INT64_MAX),
                              ex_CHANGE_TRUST_SELF_NOT_ALLOWED);
            validateTrustLineIsConst();

            auto gatewayAccountBefore = loadAccount(gateway, app);
            gateway.pay(gateway, idrCur, 50);
            validateTrustLineIsConst();
            auto gatewayAccountAfter = loadAccount(gateway, app);
            REQUIRE(gatewayAccountAfter->getBalance() ==
                    (gatewayAccountBefore->getBalance() -
                     app.getLedgerManager().getTxFee()));

            // lower the limit will fail, because it is still INT64_MAX
            REQUIRE_THROWS_AS(gateway.changeTrust(idrCur, 50),
                              ex_CHANGE_TRUST_SELF_NOT_ALLOWED);
            validateTrustLineIsConst();

            // delete the trust line will fail
            REQUIRE_THROWS_AS(gateway.changeTrust(idrCur, 0),
                              ex_CHANGE_TRUST_SELF_NOT_ALLOWED);
            validateTrustLineIsConst();
        }
    }
}