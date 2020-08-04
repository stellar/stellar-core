// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "test/TestAccount.h"
#include "test/TestExceptions.h"
#include "test/TestMarket.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"

using namespace stellar;
using namespace stellar::txtest;

TEST_CASE("create account", "[tx][createaccount]")
{
    Config cfg = getTestConfig();

    // Do our setup in version 1 so that for_all_versions below does not
    // try to downgrade us from >1 to 1.
    cfg.USE_CONFIG_FOR_GENESIS = false;

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);
    app->start();

    // set up world
    auto root = TestAccount::createRoot(*app);

    int64_t const txfee = app->getLedgerManager().getLastTxFee();
    int64_t const minBalance2 =
        app->getLedgerManager().getLastMinBalance(2) + 10 * txfee;

    SECTION("Success")
    {
        for_all_versions(*app, [&] {
            auto b1 =
                root.create("B", app->getLedgerManager().getLastMinBalance(0));
            SECTION("Account already exists")
            {
                REQUIRE_THROWS_AS(
                    root.create("B",
                                app->getLedgerManager().getLastMinBalance(0)),
                    ex_CREATE_ACCOUNT_ALREADY_EXIST);
            }
        });
    }

    SECTION("Not enough funds (source)")
    {
        for_all_versions(*app, [&] {
            int64_t gatewayPayment = minBalance2 + 1;
            auto gateway = root.create("gate", gatewayPayment);
            REQUIRE_THROWS_AS(gateway.create("B", gatewayPayment),
                              ex_CREATE_ACCOUNT_UNDERFUNDED);
        });
    }

    SECTION("Amount too small to create account")
    {
        for_all_versions(*app, [&] {
            REQUIRE_THROWS_AS(
                root.create("B",
                            app->getLedgerManager().getLastMinBalance(0) - 1),
                ex_CREATE_ACCOUNT_LOW_RESERVE);
        });
    }

    SECTION("with native selling liabilities")
    {
        auto const minBal0 = app->getLedgerManager().getLastMinBalance(0);
        auto const minBal3 = app->getLedgerManager().getLastMinBalance(3);

        auto const native = makeNativeAsset();
        auto acc1 = root.create("acc1", minBal3 + 2 * txfee + 500);
        auto cur1 = acc1.asset("CUR1");
        auto setup = [&]() {
            TestMarket market(*app);
            market.requireChangesWithOffer({}, [&] {
                return market.addOffer(acc1, {native, cur1, Price{1, 1}, 500});
            });
        };
        for_versions_to(9, *app, [&] {
            setup();
            acc1.create("acc2", minBal0 + 1);
        });
        for_versions_from(10, *app, [&] {
            setup();
            REQUIRE_THROWS_AS(acc1.create("acc2", minBal0 + 1),
                              ex_CREATE_ACCOUNT_UNDERFUNDED);
            root.pay(acc1, txfee);
            acc1.create("acc2", minBal0);
        });
    }

    SECTION("with native buying liabilities")
    {
        auto const minBal0 = app->getLedgerManager().getLastMinBalance(0);
        auto const minBal3 = app->getLedgerManager().getLastMinBalance(3);

        auto const native = makeNativeAsset();
        auto acc1 = root.create("acc1", minBal3 + 2 * txfee + 500);
        TestMarket market(*app);

        auto cur1 = acc1.asset("CUR1");
        market.requireChangesWithOffer({}, [&] {
            return market.addOffer(acc1, {cur1, native, Price{1, 1}, 500});
        });

        for_all_versions(*app, [&] { acc1.create("acc2", minBal0 + 500); });
    }
}
