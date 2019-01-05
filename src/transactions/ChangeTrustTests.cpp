// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "ledger/TrustLineWrapper.h"
#include "lib/catch.hpp"
#include "lib/json/json.h"
#include "main/Application.h"
#include "test/TestAccount.h"
#include "test/TestExceptions.h"
#include "test/TestMarket.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/TransactionUtils.h"
#include "util/Logging.h"

using namespace stellar;
using namespace stellar::txtest;

TEST_CASE("change trust", "[tx][changetrust]")
{
    Config const& cfg = getTestConfig();

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);

    app->start();

    // set up world
    auto root = TestAccount::createRoot(*app);
    auto const minBalance2 = app->getLedgerManager().getLastMinBalance(2);
    auto gateway = root.create("gw", minBalance2);
    Asset idr = makeAsset(gateway, "IDR");

    SECTION("basic tests")
    {
        for_all_versions(*app, [&] {
            // create a trustline with a limit of 0
            REQUIRE_THROWS_AS(root.changeTrust(idr, 0),
                              ex_CHANGE_TRUST_INVALID_LIMIT);

            // create a trustline with a limit of 100
            root.changeTrust(idr, 100);

            // fill it to 90
            gateway.pay(root, idr, 90);

            // can't lower the limit below balance
            REQUIRE_THROWS_AS(root.changeTrust(idr, 89),
                              ex_CHANGE_TRUST_INVALID_LIMIT);

            // can't delete if there is a balance
            REQUIRE_THROWS_AS(root.changeTrust(idr, 0),
                              ex_CHANGE_TRUST_INVALID_LIMIT);

            // lower the limit at the balance
            root.changeTrust(idr, 90);

            // clear the balance
            root.pay(gateway, idr, 90);

            // delete the trust line
            root.changeTrust(idr, 0);
            REQUIRE(!root.hasTrustLine(idr));
        });
    }
    SECTION("issuer does not exist")
    {
        SECTION("new trust line")
        {
            for_all_versions(*app, [&] {
                Asset usd = makeAsset(getAccount("non-existing"), "IDR");
                REQUIRE_THROWS_AS(root.changeTrust(usd, 100),
                                  ex_CHANGE_TRUST_NO_ISSUER);
            });
        }
        SECTION("edit existing")
        {
            closeLedgerOn(*app, 2, 1, 1, 2016);
            for_all_versions(*app, [&] {
                root.changeTrust(idr, 100);
                // Merge gateway back into root (the trustline still exists)
                gateway.merge(root);

                REQUIRE_THROWS_AS(root.changeTrust(idr, 99),
                                  ex_CHANGE_TRUST_NO_ISSUER);
                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    REQUIRE(!stellar::loadAccount(ltx, gateway));
                }
            });
        }
    }
    SECTION("trusting self")
    {
        auto validateTrustLineIsConst = [&]() {
            LedgerTxn ltx(app->getLedgerTxnRoot());
            auto trustLine =
                stellar::loadTrustLine(ltx, gateway.getPublicKey(), idr);
            REQUIRE(trustLine);
            REQUIRE(trustLine.getBalance() == INT64_MAX);
        };

        auto loadAccount = [&](PublicKey const& k) {
            LedgerTxn ltx(app->getLedgerTxnRoot());
            auto le = stellar::loadAccount(ltx, k).current();
            return le.data.account();
        };

        validateTrustLineIsConst();

        for_versions_to(2, *app, [&] {
            // create a trustline with a limit of INT64_MAX - 1 wil lfail
            REQUIRE_THROWS_AS(gateway.changeTrust(idr, INT64_MAX - 1),
                              ex_CHANGE_TRUST_INVALID_LIMIT);
            validateTrustLineIsConst();

            // create a trustline with a limit of INT64_MAX
            gateway.changeTrust(idr, INT64_MAX);
            validateTrustLineIsConst();

            auto gatewayAccountBefore = loadAccount(gateway);
            gateway.pay(gateway, idr, 50);
            validateTrustLineIsConst();
            auto gatewayAccountAfter = loadAccount(gateway);
            REQUIRE(gatewayAccountAfter.balance ==
                    (gatewayAccountBefore.balance -
                     app->getLedgerManager().getLastTxFee()));

            // lower the limit will fail, because it is still INT64_MAX
            REQUIRE_THROWS_AS(gateway.changeTrust(idr, 50),
                              ex_CHANGE_TRUST_INVALID_LIMIT);
            validateTrustLineIsConst();

            // delete the trust line will fail
            REQUIRE_THROWS_AS(gateway.changeTrust(idr, 0),
                              ex_CHANGE_TRUST_INVALID_LIMIT);
            validateTrustLineIsConst();
        });

        for_versions_from(3, *app, [&] {
            REQUIRE_THROWS_AS(gateway.changeTrust(idr, INT64_MAX - 1),
                              ex_CHANGE_TRUST_SELF_NOT_ALLOWED);
            validateTrustLineIsConst();

            REQUIRE_THROWS_AS(gateway.changeTrust(idr, INT64_MAX),
                              ex_CHANGE_TRUST_SELF_NOT_ALLOWED);
            validateTrustLineIsConst();

            auto gatewayAccountBefore = loadAccount(gateway);
            gateway.pay(gateway, idr, 50);
            validateTrustLineIsConst();
            auto gatewayAccountAfter = loadAccount(gateway);
            REQUIRE(gatewayAccountAfter.balance ==
                    (gatewayAccountBefore.balance -
                     app->getLedgerManager().getLastTxFee()));

            // lower the limit will fail, because it is still INT64_MAX
            REQUIRE_THROWS_AS(gateway.changeTrust(idr, 50),
                              ex_CHANGE_TRUST_SELF_NOT_ALLOWED);
            validateTrustLineIsConst();

            // delete the trust line will fail
            REQUIRE_THROWS_AS(gateway.changeTrust(idr, 0),
                              ex_CHANGE_TRUST_SELF_NOT_ALLOWED);
            validateTrustLineIsConst();
        });
    }

    SECTION("trustline on native asset")
    {
        const auto nativeAsset = makeNativeAsset();
        for_versions_to(9, *app, [&] {
            REQUIRE_THROWS_AS(gateway.changeTrust(nativeAsset, INT64_MAX - 1),
                              ex_txINTERNAL_ERROR);
        });

        for_versions_from(10, *app, [&] {
            REQUIRE_THROWS_AS(gateway.changeTrust(nativeAsset, INT64_MAX - 1),
                              ex_CHANGE_TRUST_MALFORMED);
        });
    }

    SECTION("create trust line with native selling liabilities")
    {
        auto const minBal2 = app->getLedgerManager().getLastMinBalance(2);
        auto txfee = app->getLedgerManager().getLastTxFee();
        auto const native = makeNativeAsset();
        auto acc1 = root.create("acc1", minBal2 + 2 * txfee + 500 - 1);
        TestMarket market(*app);

        auto cur1 = acc1.asset("CUR1");
        market.requireChangesWithOffer({}, [&] {
            return market.addOffer(acc1, {native, cur1, Price{1, 1}, 500});
        });

        for_versions_to(9, *app, [&] { acc1.changeTrust(idr, 1000); });
        for_versions_from(10, *app, [&] {
            REQUIRE_THROWS_AS(acc1.changeTrust(idr, 1000),
                              ex_CHANGE_TRUST_LOW_RESERVE);
            root.pay(acc1, txfee + 1);
            acc1.changeTrust(idr, 1000);
        });
    }

    SECTION("create trust line with native buying liabilities")
    {
        auto const minBal2 = app->getLedgerManager().getLastMinBalance(2);
        auto txfee = app->getLedgerManager().getLastTxFee();
        auto const native = makeNativeAsset();
        auto acc1 = root.create("acc1", minBal2 + 2 * txfee + 500 - 1);
        TestMarket market(*app);

        auto cur1 = acc1.asset("CUR1");
        market.requireChangesWithOffer({}, [&] {
            return market.addOffer(acc1, {cur1, native, Price{1, 1}, 500});
        });

        for_all_versions(*app, [&] { acc1.changeTrust(idr, 1000); });
    }

    SECTION("cannot reduce limit below buying liabilities or delete")
    {
        for_versions_from(10, *app, [&] {
            auto txfee = app->getLedgerManager().getLastTxFee();
            auto const native = makeNativeAsset();
            auto acc1 = root.create("acc1", minBalance2 + 10 * txfee);
            TestMarket market(*app);

            acc1.changeTrust(idr, 1000);
            market.requireChangesWithOffer({}, [&] {
                return market.addOffer(acc1, {native, idr, Price{1, 1}, 500});
            });
            acc1.changeTrust(idr, 500);
            REQUIRE_THROWS_AS(acc1.changeTrust(idr, 499),
                              ex_CHANGE_TRUST_INVALID_LIMIT);
            REQUIRE_THROWS_AS(acc1.changeTrust(idr, 0),
                              ex_CHANGE_TRUST_INVALID_LIMIT);
        });
    }
}
