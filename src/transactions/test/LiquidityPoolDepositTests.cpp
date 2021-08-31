// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/catch.hpp"
#include "main/Application.h"
#include "test/TestAccount.h"
#include "test/TestExceptions.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/TransactionUtils.h"

using namespace stellar;
using namespace stellar::txtest;

TEST_CASE("liquidity pool deposit", "[tx][liquiditypool]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());

    // set up world
    auto const& lm = app->getLedgerManager();
    auto minBal = [&](int32_t n) { return lm.getLastMinBalance(n); };
    auto root = TestAccount::createRoot(*app);
    auto native = makeNativeAsset();
    auto cur1 = makeAsset(root, "CUR1");
    auto cur2 = makeAsset(root, "CUR2");
    auto share12 =
        makeChangeTrustAssetPoolShare(cur1, cur2, LIQUIDITY_POOL_FEE_V18);
    auto pool12 = xdrSha256(share12.liquidityPool());
    auto shareNative1 =
        makeChangeTrustAssetPoolShare(native, cur1, LIQUIDITY_POOL_FEE_V18);
    auto poolNative1 = xdrSha256(shareNative1.liquidityPool());

    SECTION("not supported before protocol 18")
    {
        for_versions_to(17, *app, [&] {
            REQUIRE_THROWS_AS(root.liquidityPoolDeposit(
                                  {}, 100, 100, Price{1, 1}, Price{1, 1}),
                              ex_opNOT_SUPPORTED);
        });
    }

    SECTION("validity checks")
    {
        for_versions_from(18, *app, [&] {
            // bad maxAmountA
            REQUIRE_THROWS_AS(
                root.liquidityPoolDeposit({}, 0, 100, Price{1, 1}, Price{1, 1}),
                ex_LIQUIDITY_POOL_DEPOSIT_MALFORMED);
            REQUIRE_THROWS_AS(root.liquidityPoolDeposit(
                                  {}, -1, 100, Price{1, 1}, Price{1, 1}),
                              ex_LIQUIDITY_POOL_DEPOSIT_MALFORMED);
            // bad maxAmountB
            REQUIRE_THROWS_AS(
                root.liquidityPoolDeposit({}, 100, 0, Price{1, 1}, Price{1, 1}),
                ex_LIQUIDITY_POOL_DEPOSIT_MALFORMED);
            REQUIRE_THROWS_AS(root.liquidityPoolDeposit(
                                  {}, 100, -1, Price{1, 1}, Price{1, 1}),
                              ex_LIQUIDITY_POOL_DEPOSIT_MALFORMED);
            // bad minPrice.n
            REQUIRE_THROWS_AS(root.liquidityPoolDeposit(
                                  {}, 100, 100, Price{0, 1}, Price{1, 1}),
                              ex_LIQUIDITY_POOL_DEPOSIT_MALFORMED);
            REQUIRE_THROWS_AS(root.liquidityPoolDeposit(
                                  {}, 100, 100, Price{-1, 1}, Price{1, 1}),
                              ex_LIQUIDITY_POOL_DEPOSIT_MALFORMED);
            // bad minPrice.d
            REQUIRE_THROWS_AS(root.liquidityPoolDeposit(
                                  {}, 100, 100, Price{1, 0}, Price{1, 1}),
                              ex_LIQUIDITY_POOL_DEPOSIT_MALFORMED);
            REQUIRE_THROWS_AS(root.liquidityPoolDeposit(
                                  {}, 100, 100, Price{1, -1}, Price{1, 1}),
                              ex_LIQUIDITY_POOL_DEPOSIT_MALFORMED);
            // bad maxPrice.n
            REQUIRE_THROWS_AS(root.liquidityPoolDeposit(
                                  {}, 100, 100, Price{1, 1}, Price{0, 1}),
                              ex_LIQUIDITY_POOL_DEPOSIT_MALFORMED);
            REQUIRE_THROWS_AS(root.liquidityPoolDeposit(
                                  {}, 100, 100, Price{1, 1}, Price{-1, 1}),
                              ex_LIQUIDITY_POOL_DEPOSIT_MALFORMED);
            // bad maxPrice.d
            REQUIRE_THROWS_AS(root.liquidityPoolDeposit(
                                  {}, 100, 100, Price{1, 1}, Price{1, 0}),
                              ex_LIQUIDITY_POOL_DEPOSIT_MALFORMED);
            REQUIRE_THROWS_AS(root.liquidityPoolDeposit(
                                  {}, 100, 100, Price{1, 1}, Price{1, -1}),
                              ex_LIQUIDITY_POOL_DEPOSIT_MALFORMED);
        });
    }

    SECTION("both non-native without liabilities")
    {
        for_versions_from(18, *app, [&] {
            root.setOptions(setFlags(AUTH_REQUIRED_FLAG));

            // This section is all about depositing into an empty pool
            auto a1 = root.create("a1", minBal(10));

            // No trust
            REQUIRE_THROWS_AS(a1.liquidityPoolDeposit(pool12, 400, 900,
                                                      Price{1, 1}, Price{1, 1}),
                              ex_LIQUIDITY_POOL_DEPOSIT_NO_TRUST);
            a1.changeTrust(cur1, 2000);
            a1.changeTrust(cur2, 2000);
            root.allowMaintainLiabilities(cur1, a1);
            root.allowMaintainLiabilities(cur2, a1);
            a1.changeTrust(share12, 1);

            // Not authorized
            REQUIRE_THROWS_AS(a1.liquidityPoolDeposit(pool12, 400, 900,
                                                      Price{1, 1}, Price{1, 1}),
                              ex_LIQUIDITY_POOL_DEPOSIT_NOT_AUTHORIZED);
            root.allowTrust(cur1, a1);
            REQUIRE_THROWS_AS(a1.liquidityPoolDeposit(pool12, 400, 900,
                                                      Price{1, 1}, Price{1, 1}),
                              ex_LIQUIDITY_POOL_DEPOSIT_NOT_AUTHORIZED);
            root.allowTrust(cur2, a1);

            // Underfunded
            REQUIRE_THROWS_AS(a1.liquidityPoolDeposit(pool12, 400, 900,
                                                      Price{1, 1}, Price{1, 1}),
                              ex_LIQUIDITY_POOL_DEPOSIT_UNDERFUNDED);
            root.pay(a1, cur1, 800);
            REQUIRE_THROWS_AS(a1.liquidityPoolDeposit(pool12, 400, 900,
                                                      Price{1, 1}, Price{1, 1}),
                              ex_LIQUIDITY_POOL_DEPOSIT_UNDERFUNDED);
            root.pay(a1, cur2, 1800);

            // Bad price
            REQUIRE_THROWS_AS(a1.liquidityPoolDeposit(pool12, 400, 900,
                                                      Price{1, 1}, Price{1, 1}),
                              ex_LIQUIDITY_POOL_DEPOSIT_BAD_PRICE);

            // Line full
            REQUIRE_THROWS_AS(a1.liquidityPoolDeposit(pool12, 400, 900,
                                                      Price{4, 9}, Price{4, 9}),
                              ex_LIQUIDITY_POOL_DEPOSIT_LINE_FULL);
            a1.changeTrust(share12, 600);

            // Success
            a1.liquidityPoolDeposit(pool12, 400, 900, Price{4, 9}, Price{4, 9});
            REQUIRE(a1.getTrustlineBalance(cur1) == 400);
            REQUIRE(a1.getTrustlineBalance(cur2) == 900);
            REQUIRE(a1.getTrustlineBalance(pool12) == 600);
            checkLiquidityPool(*app, pool12, 400, 900, 600);

            // This section is all about depositing into a non-empty pool
            auto a2 = root.create("a2", minBal(10));

            // No trust
            REQUIRE_THROWS_AS(a2.liquidityPoolDeposit(pool12, 400, 900,
                                                      Price{1, 1}, Price{1, 1}),
                              ex_LIQUIDITY_POOL_DEPOSIT_NO_TRUST);
            a2.changeTrust(cur1, INT64_MAX);
            a2.changeTrust(cur2, INT64_MAX);
            root.allowMaintainLiabilities(cur1, a2);
            root.allowMaintainLiabilities(cur2, a2);
            a2.changeTrust(share12, 1);

            // Not authorized
            REQUIRE_THROWS_AS(a2.liquidityPoolDeposit(pool12, 400, 900,
                                                      Price{1, 1}, Price{1, 1}),
                              ex_LIQUIDITY_POOL_DEPOSIT_NOT_AUTHORIZED);
            root.allowTrust(cur1, a2);
            REQUIRE_THROWS_AS(a2.liquidityPoolDeposit(pool12, 400, 900,
                                                      Price{1, 1}, Price{1, 1}),
                              ex_LIQUIDITY_POOL_DEPOSIT_NOT_AUTHORIZED);
            root.allowTrust(cur2, a2);

            // Underfunded
            REQUIRE_THROWS_AS(a2.liquidityPoolDeposit(pool12, 400, 900,
                                                      Price{1, 1}, Price{1, 1}),
                              ex_LIQUIDITY_POOL_DEPOSIT_UNDERFUNDED);
            root.pay(a2, cur1, INT64_MAX);
            REQUIRE_THROWS_AS(a2.liquidityPoolDeposit(pool12, 400, 900,
                                                      Price{1, 1}, Price{1, 1}),
                              ex_LIQUIDITY_POOL_DEPOSIT_UNDERFUNDED);
            root.pay(a2, cur2, INT64_MAX);

            // Bad price
            REQUIRE_THROWS_AS(a2.liquidityPoolDeposit(pool12, 400, 900,
                                                      Price{1, 1}, Price{1, 1}),
                              ex_LIQUIDITY_POOL_DEPOSIT_BAD_PRICE);

            // Line full
            REQUIRE_THROWS_AS(a2.liquidityPoolDeposit(pool12, 400, 900,
                                                      Price{4, 9}, Price{4, 9}),
                              ex_LIQUIDITY_POOL_DEPOSIT_LINE_FULL);
            a2.changeTrust(share12, INT64_MAX);

            // Pool full
            REQUIRE_THROWS_AS(a2.liquidityPoolDeposit(
                                  pool12, INT64_MAX, INT64_MAX,
                                  Price{1, INT32_MAX}, Price{INT32_MAX, 1}),
                              ex_LIQUIDITY_POOL_DEPOSIT_POOL_FULL);

            // Success
            a2.liquidityPoolDeposit(pool12, 101, 151, Price{3, 9}, Price{5, 9});
            REQUIRE(a2.getTrustlineBalance(cur1) == INT64_MAX - 67);
            REQUIRE(a2.getTrustlineBalance(cur2) == INT64_MAX - 150);
            REQUIRE(a2.getTrustlineBalance(pool12) == 100);
            checkLiquidityPool(*app, pool12, 467, 1050, 700);
        });
    }

    SECTION("one non-native without liabilities")
    {
        for_versions_from(18, *app, [&] {
            root.setOptions(setFlags(AUTH_REQUIRED_FLAG));

            // This section is all about depositing into an empty pool
            auto a1 = root.create("a1", minBal(3) + 6 * 100);

            // No trust
            REQUIRE_THROWS_AS(a1.liquidityPoolDeposit(poolNative1, 1, INT32_MAX,
                                                      Price{1, 1}, Price{1, 1}),
                              ex_LIQUIDITY_POOL_DEPOSIT_NO_TRUST);
            a1.changeTrust(cur1, INT64_MAX);
            root.allowMaintainLiabilities(cur1, a1);
            a1.changeTrust(shareNative1, 1);

            // Not authorized
            REQUIRE_THROWS_AS(a1.liquidityPoolDeposit(poolNative1, 1, INT32_MAX,
                                                      Price{1, 1}, Price{1, 1}),
                              ex_LIQUIDITY_POOL_DEPOSIT_NOT_AUTHORIZED);
            root.allowTrust(cur1, a1);

            // Underfunded
            REQUIRE_THROWS_AS(a1.liquidityPoolDeposit(poolNative1, 1, INT32_MAX,
                                                      Price{1, 1}, Price{1, 1}),
                              ex_LIQUIDITY_POOL_DEPOSIT_UNDERFUNDED);
            root.pay(a1, minBal(10));
            REQUIRE_THROWS_AS(a1.liquidityPoolDeposit(poolNative1, 1, INT32_MAX,
                                                      Price{1, 1}, Price{1, 1}),
                              ex_LIQUIDITY_POOL_DEPOSIT_UNDERFUNDED);
            root.pay(a1, cur1, INT64_MAX);

            // Bad price
            REQUIRE_THROWS_AS(a1.liquidityPoolDeposit(poolNative1, 1, INT32_MAX,
                                                      Price{1, 1}, Price{1, 1}),
                              ex_LIQUIDITY_POOL_DEPOSIT_BAD_PRICE);

            // Line full
            REQUIRE_THROWS_AS(a1.liquidityPoolDeposit(poolNative1, 1, INT32_MAX,
                                                      Price{1, INT32_MAX},
                                                      Price{1, INT32_MAX}),
                              ex_LIQUIDITY_POOL_DEPOSIT_LINE_FULL);
            a1.changeTrust(shareNative1, INT64_MAX);

            // Success
            int64_t balance = a1.getBalance();
            a1.liquidityPoolDeposit(poolNative1, 1, INT32_MAX,
                                    Price{1, INT32_MAX}, Price{1, INT32_MAX});
            REQUIRE(a1.getBalance() == balance - 100 - 1);
            REQUIRE(a1.getTrustlineBalance(cur1) == INT64_MAX - INT32_MAX);
            REQUIRE(a1.getTrustlineBalance(poolNative1) == 46341);
            checkLiquidityPool(*app, poolNative1, 1, INT32_MAX, 46341);

            // This section is all about depositing into a non-empty pool
            auto a2 = root.create("a2", minBal(3) + 6 * 100);

            // No trust
            REQUIRE_THROWS_AS(a2.liquidityPoolDeposit(poolNative1, 1, INT32_MAX,
                                                      Price{1, 1}, Price{1, 1}),
                              ex_LIQUIDITY_POOL_DEPOSIT_NO_TRUST);
            a2.changeTrust(cur1, INT64_MAX);
            root.allowMaintainLiabilities(cur1, a2);
            a2.changeTrust(shareNative1, 1);

            // Not authorized
            REQUIRE_THROWS_AS(a2.liquidityPoolDeposit(poolNative1, 1, INT32_MAX,
                                                      Price{1, 1}, Price{1, 1}),
                              ex_LIQUIDITY_POOL_DEPOSIT_NOT_AUTHORIZED);
            root.allowTrust(cur1, a2);

            // Underfunded
            REQUIRE_THROWS_AS(a2.liquidityPoolDeposit(poolNative1, 1, INT32_MAX,
                                                      Price{1, 1}, Price{1, 1}),
                              ex_LIQUIDITY_POOL_DEPOSIT_UNDERFUNDED);
            root.pay(a2, minBal(10) + 5000000000);
            REQUIRE_THROWS_AS(a2.liquidityPoolDeposit(poolNative1, 1, INT32_MAX,
                                                      Price{1, 1}, Price{1, 1}),
                              ex_LIQUIDITY_POOL_DEPOSIT_UNDERFUNDED);
            root.pay(a2, cur1, INT64_MAX);

            // Bad price
            REQUIRE_THROWS_AS(a2.liquidityPoolDeposit(poolNative1, 1, INT32_MAX,
                                                      Price{1, 1}, Price{1, 1}),
                              ex_LIQUIDITY_POOL_DEPOSIT_BAD_PRICE);

            // Line full
            REQUIRE_THROWS_AS(a2.liquidityPoolDeposit(poolNative1, 1, INT32_MAX,
                                                      Price{1, INT32_MAX},
                                                      Price{INT32_MAX, 1}),
                              ex_LIQUIDITY_POOL_DEPOSIT_LINE_FULL);
            a2.changeTrust(shareNative1, INT64_MAX);

            // Pool full
            REQUIRE_THROWS_AS(a2.liquidityPoolDeposit(
                                  poolNative1, 5000000000, INT64_MAX,
                                  Price{1, INT32_MAX}, Price{INT32_MAX, 1}),
                              ex_LIQUIDITY_POOL_DEPOSIT_POOL_FULL);

            // Success
            balance = a2.getBalance();
            a2.liquidityPoolDeposit(poolNative1, 1, INT32_MAX,
                                    Price{1, INT32_MAX}, Price{INT32_MAX, 1});
            REQUIRE(a2.getBalance() == balance - 100 - 1);
            REQUIRE(a2.getTrustlineBalance(cur1) == INT64_MAX - INT32_MAX);
            REQUIRE(a2.getTrustlineBalance(poolNative1) == 46341);
            checkLiquidityPool(*app, poolNative1, 2, 2 * (int64_t)INT32_MAX,
                               92682);
        });
    }

    SECTION("underfunded due to liabilities")
    {
        for_versions_from(18, *app, [&] {
            auto a1 = root.create("a1", minBal(10));
            auto buyingAsset = makeAsset(root, "BUY1");

            a1.changeTrust(cur1, 1);
            a1.changeTrust(cur2, 1);
            a1.changeTrust(buyingAsset, 1);

            root.pay(a1, cur1, 1);
            root.pay(a1, cur2, 1);
            a1.changeTrust(share12, 2);

            auto underfundedTest = [&](Asset const& sellingAsset) {
                bool isNative = sellingAsset.type() == ASSET_TYPE_NATIVE;
                auto poolID = isNative ? poolNative1 : pool12;

                if (isNative)
                {
                    // leave enough for fees and offer
                    a1.pay(root, a1.getAvailableBalance() -
                                     lm.getLastTxFee() * 3 -
                                     lm.getLastReserve() - 1);
                }

                auto offerID1 = a1.manageOffer(0, sellingAsset, buyingAsset,
                                               Price{1, 1}, 1);
                REQUIRE_THROWS_AS(a1.liquidityPoolDeposit(
                                      poolID, 1, 1, Price{1, 1}, Price{1, 1}),
                                  ex_LIQUIDITY_POOL_DEPOSIT_UNDERFUNDED);

                if (isNative)
                {
                    // pay enough for the fees for the next two ops
                    root.pay(a1, lm.getLastTxFee() * 2);
                }

                // delete offer
                a1.manageOffer(offerID1, sellingAsset, buyingAsset, Price{1, 1},
                               0, ManageOfferEffect::MANAGE_OFFER_DELETED);

                // deposit succeeds
                a1.liquidityPoolDeposit(poolID, 1, 1, Price{1, 1}, Price{1, 1});
            };

            SECTION("assetA")
            {
                underfundedTest(cur1);

                // do it again so we can test with an existing pool
                root.pay(a1, cur1, 1);
                root.pay(a1, cur2, 1);
                underfundedTest(cur1);
            }

            SECTION("assetB")
            {
                underfundedTest(cur2);

                // do it again so we can test with an existing pool
                root.pay(a1, cur1, 1);
                root.pay(a1, cur2, 1);
                underfundedTest(cur2);
            }

            SECTION("native")
            {
                a1.changeTrust(shareNative1, 2);
                underfundedTest(native);

                // do it again so we can test with an existing pool
                root.pay(a1, minBal(10));
                root.pay(a1, cur1, 1);
                underfundedTest(native);
            }
        });
    }
}
