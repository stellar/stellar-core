// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "lib/catch.hpp"
#include "lib/json/json.h"
#include "main/Application.h"
#include "test/TestAccount.h"
#include "test/TestExceptions.h"
#include "test/TestMarket.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "util/Logging.h"
#include "xdrpp/marshal.h"

using namespace stellar;
using namespace stellar::txtest;

// add data
// change data
// remove data
// remove data that isn't there
// add too much data
TEST_CASE("manage data", "[tx][managedata]")
{
    Config const& cfg = getTestConfig();

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);

    app->start();

    // set up world
    auto root = TestAccount::createRoot(*app);

    const int64_t minBalance =
        app->getLedgerManager().getLastMinBalance(3) - 100;

    auto gateway = root.create("gw", minBalance);

    DataValue value, value2;
    value.resize(64);
    value2.resize(64);
    for (int n = 0; n < 64; n++)
    {
        value[n] = (unsigned char)n;
        value2[n] = (unsigned char)n + 3;
    }

    std::string t1("test");
    std::string t2("test2");
    std::string t3("test3");
    std::string t4("test4");

    for_versions({1}, *app, [&] {
        REQUIRE_THROWS_AS(gateway.manageData(t1, &value),
                          ex_MANAGE_DATA_NOT_SUPPORTED_YET);
        REQUIRE_THROWS_AS(gateway.manageData(t2, &value),
                          ex_MANAGE_DATA_NOT_SUPPORTED_YET);
        // try to add too much data
        REQUIRE_THROWS_AS(gateway.manageData(t3, &value),
                          ex_MANAGE_DATA_NOT_SUPPORTED_YET);

        // modify an existing data entry
        REQUIRE_THROWS_AS(gateway.manageData(t1, &value2),
                          ex_MANAGE_DATA_NOT_SUPPORTED_YET);

        // clear an existing data entry
        REQUIRE_THROWS_AS(gateway.manageData(t1, nullptr),
                          ex_MANAGE_DATA_NOT_SUPPORTED_YET);

        // can now add test3 since test was removed
        REQUIRE_THROWS_AS(gateway.manageData(t3, &value),
                          ex_MANAGE_DATA_NOT_SUPPORTED_YET);

        // fail to remove data entry that isn't present
        REQUIRE_THROWS_AS(gateway.manageData(t4, nullptr),
                          ex_MANAGE_DATA_NOT_SUPPORTED_YET);
    });

    for_versions_from({2, 4}, *app, [&] {
        gateway.manageData(t1, &value);
        gateway.manageData(t2, &value);
        // try to add too much data
        REQUIRE_THROWS_AS(gateway.manageData(t3, &value),
                          ex_MANAGE_DATA_LOW_RESERVE);

        // modify an existing data entry
        gateway.manageData(t1, &value2);

        // clear an existing data entry
        gateway.manageData(t1, nullptr);

        // can now add test3 since test was removed
        gateway.manageData(t3, &value);

        // fail to remove data entry that isn't present
        REQUIRE_THROWS_AS(gateway.manageData(t4, nullptr),
                          ex_MANAGE_DATA_NAME_NOT_FOUND);
    });

    for_versions({3}, *app, [&] {
        REQUIRE_THROWS_AS(gateway.manageData(t1, &value), ex_txINTERNAL_ERROR);
        REQUIRE_THROWS_AS(gateway.manageData(t2, &value), ex_txINTERNAL_ERROR);
        // try to add too much data
        REQUIRE_THROWS_AS(gateway.manageData(t3, &value), ex_txINTERNAL_ERROR);

        // modify an existing data entry
        REQUIRE_THROWS_AS(gateway.manageData(t1, &value2), ex_txINTERNAL_ERROR);

        // clear an existing data entry
        REQUIRE_THROWS_AS(gateway.manageData(t1, nullptr), ex_txINTERNAL_ERROR);

        // can now add test3 since test was removed
        REQUIRE_THROWS_AS(gateway.manageData(t3, &value), ex_txINTERNAL_ERROR);

        // fail to remove data entry that isn't present
        REQUIRE_THROWS_AS(gateway.manageData(t4, nullptr), ex_txINTERNAL_ERROR);
    });

    SECTION("create data with native selling liabilities")
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

        for_versions({2}, *app, [&] { acc1.manageData(t1, &value); });
        for_versions(4, 9, *app, [&] { acc1.manageData(t1, &value); });
        for_versions_from(10, *app, [&] {
            REQUIRE_THROWS_AS(acc1.manageData(t1, &value),
                              ex_MANAGE_DATA_LOW_RESERVE);
            root.pay(acc1, txfee + 1);
            acc1.manageData(t1, &value);
        });
    }

    SECTION("create data with native buying liabilities")
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

        for_versions({2}, *app, [&] { acc1.manageData(t1, &value); });
        for_versions_from(4, *app, [&] { acc1.manageData(t1, &value); });
    }
}
