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

TEST_CASE("liquidity pool withdraw", "[tx][liquiditypool]")
{
    Config cfg = getTestConfig();

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);

    // set up world
    auto root = TestAccount::createRoot(*app);

    auto minBal = [&](int32_t n) {
        return app->getLedgerManager().getLastMinBalance(n);
    };

    auto acc1 = root.create("acc1", minBal(10));
    auto native = makeNativeAsset();
    auto cur1 = makeAsset(root, "CUR1");
    auto cur2 = makeAsset(root, "CUR2");
    auto share12 =
        makeChangeTrustAssetPoolShare(cur1, cur2, LIQUIDITY_POOL_FEE_V18);
    auto pool12 = xdrSha256(share12.liquidityPool());
    auto shareNative1 =
        makeChangeTrustAssetPoolShare(native, cur1, LIQUIDITY_POOL_FEE_V18);
    auto poolNative1 = xdrSha256(shareNative1.liquidityPool());

    SECTION("not supported before version 18")
    {
        for_versions_to(17, *app, [&] {
            REQUIRE_THROWS_AS(acc1.liquidityPoolWithdraw(PoolID{}, 0, 0, 0),
                              ex_opNOT_SUPPORTED);
        });
    }

    for_versions_from(18, *app, [&] {
        SECTION("malformed")
        {
            REQUIRE_THROWS_AS(acc1.liquidityPoolWithdraw(PoolID{}, 0, 1, 1),
                              ex_LIQUIDITY_POOL_WITHDRAW_MALFORMED);

            REQUIRE_THROWS_AS(acc1.liquidityPoolWithdraw(PoolID{}, 1, -1, 1),
                              ex_LIQUIDITY_POOL_WITHDRAW_MALFORMED);

            REQUIRE_THROWS_AS(acc1.liquidityPoolWithdraw(PoolID{}, 1, 1, -1),
                              ex_LIQUIDITY_POOL_WITHDRAW_MALFORMED);
        }

        SECTION("both non-native without liabilities")
        {
            REQUIRE_THROWS_AS(acc1.liquidityPoolWithdraw(pool12, 1, 0, 0),
                              ex_LIQUIDITY_POOL_WITHDRAW_NO_TRUST);

            acc1.changeTrust(cur1, 200);
            acc1.changeTrust(cur2, 50);
            root.pay(acc1, cur1, 200);
            root.pay(acc1, cur2, 50);

            acc1.changeTrust(share12, 100);

            REQUIRE_THROWS_AS(acc1.liquidityPoolWithdraw(pool12, 1, 0, 0),
                              ex_LIQUIDITY_POOL_WITHDRAW_UNDERFUNDED);

            acc1.liquidityPoolDeposit(pool12, 200, 50, Price{4, 1},
                                      Price{4, 1});

            REQUIRE_THROWS_AS(acc1.liquidityPoolWithdraw(pool12, 100, 201, 0),
                              ex_LIQUIDITY_POOL_WITHDRAW_UNDER_MINIMUM);

            REQUIRE_THROWS_AS(acc1.liquidityPoolWithdraw(pool12, 100, 0, 51),
                              ex_LIQUIDITY_POOL_WITHDRAW_UNDER_MINIMUM);

            acc1.changeTrust(cur1, 1);
            REQUIRE_THROWS_AS(acc1.liquidityPoolWithdraw(pool12, 2, 0, 0),
                              ex_LIQUIDITY_POOL_WITHDRAW_LINE_FULL);

            acc1.changeTrust(cur2, 1);
            REQUIRE_THROWS_AS(acc1.liquidityPoolWithdraw(pool12, 2, 0, 0),
                              ex_LIQUIDITY_POOL_WITHDRAW_LINE_FULL);

            acc1.changeTrust(cur1, 200);
            acc1.changeTrust(cur2, 50);

            // withdrawal should work even if just authorized to maintain
            // liabililties
            root.setOptions(setFlags(AUTH_REVOCABLE_FLAG));
            root.allowMaintainLiabilities(cur1, acc1);
            root.allowMaintainLiabilities(cur2, acc1);

            // success
            acc1.liquidityPoolWithdraw(pool12, 50, 100, 25);
            REQUIRE(acc1.getTrustlineBalance(cur1) == 100);
            REQUIRE(acc1.getTrustlineBalance(cur2) == 25);
            REQUIRE(acc1.getTrustlineBalance(pool12) == 50);
            checkLiquidityPool(*app, pool12, 100, 25, 50);

            // empty the pool
            acc1.liquidityPoolWithdraw(pool12, 50, 100, 25);
            REQUIRE(acc1.getTrustlineBalance(cur1) == 200);
            REQUIRE(acc1.getTrustlineBalance(cur2) == 50);
            REQUIRE(acc1.getTrustlineBalance(pool12) == 0);
            checkLiquidityPool(*app, pool12, 0, 0, 0);

            // Do another deposit/withdraw where rounding comes into play
            root.allowTrust(cur1, acc1);
            root.allowTrust(cur2, acc1);

            // sqrt(90*24) = is ~46.48, which should get rounded up to 47
            acc1.liquidityPoolDeposit(pool12, 90, 24, Price{90, 24},
                                      Price{90, 24});
            REQUIRE(acc1.getTrustlineBalance(cur1) == 110);
            REQUIRE(acc1.getTrustlineBalance(cur2) == 26);
            REQUIRE(acc1.getTrustlineBalance(pool12) == 47);
            checkLiquidityPool(*app, pool12, 90, 24, 47);

            // floor(2/47*90) = 3
            // floor(2/47*24) = 1
            acc1.liquidityPoolWithdraw(pool12, 2, 0, 0);
            REQUIRE(acc1.getTrustlineBalance(cur1) == 113);
            REQUIRE(acc1.getTrustlineBalance(cur2) == 27);
            REQUIRE(acc1.getTrustlineBalance(pool12) == 45);
            checkLiquidityPool(*app, pool12, 87, 23, 45);

            // empty the pool
            acc1.liquidityPoolWithdraw(pool12, 45, 0, 0);
            REQUIRE(acc1.getTrustlineBalance(cur1) == 200);
            REQUIRE(acc1.getTrustlineBalance(cur2) == 50);
            REQUIRE(acc1.getTrustlineBalance(pool12) == 0);
            checkLiquidityPool(*app, pool12, 0, 0, 0);
        }

        SECTION("one non-native without liabilities")
        {
            REQUIRE_THROWS_AS(acc1.liquidityPoolWithdraw(poolNative1, 1, 0, 0),
                              ex_LIQUIDITY_POOL_WITHDRAW_NO_TRUST);

            acc1.changeTrust(cur1, 200);
            root.pay(acc1, cur1, 200);

            acc1.changeTrust(shareNative1, 100);

            REQUIRE_THROWS_AS(acc1.liquidityPoolWithdraw(poolNative1, 1, 0, 0),
                              ex_LIQUIDITY_POOL_WITHDRAW_UNDERFUNDED);

            acc1.liquidityPoolDeposit(poolNative1, 50, 200, Price{1, 4},
                                      Price{1, 4});

            REQUIRE_THROWS_AS(
                acc1.liquidityPoolWithdraw(poolNative1, 100, 51, 0),
                ex_LIQUIDITY_POOL_WITHDRAW_UNDER_MINIMUM);

            REQUIRE_THROWS_AS(
                acc1.liquidityPoolWithdraw(poolNative1, 100, 0, 201),
                ex_LIQUIDITY_POOL_WITHDRAW_UNDER_MINIMUM);

            acc1.changeTrust(cur1, 1);
            REQUIRE_THROWS_AS(acc1.liquidityPoolWithdraw(poolNative1, 2, 0, 0),
                              ex_LIQUIDITY_POOL_WITHDRAW_LINE_FULL);

            acc1.changeTrust(cur1, 200);

            // success
            int64_t balance = acc1.getBalance();
            acc1.liquidityPoolWithdraw(poolNative1, 50, 25, 100);
            REQUIRE(acc1.getBalance() == balance - 100 + 25);
            REQUIRE(acc1.getTrustlineBalance(cur1) == 100);
            REQUIRE(acc1.getTrustlineBalance(poolNative1) == 50);
            checkLiquidityPool(*app, poolNative1, 25, 100, 50);

            // empty the pool
            balance = acc1.getBalance();
            acc1.liquidityPoolWithdraw(poolNative1, 50, 25, 100);
            REQUIRE(acc1.getBalance() == balance - 100 + 25);
            REQUIRE(acc1.getTrustlineBalance(cur1) == 200);
            REQUIRE(acc1.getTrustlineBalance(poolNative1) == 0);
            checkLiquidityPool(*app, poolNative1, 0, 0, 0);
        }

        SECTION("line full on native balance")
        {
            acc1.changeTrust(cur1, INT64_MAX);
            root.pay(acc1, cur1, 1000);

            acc1.changeTrust(shareNative1, INT64_MAX);

            acc1.liquidityPoolDeposit(poolNative1, 1000, 1000, Price{1, 1},
                                      Price{1, 1});

            // use cur2 so it's clear withdraw failures will be due to the
            // native balance and not cur1
            acc1.changeTrust(cur2, INT64_MAX);
            root.pay(acc1, cur2, INT64_MAX);

            // acc1 native line is full
            acc1.manageOffer(0, cur2, native, Price{1, 1},
                             INT64_MAX - acc1.getBalance());

            REQUIRE_THROWS_AS(
                acc1.liquidityPoolWithdraw(poolNative1, 1000, 1000, 0),
                ex_LIQUIDITY_POOL_WITHDRAW_LINE_FULL);
        }
    });
}
