// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTxn.h"
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

TEST_CASE_VERSIONS("liquidity pool withdraw", "[tx][liquiditypool]")
{
    Config cfg = getTestConfig(0, Config::TESTDB_IN_MEMORY);

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
            checkLiquidityPool(*app, pool12, 100, 25, 50, 1);

            // add a different depositor
            root.changeTrust(share12, 50);
            root.liquidityPoolDeposit(pool12, 100, 25, Price{4, 1},
                                      Price{4, 1});
            checkLiquidityPool(*app, pool12, 200, 50, 100, 2);

            // empty the pool share trustline
            acc1.liquidityPoolWithdraw(pool12, 50, 100, 25);
            REQUIRE(acc1.getTrustlineBalance(cur1) == 200);
            REQUIRE(acc1.getTrustlineBalance(cur2) == 50);
            REQUIRE(acc1.getTrustlineBalance(pool12) == 0);
            checkLiquidityPool(*app, pool12, 100, 25, 50, 2);

            // empty the pool
            root.liquidityPoolWithdraw(pool12, 50, 100, 25);
            checkLiquidityPool(*app, pool12, 0, 0, 0, 2);

            // Do another deposit/withdraw where rounding comes into play
            root.allowTrust(cur1, acc1);
            root.allowTrust(cur2, acc1);

            // sqrt(90*24) = is ~46.48, which should get rounded down to 46
            acc1.liquidityPoolDeposit(pool12, 90, 24, Price{90, 24},
                                      Price{90, 24});
            REQUIRE(acc1.getTrustlineBalance(cur1) == 110);
            REQUIRE(acc1.getTrustlineBalance(cur2) == 26);
            REQUIRE(acc1.getTrustlineBalance(pool12) == 46);
            checkLiquidityPool(*app, pool12, 90, 24, 46, 2);

            // floor(2/46*90) = 3
            // floor(2/46*24) = 1
            acc1.liquidityPoolWithdraw(pool12, 2, 0, 0);
            REQUIRE(acc1.getTrustlineBalance(cur1) == 113);
            REQUIRE(acc1.getTrustlineBalance(cur2) == 27);
            REQUIRE(acc1.getTrustlineBalance(pool12) == 44);
            checkLiquidityPool(*app, pool12, 87, 23, 44, 2);

            // empty the pool
            acc1.liquidityPoolWithdraw(pool12, 44, 0, 0);
            REQUIRE(acc1.getTrustlineBalance(cur1) == 200);
            REQUIRE(acc1.getTrustlineBalance(cur2) == 50);
            REQUIRE(acc1.getTrustlineBalance(pool12) == 0);
            checkLiquidityPool(*app, pool12, 0, 0, 0, 2);
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
            checkLiquidityPool(*app, poolNative1, 25, 100, 50, 1);

            // add a different depositor
            root.changeTrust(shareNative1, 50);
            root.liquidityPoolDeposit(poolNative1, 25, 100, Price{1, 4},
                                      Price{1, 4});
            checkLiquidityPool(*app, poolNative1, 50, 200, 100, 2);

            // empty pool share trustline
            balance = acc1.getBalance();
            acc1.liquidityPoolWithdraw(poolNative1, 50, 25, 100);
            REQUIRE(acc1.getBalance() == balance - 100 + 25);
            REQUIRE(acc1.getTrustlineBalance(cur1) == 200);
            REQUIRE(acc1.getTrustlineBalance(poolNative1) == 0);
            checkLiquidityPool(*app, poolNative1, 25, 100, 50, 2);

            // empty the other pool share trustline
            balance = root.getBalance();
            root.liquidityPoolWithdraw(poolNative1, 50, 25, 100);
            REQUIRE(root.getBalance() == balance - 100 + 25);
            REQUIRE(root.getTrustlineBalance(poolNative1) == 0);
            checkLiquidityPool(*app, poolNative1, 0, 0, 0, 2);
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

            // acc1 native line is full (minus the fee paid for this op)
            acc1.manageOffer(0, cur2, native, Price{1, 1},
                             INT64_MAX - acc1.getBalance());

            // we can withdraw enough for the fee of the above operation plus
            // this one
            acc1.liquidityPoolWithdraw(poolNative1, 200, 200, 0);

            // at the limit, so we can't withdraw more than the fee
            REQUIRE_THROWS_AS(
                acc1.liquidityPoolWithdraw(poolNative1, 101, 101, 0),
                ex_LIQUIDITY_POOL_WITHDRAW_LINE_FULL);
        }

        SECTION("withdraw into account with liabilities")
        {
            acc1.changeTrust(cur1, 100);
            acc1.changeTrust(cur2, 100);
            root.pay(acc1, cur1, 100);
            root.pay(acc1, cur2, 100);

            acc1.changeTrust(share12, 100);

            acc1.liquidityPoolDeposit(pool12, 100, 100, Price{1, 1},
                                      Price{1, 1});

            auto cur1OfferID =
                acc1.manageOffer(0, native, cur1, Price{1, 1}, 100);

            REQUIRE_THROWS_AS(acc1.liquidityPoolWithdraw(pool12, 1, 1, 0),
                              ex_LIQUIDITY_POOL_WITHDRAW_LINE_FULL);

            // delete the offer
            acc1.manageOffer(cur1OfferID, native, cur1, Price{1, 1}, 0,
                             ManageOfferEffect::MANAGE_OFFER_DELETED);

            // withdraw should succeed now
            acc1.liquidityPoolWithdraw(pool12, 1, 1, 0);

            // now test liabilities on the second asset
            auto cur2OfferID =
                acc1.manageOffer(0, native, cur2, Price{1, 1}, 99);

            REQUIRE_THROWS_AS(acc1.liquidityPoolWithdraw(pool12, 1, 1, 0),
                              ex_LIQUIDITY_POOL_WITHDRAW_LINE_FULL);

            // delete the offer
            acc1.manageOffer(cur2OfferID, native, cur2, Price{1, 1}, 0,
                             ManageOfferEffect::MANAGE_OFFER_DELETED);

            // withdraw should succeed again
            acc1.liquidityPoolWithdraw(pool12, 1, 1, 0);
        }

        SECTION("both non-native issuer deposit and withdraw")
        {
            root.changeTrust(share12, INT64_MAX);
            root.liquidityPoolDeposit(pool12, INT64_MAX, INT64_MAX, Price{1, 1},
                                      Price{1, 1});
            checkLiquidityPool(*app, pool12, INT64_MAX, INT64_MAX, INT64_MAX,
                               1);

            {
                // test pool full
                acc1.changeTrust(cur1, 1);
                acc1.changeTrust(cur2, 1);
                root.pay(acc1, cur1, 1);
                root.pay(acc1, cur2, 1);
                acc1.changeTrust(share12, 1);

                REQUIRE_THROWS_AS(acc1.liquidityPoolDeposit(
                                      pool12, 1, 1, Price{1, 1}, Price{1, 1}),
                                  ex_LIQUIDITY_POOL_DEPOSIT_POOL_FULL);
            }

            root.liquidityPoolWithdraw(pool12, INT64_MAX, INT64_MAX, 1);
            checkLiquidityPool(*app, pool12, 0, 0, 0, 2);
            root.changeTrust(share12, 0);
        }

        SECTION("one non-native issuer deposit and withdraw")
        {
            root.changeTrust(shareNative1, INT64_MAX);
            root.liquidityPoolDeposit(poolNative1, 1000, 1000, Price{1, 1},
                                      Price{1, 1});
            root.liquidityPoolWithdraw(poolNative1, 1000, 1000, 1000);
            checkLiquidityPool(*app, poolNative1, 0, 0, 0, 1);
            root.changeTrust(shareNative1, 0);
        }

        SECTION("both non-native one asset withdraw is zero")
        {
            acc1.changeTrust(cur1, 10);
            acc1.changeTrust(cur2, 10);
            root.pay(acc1, cur1, 1);
            root.pay(acc1, cur2, 10);
            acc1.changeTrust(share12, 4);

            acc1.liquidityPoolDeposit(pool12, 1, 10, Price{1, 10},
                                      Price{1, 10});
            REQUIRE(acc1.getTrustlineBalance(cur1) == 0);
            REQUIRE(acc1.getTrustlineBalance(cur2) == 0);
            REQUIRE(acc1.getTrustlineBalance(pool12) == 3);
            checkLiquidityPool(*app, pool12, 1, 10, 3, 1);

            acc1.liquidityPoolWithdraw(pool12, 2, 0, 6);
            checkLiquidityPool(*app, pool12, 1, 4, 1, 1);

            acc1.liquidityPoolWithdraw(pool12, 1, 1, 4);
            REQUIRE(acc1.getTrustlineBalance(cur1) == 1);
            REQUIRE(acc1.getTrustlineBalance(cur2) == 10);
            REQUIRE(acc1.getTrustlineBalance(pool12) == 0);
            checkLiquidityPool(*app, pool12, 0, 0, 0, 1);

            // now make assetB withdraw 0
            root.pay(acc1, cur1, 9);
            acc1.pay(root, cur2, 9);

            acc1.liquidityPoolDeposit(pool12, 10, 1, Price{10, 1},
                                      Price{10, 1});
            REQUIRE(acc1.getTrustlineBalance(cur1) == 0);
            REQUIRE(acc1.getTrustlineBalance(cur2) == 0);
            REQUIRE(acc1.getTrustlineBalance(pool12) == 3);
            checkLiquidityPool(*app, pool12, 10, 1, 3, 1);

            acc1.liquidityPoolWithdraw(pool12, 2, 6, 0);
            checkLiquidityPool(*app, pool12, 4, 1, 1, 1);

            acc1.liquidityPoolWithdraw(pool12, 1, 4, 1);
            REQUIRE(acc1.getTrustlineBalance(cur1) == 10);
            REQUIRE(acc1.getTrustlineBalance(cur2) == 1);
            REQUIRE(acc1.getTrustlineBalance(pool12) == 0);
            checkLiquidityPool(*app, pool12, 0, 0, 0, 1);
        }

        SECTION("native asset withdraw is zero")
        {
            acc1.changeTrust(cur1, 10);
            root.pay(acc1, cur1, 10);
            acc1.changeTrust(shareNative1, 4);

            int64_t balance = acc1.getBalance();
            acc1.liquidityPoolDeposit(poolNative1, 1, 10, Price{1, 10},
                                      Price{1, 10});
            REQUIRE(acc1.getBalance() == balance - 100 - 1);
            REQUIRE(acc1.getTrustlineBalance(cur1) == 0);
            REQUIRE(acc1.getTrustlineBalance(poolNative1) == 3);
            checkLiquidityPool(*app, poolNative1, 1, 10, 3, 1);

            acc1.liquidityPoolWithdraw(poolNative1, 2, 0, 6);
            checkLiquidityPool(*app, poolNative1, 1, 4, 1, 1);

            balance = acc1.getBalance();
            acc1.liquidityPoolWithdraw(poolNative1, 1, 1, 4);
            REQUIRE(acc1.getBalance() == balance - 100 + 1);
            REQUIRE(acc1.getTrustlineBalance(cur1) == 10);
            REQUIRE(acc1.getTrustlineBalance(poolNative1) == 0);
            checkLiquidityPool(*app, poolNative1, 0, 0, 0, 1);
        }

        SECTION("large deposit/withdraw test")
        {
            // balances between 1000 and 1050, with # of trades and trade sizes
            // between 5 and 25
            for (int i = 1000; i < 1050; ++i)
            {
                stellar::uniform_int_distribution<int64_t> dist(5, 25);

                SECTION(fmt::format("deposit amount = {}", i))
                {
                    std::vector<std::pair<bool, int64_t>> trades;
                    int64_t numTrades = dist(Catch::rng());
                    for (int j = 0; j < numTrades; ++j)
                    {
                        int64_t tradeSize = dist(Catch::rng());
                        trades.emplace_back(tradeSize % 2 == 0, tradeSize);
                    }

                    depositTradeWithdrawTest(*app, root, i, trades);
                }
            }
        }
    });
}