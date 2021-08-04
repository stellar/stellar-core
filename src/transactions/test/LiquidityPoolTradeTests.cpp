// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
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

TEST_CASE("liquidity pool trade", "[tx][liquiditypool]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());

    // set up world
    auto minBal = [&](int32_t n) {
        return app->getLedgerManager().getLastMinBalance(n);
    };
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

    for_versions_from(18, *app, [&] {
        SECTION("without offers")
        {
            auto a1 = root.create("a1", minBal(10));
            a1.changeTrust(cur1, INT64_MAX);
            a1.changeTrust(cur2, INT64_MAX);
            a1.changeTrust(share12, INT64_MAX);
            a1.changeTrust(shareNative1, INT64_MAX);
            root.pay(a1, cur1, 10000);
            root.pay(a1, cur2, 10000);

            auto a2 = root.create("a2", minBal(10));
            a2.changeTrust(cur1, INT64_MAX);
            a2.changeTrust(cur2, INT64_MAX);
            a2.changeTrust(share12, INT64_MAX);
            a2.changeTrust(shareNative1, INT64_MAX);
            root.pay(a2, cur1, 10000);
            root.pay(a2, cur2, 10000);

            a1.liquidityPoolDeposit(pool12, 1000, 1000, Price{1, INT32_MAX},
                                    Price{INT32_MAX, 1});
            a1.liquidityPoolDeposit(poolNative1, 1000, 1000,
                                    Price{1, INT32_MAX}, Price{INT32_MAX, 1});

            auto getBalance = [](TestAccount const& acc, Asset const& asset) {
                return asset.type() == ASSET_TYPE_NATIVE
                           ? acc.getBalance()
                           : acc.getTrustlineBalance(asset);
            };

            auto check = [&](Asset const& toPoolAsset, int64_t toPool,
                             Asset const& fromPoolAsset, int64_t fromPool,
                             auto apply) {
                auto share =
                    toPoolAsset < fromPoolAsset
                        ? makeChangeTrustAssetPoolShare(toPoolAsset,
                                                        fromPoolAsset,
                                                        LIQUIDITY_POOL_FEE_V18)
                        : makeChangeTrustAssetPoolShare(fromPoolAsset,
                                                        toPoolAsset,
                                                        LIQUIDITY_POOL_FEE_V18);
                auto pool = xdrSha256(share.liquidityPool());

                int64_t toPoolBalanceBefore1 = getBalance(a1, toPoolAsset);
                int64_t fromPoolBalanceBefore1 = getBalance(a1, fromPoolAsset);
                int64_t toPoolBalanceBefore2 = getBalance(a2, toPoolAsset);
                int64_t fromPoolBalanceBefore2 = getBalance(a2, fromPoolAsset);
                if (toPoolAsset.type() == ASSET_TYPE_NATIVE)
                {
                    toPoolBalanceBefore2 -= 100;
                }
                else if (fromPoolAsset.type() == ASSET_TYPE_NATIVE)
                {
                    fromPoolBalanceBefore2 -= 100;
                }

                auto const res = apply();
                REQUIRE(res.success().offers.size() == 1);
                REQUIRE(res.success().offers[0].liquidityPool() ==
                        ClaimLiquidityAtom(pool, fromPoolAsset, fromPool,
                                           toPoolAsset, toPool));

                REQUIRE(getBalance(a1, toPoolAsset) == toPoolBalanceBefore1);
                REQUIRE(getBalance(a1, fromPoolAsset) ==
                        fromPoolBalanceBefore1);
                REQUIRE(getBalance(a2, toPoolAsset) ==
                        toPoolBalanceBefore2 - toPool);
                REQUIRE(getBalance(a2, fromPoolAsset) ==
                        fromPoolBalanceBefore2 + fromPool);

                LedgerTxn ltx(app->getLedgerTxnRoot());
                auto lp = loadLiquidityPool(ltx, pool);
                auto const& cp =
                    lp.current().data.liquidityPool().body.constantProduct();
                if (fromPoolAsset == cp.params.assetA &&
                    toPoolAsset == cp.params.assetB)
                {
                    REQUIRE(cp.reserveA == 1000 - fromPool);
                    REQUIRE(cp.reserveB == 1000 + toPool);
                }
                else if (fromPoolAsset == cp.params.assetB &&
                         toPoolAsset == cp.params.assetA)
                {
                    REQUIRE(cp.reserveA == 1000 + toPool);
                    REQUIRE(cp.reserveB == 1000 - fromPool);
                }
                else
                {
                    REQUIRE(false);
                }
            };

            auto testStrictReceive = [&](Asset const& sendAsset,
                                         int64_t sendAmount,
                                         Asset const& destAsset,
                                         int64_t destAmount) {
                SECTION("satisfies limit")
                {
                    check(sendAsset, sendAmount, destAsset, destAmount, [&] {
                        return a2.pay(a2, sendAsset, sendAmount + 1, destAsset,
                                      destAmount, {});
                    });
                }

                SECTION("at limit")
                {
                    check(sendAsset, sendAmount, destAsset, destAmount, [&] {
                        return a2.pay(a2, sendAsset, sendAmount, destAsset,
                                      destAmount, {});
                    });
                }

                SECTION("fails due to limit")
                {
                    REQUIRE_THROWS_AS(
                        a2.pay(a2, sendAsset, sendAmount - 1, destAsset,
                               destAmount, {}),
                        ex_PATH_PAYMENT_STRICT_RECEIVE_OVER_SENDMAX);
                }
            };

            auto testStrictSend = [&](Asset const& sendAsset,
                                      int64_t sendAmount,
                                      Asset const& destAsset,
                                      int64_t destAmount) {
                SECTION("satisfies limit")
                {
                    check(sendAsset, sendAmount, destAsset, destAmount, [&] {
                        return a2.pathPaymentStrictSend(a2, sendAsset,
                                                        sendAmount, destAsset,
                                                        destAmount - 1, {});
                    });
                }

                SECTION("at limit")
                {
                    check(sendAsset, sendAmount, destAsset, destAmount, [&] {
                        return a2.pathPaymentStrictSend(a2, sendAsset,
                                                        sendAmount, destAsset,
                                                        destAmount, {});
                    });
                }

                SECTION("fails due to limit")
                {
                    REQUIRE_THROWS_AS(
                        a2.pathPaymentStrictSend(a2, sendAsset, sendAmount,
                                                 destAsset, destAmount + 1, {}),
                        ex_PATH_PAYMENT_STRICT_SEND_UNDER_DESTMIN);
                }
            };

            SECTION("both non-native, strict receive")
            {
                SECTION("pool sells cur1")
                {
                    testStrictReceive(cur2, 112, cur1, 100);
                }
                SECTION("pool sells cur2")
                {
                    testStrictReceive(cur1, 112, cur2, 100);
                }
            }

            SECTION("one native, strict receive")
            {
                SECTION("pool sells cur1")
                {
                    testStrictReceive(native, 112, cur1, 100);
                }
                SECTION("pool sells native")
                {
                    testStrictReceive(cur1, 112, native, 100);
                }
            }

            SECTION("both non-native, strict send")
            {
                SECTION("pool sells cur1")
                {
                    testStrictSend(cur2, 112, cur1, 100);
                }
                SECTION("pool sells cur2")
                {
                    testStrictSend(cur1, 112, cur2, 100);
                }
            }

            SECTION("one native, strict send")
            {
                SECTION("pool sells cur1")
                {
                    testStrictSend(native, 112, cur1, 100);
                }
                SECTION("pool sells native")
                {
                    testStrictSend(cur1, 112, native, 100);
                }
            }
        }

        SECTION("chooses best price")
        {
            auto a1 = root.create("a1", minBal(10));
            a1.changeTrust(cur1, INT64_MAX);
            a1.changeTrust(cur2, INT64_MAX);
            a1.changeTrust(share12, INT64_MAX);
            root.pay(a1, cur1, 10000);
            root.pay(a1, cur2, 10000);

            auto a2 = root.create("a2", minBal(10));
            a2.changeTrust(cur1, INT64_MAX);
            a2.changeTrust(cur2, INT64_MAX);
            a2.changeTrust(share12, INT64_MAX);
            root.pay(a2, cur1, 10000);
            root.pay(a2, cur2, 10000);

            SECTION("pool has strictly better price")
            {
                // Buying 10 cur2 costs 20 cur1
                a1.manageOffer(0, cur2, cur1, Price{2, 1}, 4);
                a1.manageOffer(0, cur2, cur1, Price{2, 1}, 6);

                // 1000*1000/990 = 1010.1010 so buying 10 cur2 costs 11 cur1
                a1.liquidityPoolDeposit(pool12, 1000, 1000, Price{1, INT32_MAX},
                                        Price{INT32_MAX, 1});

                auto res = a2.pay(a2, cur1, 100, cur2, 10, {});
                REQUIRE(res.success().offers.size() == 1);
                REQUIRE(res.success().offers[0].type() ==
                        CLAIM_ATOM_TYPE_LIQUIDITY_POOL);
            }

            SECTION("both prices equal")
            {
                // Buying 10 cur2 costs 19 cur1
                a1.manageOffer(0, cur2, cur1, Price{1, 1}, 1);
                a1.manageOffer(0, cur2, cur1, Price{2, 1}, 9);

                // 1800*1000/990 = 1818.1818 so buying 10 cur2 costs 19 cur1
                a1.liquidityPoolDeposit(pool12, 1800, 1000, Price{1, INT32_MAX},
                                        Price{INT32_MAX, 1});

                auto res = a2.pay(a2, cur1, 100, cur2, 10, {});
                REQUIRE(res.success().offers.size() == 1);
                REQUIRE(res.success().offers[0].type() ==
                        CLAIM_ATOM_TYPE_LIQUIDITY_POOL);
            }

            SECTION("book has strictly better price")
            {
                // Buying 10 cur2 costs 20 cur1
                a1.manageOffer(0, cur2, cur1, Price{2, 1}, 4);
                a1.manageOffer(0, cur2, cur1, Price{2, 1}, 6);

                // 2000*1000/990 = 2020.2020 so buying 10 cur2 costs 21 cur1
                a1.liquidityPoolDeposit(pool12, 2000, 1000, Price{1, INT32_MAX},
                                        Price{INT32_MAX, 1});

                auto res = a2.pay(a2, cur1, 100, cur2, 10, {});
                REQUIRE(res.success().offers.size() == 2);
                REQUIRE(res.success().offers[0].type() ==
                        CLAIM_ATOM_TYPE_ORDER_BOOK);
                REQUIRE(res.success().offers[1].type() ==
                        CLAIM_ATOM_TYPE_ORDER_BOOK);
            }
        }

        SECTION("max offers to cross")
        {
            auto cur3 = makeAsset(root, "CUR3");

            auto a1 = root.create("a1", minBal(2000));
            a1.changeTrust(cur1, INT64_MAX);
            a1.changeTrust(cur2, INT64_MAX);
            a1.changeTrust(cur3, INT64_MAX);
            a1.changeTrust(share12, INT64_MAX);
            root.pay(a1, cur1, 10000);
            root.pay(a1, cur2, 10000);
            root.pay(a1, cur3, 10000);

            auto a2 = root.create("a2", minBal(2000));
            a2.changeTrust(cur1, INT64_MAX);
            a2.changeTrust(cur2, INT64_MAX);
            a2.changeTrust(cur3, INT64_MAX);
            root.pay(a2, cur1, 10000);
            root.pay(a2, cur2, 10000);
            root.pay(a2, cur3, 10000);

            auto a3 = root.create("a3", minBal(10));
            a3.changeTrust(cur1, INT64_MAX);
            a3.changeTrust(cur2, INT64_MAX);
            a3.changeTrust(cur3, INT64_MAX);
            root.pay(a3, cur1, 10000);
            root.pay(a3, cur2, 10000);
            root.pay(a3, cur3, 10000);

            for (size_t i = 0; i < 500; ++i)
            {
                a1.manageOffer(0, cur3, cur2, Price{1, 1}, 1);
                a2.manageOffer(0, cur3, cur2, Price{1, 1}, 1);
            }

            SECTION("order book succeeds when crossing limit")
            {
                a1.manageOffer(0, cur2, cur1, Price{1, 1}, 1000);
                REQUIRE_NOTHROW(a3.pay(a3, cur1, 2000, cur3, 999, {cur2}));
            }

            SECTION("order book fails when crossing one above limit")
            {
                a1.manageOffer(0, cur2, cur1, Price{1, 1}, 1000);
                REQUIRE_THROWS_AS(a3.pay(a1, cur1, 2000, cur3, 1000, {cur2}),
                                  ex_opEXCEEDED_WORK_LIMIT);
            }

            SECTION("liquidity pool succeeds when crossing limit")
            {
                a1.liquidityPoolDeposit(pool12, 100, 2000, Price{1, INT32_MAX},
                                        Price{INT32_MAX, 1});
                REQUIRE_NOTHROW(a3.pay(a3, cur1, 2000, cur3, 999, {cur2}));
            }

            SECTION("liquidity pool fails when crossing one above limit")
            {
                // Can't cross pool, no offers in book
                a1.liquidityPoolDeposit(pool12, 100, 2000, Price{1, INT32_MAX},
                                        Price{INT32_MAX, 1});
                REQUIRE_THROWS_AS(
                    a3.pay(a3, cur1, 2000, cur3, 1000, {cur2}),
                    ex_PATH_PAYMENT_STRICT_RECEIVE_TOO_FEW_OFFERS);

                // Can't cross pool, offer in book is self trade
                a3.manageOffer(0, cur2, cur1, Price{2, 1}, 1000);
                REQUIRE_THROWS_AS(
                    a3.pay(a3, cur1, 2000, cur3, 1000, {cur2}),
                    ex_PATH_PAYMENT_STRICT_RECEIVE_OFFER_CROSS_SELF);

                // Can't cross pool, offer in book exceeds work limit
                a1.manageOffer(0, cur2, cur1, Price{1, 1}, 1000);
                REQUIRE_THROWS_AS(a3.pay(a3, cur1, 2000, cur3, 1000, {cur2}),
                                  ex_opEXCEEDED_WORK_LIMIT);
            }
        }
    });
}
