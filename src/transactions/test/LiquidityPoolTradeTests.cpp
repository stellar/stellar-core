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
#include "transactions/OfferExchange.h"
#include "transactions/TransactionUtils.h"

#include "util/Logging.h"
#include "util/XDRCereal.h"

using namespace stellar;
using namespace stellar::txtest;

void
testLiquidityPoolTrading(Application& app, Asset const& cur1, Asset const& cur2)
{
    auto share12 = makeChangeTrustAssetPoolShare(
        std::min(cur1, cur2), std::max(cur1, cur2), LIQUIDITY_POOL_FEE_V18);
    auto pool12 = xdrSha256(share12.liquidityPool());

    auto minBal = [&](int32_t n) {
        return app.getLedgerManager().getLastMinBalance(n);
    };
    auto root = TestAccount::createRoot(app);

    auto checkLiquidityPool12 = [&](int64_t reserveA, int64_t reserveB,
                                    int64_t totalPoolShares,
                                    int64_t poolSharesTrustLineCount) {
        if (!(cur1 < cur2))
        {
            std::swap(reserveA, reserveB);
        }
        stellar::txtest::checkLiquidityPool(app, pool12, reserveA, reserveB,
                                            totalPoolShares,
                                            poolSharesTrustLineCount);
    };

    auto depositIntoPool12 = [&](TestAccount& acc, int64_t amountA,
                                 int64_t amountB, Price const& minPrice,
                                 Price const& maxPrice) {
        if (!(cur1 < cur2))
        {
            std::swap(amountA, amountB);
        }
        acc.liquidityPoolDeposit(pool12, amountA, amountB, minPrice, maxPrice);
    };

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
            depositIntoPool12(a1, 1000, 1000, Price{1, INT32_MAX},
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
            depositIntoPool12(a1, 1800, 1000, Price{1, INT32_MAX},
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
            depositIntoPool12(a1, 2000, 1000, Price{1, INT32_MAX},
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
        TempReduceLimitsForTesting limitUpdater(100, 10);

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

        REQUIRE(getMaxOffersToCross() == 10);
        for (size_t i = 0; i < 5; ++i)
        {
            a1.manageOffer(0, cur3, cur2, Price{1, 1}, 1);
            a2.manageOffer(0, cur3, cur2, Price{1, 1}, 1);
        }

        SECTION("order book succeeds when crossing limit")
        {
            a1.manageOffer(0, cur2, cur1, Price{1, 1}, 10);
            REQUIRE_NOTHROW(a3.pay(a3, cur1, 2000, cur3, 9, {cur2}));
        }

        SECTION("order book fails when crossing one above limit")
        {
            a1.manageOffer(0, cur2, cur1, Price{1, 1}, 10);
            REQUIRE_THROWS_AS(a3.pay(a1, cur1, 2000, cur3, 10, {cur2}),
                              ex_opEXCEEDED_WORK_LIMIT);
        }

        SECTION("liquidity pool succeeds when crossing limit")
        {
            depositIntoPool12(a1, 100, 2000, Price{1, INT32_MAX},
                              Price{INT32_MAX, 1});
            REQUIRE_NOTHROW(a3.pay(a3, cur1, 2000, cur3, 9, {cur2}));
        }

        SECTION("liquidity pool fails when crossing one above limit")
        {
            // in this section, the pool can't be crossed for cur1->cur2
            // because we've already reached the limit with cur2->cur3
            depositIntoPool12(a1, 100, 2000, Price{1, INT32_MAX},
                              Price{INT32_MAX, 1});

            // Can't cross pool, no offers in book
            REQUIRE_THROWS_AS(a3.pay(a3, cur1, 2000, cur3, 10, {cur2}),
                              ex_opEXCEEDED_WORK_LIMIT);

            // Can't cross pool, offer in book is self trade
            auto id = a3.manageOffer(0, cur2, cur1, Price{1, 1}, 10);
            REQUIRE_THROWS_AS(a3.pay(a3, cur1, 2000, cur3, 10, {cur2}),
                              ex_opEXCEEDED_WORK_LIMIT);
            a3.manageOffer(id, cur2, cur1, Price{1, 1}, 0,
                           MANAGE_OFFER_DELETED);

            // Can't cross pool, offer in book exceeds work limit
            a1.manageOffer(0, cur2, cur1, Price{1, 1}, 10);
            REQUIRE_THROWS_AS(a3.pay(a3, cur1, 2000, cur3, 10, {cur2}),
                              ex_opEXCEEDED_WORK_LIMIT);
        }
    }

    SECTION("liquidity pool charges the correct fee")
    {
        auto a1 = root.create("a1", minBal(10));
        a1.changeTrust(cur1, INT64_MAX);
        root.pay(a1, cur1, 10000);

        auto a2 = root.create("a2", minBal(10));
        a2.changeTrust(cur2, INT64_MAX);

        auto mm12 = root.create("mm12", minBal(10));
        mm12.changeTrust(cur1, INT64_MAX);
        mm12.changeTrust(cur2, INT64_MAX);
        mm12.changeTrust(share12, INT64_MAX);
        root.pay(mm12, cur1, 10000);
        root.pay(mm12, cur2, 10000);
        depositIntoPool12(mm12, 1000, 2000, Price{1, INT32_MAX},
                          Price{INT32_MAX, 1});

        // Need to send 1000 to receive 1000 (note that this swaps the
        // reserves). But the send amount is _after fees_, and observe that
        // - 0.3% of 1003 is 999.91 < 1000
        // - 0.3% of 1004 is 1000.98 > 1000
        // So the fee is just above 3, and rounding up makes it 4.

        SECTION("strict send")
        {
            REQUIRE_THROWS_AS(
                a1.pathPaymentStrictSend(a2, cur1, 1004, cur2, 1001, {}),
                ex_PATH_PAYMENT_STRICT_SEND_UNDER_DESTMIN);
            a1.pathPaymentStrictSend(a2, cur1, 1004, cur2, 1000, {});
        }

        SECTION("strict receive")
        {
            REQUIRE_THROWS_AS(a1.pay(a2, cur1, 1003, cur2, 1000, {}),
                              ex_PATH_PAYMENT_STRICT_RECEIVE_OVER_SENDMAX);
            a1.pay(a2, cur1, 1004, cur2, 1000, {});
        }
    }

    SECTION("trade fails due to excess reserves")
    {
        auto a1 = root.create("a1", minBal(10));
        a1.changeTrust(cur1, INT64_MAX);
        root.pay(a1, cur1, 10000);

        auto a2 = root.create("a2", minBal(10));
        a2.changeTrust(cur2, INT64_MAX);

        auto mm12 = root.create("mm12", minBal(10));
        mm12.changeTrust(cur1, INT64_MAX);
        mm12.changeTrust(cur2, INT64_MAX);
        mm12.changeTrust(share12, INT64_MAX);
        root.pay(mm12, cur1, INT64_MAX);
        root.pay(mm12, cur2, INT64_MAX);
        depositIntoPool12(mm12, INT64_MAX - 10, INT64_MAX - 10,
                          Price{1, INT32_MAX}, Price{INT32_MAX, 1});

        SECTION("strict send")
        {
            REQUIRE_THROWS_AS(
                a1.pathPaymentStrictSend(a2, cur1, 11, cur2, 9, {}),
                ex_PATH_PAYMENT_STRICT_SEND_TOO_FEW_OFFERS);
            a1.pathPaymentStrictSend(a2, cur1, 10, cur2, 9, {});
        }

        SECTION("strict receive")
        {
            REQUIRE_THROWS_AS(a1.pay(a2, cur1, 11, cur2, 10, {}),
                              ex_PATH_PAYMENT_STRICT_RECEIVE_TOO_FEW_OFFERS);
            a1.pay(a2, cur1, 10, cur2, 9, {});
        }
    }

    SECTION("payment through empty liquidity pools")
    {
        auto cur3 = makeAsset(root, "CUR3");
        auto share23 =
            makeChangeTrustAssetPoolShare(cur2, cur3, LIQUIDITY_POOL_FEE_V18);

        auto a1 = root.create("a1", minBal(10));
        a1.changeTrust(cur1, INT64_MAX);
        root.pay(a1, cur1, 10000);

        auto a2 = root.create("a2", minBal(10));
        a2.changeTrust(cur2, INT64_MAX);
        a2.changeTrust(cur3, INT64_MAX);

        auto mm12 = root.create("mm12", minBal(10));
        mm12.changeTrust(cur1, INT64_MAX);
        mm12.changeTrust(cur2, INT64_MAX);
        mm12.changeTrust(share12, INT64_MAX);
        root.pay(mm12, cur1, INT64_MAX);
        root.pay(mm12, cur2, INT64_MAX);

        auto mm23 = root.create("mm23", minBal(10));
        mm23.changeTrust(cur2, INT64_MAX);
        mm23.changeTrust(cur3, INT64_MAX);
        mm23.changeTrust(share23, INT64_MAX);
        root.pay(mm23, cur2, INT64_MAX);
        root.pay(mm23, cur3, INT64_MAX);

        SECTION("strict send")
        {
            REQUIRE_THROWS_AS(
                a1.pathPaymentStrictSend(a2, cur1, 100, cur2, 1, {}),
                ex_PATH_PAYMENT_STRICT_SEND_TOO_FEW_OFFERS);
            REQUIRE_THROWS_AS(
                a1.pathPaymentStrictSend(a2, cur1, 100, cur3, 1, {cur2}),
                ex_PATH_PAYMENT_STRICT_SEND_TOO_FEW_OFFERS);
        }

        SECTION("strict receive")
        {
            REQUIRE_THROWS_AS(a1.pay(a2, cur1, INT64_MAX, cur2, 100, {}),
                              ex_PATH_PAYMENT_STRICT_RECEIVE_TOO_FEW_OFFERS);
            REQUIRE_THROWS_AS(a1.pay(a2, cur1, INT64_MAX, cur3, 100, {cur2}),
                              ex_PATH_PAYMENT_STRICT_RECEIVE_TOO_FEW_OFFERS);
        }
    }

    SECTION("payment through pool that yields nothing")
    {
        auto a1 = root.create("a1", minBal(10));
        a1.changeTrust(cur1, INT64_MAX);
        root.pay(a1, cur1, 10000);

        auto a2 = root.create("a2", minBal(10));
        a2.changeTrust(cur2, INT64_MAX);

        auto mm12 = root.create("mm12", minBal(10));
        mm12.changeTrust(cur1, INT64_MAX);
        mm12.changeTrust(cur2, INT64_MAX);
        mm12.changeTrust(share12, INT64_MAX);
        root.pay(mm12, cur1, INT64_MAX);
        root.pay(mm12, cur2, INT64_MAX);
        depositIntoPool12(mm12, 1000, 1000, Price{1, INT32_MAX},
                          Price{INT32_MAX, 1});

        SECTION("strict send")
        {
            // selling 1 to a pool always yields 0, which means to use the
            // order book instead. the order book is empty, so the result
            // is TOO_FEW_OFFERS.
            REQUIRE_THROWS_AS(
                a1.pathPaymentStrictSend(a2, cur1, 1, cur2, 1, {}),
                ex_PATH_PAYMENT_STRICT_SEND_TOO_FEW_OFFERS);
        }

        // Not possible with PathPaymentStrictReceive.
    }

    SECTION("payment through pool after offer that yields nothing")
    {
        auto cur3 = makeAsset(root, "CUR3");
        auto share23 =
            makeChangeTrustAssetPoolShare(cur2, cur3, LIQUIDITY_POOL_FEE_V18);
        auto pool23 = xdrSha256(share23.liquidityPool());

        auto a1 = root.create("a1", minBal(10));
        a1.changeTrust(cur1, INT64_MAX);
        root.pay(a1, cur1, 10000);

        auto a2 = root.create("a2", minBal(10));
        a2.changeTrust(cur3, INT64_MAX);

        auto mm12 = root.create("mm12", minBal(10));
        mm12.changeTrust(cur1, INT64_MAX);
        mm12.changeTrust(cur2, INT64_MAX);
        mm12.changeTrust(share12, INT64_MAX);
        root.pay(mm12, cur1, 10000);
        root.pay(mm12, cur2, 10000);
        mm12.manageOffer(0, cur2, cur1, Price{2, 1}, 10);

        auto mm23 = root.create("mm23", minBal(10));
        mm23.changeTrust(cur2, INT64_MAX);
        mm23.changeTrust(cur3, INT64_MAX);
        mm23.changeTrust(share23, INT64_MAX);
        root.pay(mm23, cur2, INT64_MAX);
        root.pay(mm23, cur3, INT64_MAX);
        mm23.liquidityPoolDeposit(pool23, 100, 100, Price{1, INT32_MAX},
                                  Price{INT32_MAX, 1});

        SECTION("strict send")
        {
            // destination minimum is 1, so clearly selling 1 CUR1 to the
            // pool yields 0 CUR2.
            REQUIRE_THROWS_AS(
                a1.pathPaymentStrictSend(a2, cur1, 1, cur2, 1, {}),
                ex_PATH_PAYMENT_STRICT_SEND_UNDER_DESTMIN);

            // the first hop yields 0, so the second hop has 0 as input.
            // there are always enough offers to send 0 through the order
            // book, so we get UNDER_DESTMIN instead of TOO_FEW_OFFERS.
            REQUIRE_THROWS_AS(
                a1.pathPaymentStrictSend(a2, cur1, 1, cur3, 1, {cur2}),
                ex_PATH_PAYMENT_STRICT_SEND_UNDER_DESTMIN);
        }

        // Not possible with PathPaymentStrictReceive.
    }

    SECTION("payment through a pool that the sender participates in")
    {
        auto a1 = root.create("a1", minBal(10));
        a1.changeTrust(cur1, INT64_MAX);
        a1.changeTrust(cur2, INT64_MAX);
        a1.changeTrust(share12, INT64_MAX);
        root.pay(a1, cur1, 10000);
        root.pay(a1, cur2, 10000);
        depositIntoPool12(a1, 1000, 1000, Price{1, INT32_MAX},
                          Price{INT32_MAX, 1});

        auto a2 = root.create("a2", minBal(10));
        a2.changeTrust(cur2, INT64_MAX);

        SECTION("strict send")
        {
            a1.pathPaymentStrictSend(a2, cur1, 10, cur2, 9, {});
            checkLiquidityPool12(1010, 991, 1000, 1);
        }

        SECTION("strict receive")
        {
            a1.pay(a2, cur1, 10, cur2, 9, {});
            checkLiquidityPool12(1010, 991, 1000, 1);
        }
    }

    SECTION("payment through a pool that the destination participates in")
    {
        auto a1 = root.create("a1", minBal(10));
        a1.changeTrust(cur1, INT64_MAX);
        root.pay(a1, cur1, 10000);

        auto a2 = root.create("a2", minBal(10));
        a2.changeTrust(cur1, INT64_MAX);
        a2.changeTrust(cur2, INT64_MAX);
        a2.changeTrust(share12, INT64_MAX);
        root.pay(a2, cur1, 10000);
        root.pay(a2, cur2, 10000);
        depositIntoPool12(a2, 1000, 1000, Price{1, INT32_MAX},
                          Price{INT32_MAX, 1});

        SECTION("strict send")
        {
            a1.pathPaymentStrictSend(a2, cur1, 10, cur2, 9, {});
            checkLiquidityPool12(1010, 991, 1000, 1);
        }

        SECTION("strict receive")
        {
            a1.pay(a2, cur1, 10, cur2, 9, {});
            checkLiquidityPool12(1010, 991, 1000, 1);
        }
    }

    SECTION("payment through a pool that a market maker participates in")
    {
        auto a1 = root.create("a1", minBal(10));
        a1.changeTrust(cur1, INT64_MAX);
        root.pay(a1, cur1, 10000);

        auto a2 = root.create("a2", minBal(10));
        a2.changeTrust(cur2, INT64_MAX);

        auto mm12 = root.create("mm12", minBal(10));
        mm12.changeTrust(cur1, INT64_MAX);
        mm12.changeTrust(cur2, INT64_MAX);
        mm12.changeTrust(share12, INT64_MAX);
        root.pay(mm12, cur1, 10000);
        root.pay(mm12, cur2, 10000);
        mm12.manageOffer(0, cur2, cur1, Price{10, 9}, 10);
        depositIntoPool12(mm12, 1000, 1000, Price{1, INT32_MAX},
                          Price{INT32_MAX, 1});

        SECTION("strict send")
        {
            a1.pathPaymentStrictSend(a2, cur1, 10, cur2, 9, {});
            checkLiquidityPool12(1010, 991, 1000, 1);
        }

        SECTION("strict receive")
        {
            a1.pay(a2, cur1, 10, cur2, 9, {});
            checkLiquidityPool12(1010, 991, 1000, 1);
        }
    }

    SECTION("order book is better, but there is a self-trade")
    {
        auto a1 = root.create("a1", minBal(10));
        a1.changeTrust(cur1, INT64_MAX);
        a1.changeTrust(cur2, INT64_MAX);
        a1.changeTrust(share12, INT64_MAX);
        root.pay(a1, cur1, 10000);
        root.pay(a1, cur2, 10000);
        depositIntoPool12(a1, 1000, 1000, Price{1, INT32_MAX},
                          Price{INT32_MAX, 1});

        auto a2 = root.create("a2", minBal(10));
        a2.changeTrust(cur2, INT64_MAX);

        auto mm12 = root.create("mm12", minBal(10));
        mm12.changeTrust(cur1, INT64_MAX);
        mm12.changeTrust(cur2, INT64_MAX);
        mm12.changeTrust(share12, INT64_MAX);
        root.pay(mm12, cur1, 10000);
        root.pay(mm12, cur2, 10000);

        SECTION("strict send")
        {
            SECTION("no self trade")
            {
                mm12.manageOffer(0, cur2, cur1, Price{1, 1}, 10);
                a1.pathPaymentStrictSend(a2, cur1, 10, cur2, 9, {});
                checkLiquidityPool12(1000, 1000, 1000, 2);
            }

            SECTION("self trade")
            {
                a1.manageOffer(0, cur2, cur1, Price{1, 1}, 10);
                a1.pathPaymentStrictSend(a2, cur1, 10, cur2, 9, {});
                checkLiquidityPool12(1010, 991, 1000, 2);
            }
        }

        SECTION("strict receive")
        {
            SECTION("no self trade")
            {
                mm12.manageOffer(0, cur2, cur1, Price{1, 1}, 10);
                a1.pay(a2, cur1, 10, cur2, 9, {});
                checkLiquidityPool12(1000, 1000, 1000, 2);
            }

            SECTION("self trade")
            {
                a1.manageOffer(0, cur2, cur1, Price{1, 1}, 10);
                a1.pay(a2, cur1, 10, cur2, 9, {});
                checkLiquidityPool12(1010, 991, 1000, 2);
            }
        }
    }

    SECTION("payment would receive more than the reserve")
    {
        auto a1 = root.create("a1", minBal(10));
        a1.changeTrust(cur1, INT64_MAX);
        root.pay(a1, cur1, INT64_MAX);

        auto a2 = root.create("a2", minBal(10));
        a2.changeTrust(cur2, INT64_MAX);

        auto mm12 = root.create("mm12", minBal(10));
        mm12.changeTrust(cur1, INT64_MAX);
        mm12.changeTrust(cur2, INT64_MAX);
        mm12.changeTrust(share12, INT64_MAX);
        root.pay(mm12, cur1, 10000);
        root.pay(mm12, cur2, 10000);
        depositIntoPool12(mm12, 1000, 1000, Price{1, INT32_MAX},
                          Price{INT32_MAX, 1});

        // This isn't possible with PathPaymentStrictSend.

        SECTION("strict receive")
        {
            // Strictly above the reserve
            REQUIRE_THROWS_AS(a1.pay(a2, cur1, INT64_MAX, cur2, 1001, {}),
                              ex_PATH_PAYMENT_STRICT_RECEIVE_TOO_FEW_OFFERS);

            // At the reserve
            REQUIRE_THROWS_AS(a1.pay(a2, cur1, INT64_MAX, cur2, 1000, {}),
                              ex_PATH_PAYMENT_STRICT_RECEIVE_TOO_FEW_OFFERS);

            // Below the reserve
            a1.pay(a2, cur1, INT64_MAX, cur2, 999, {});
            checkLiquidityPool12(1003007, 1, 1000, 1);
        }
    }

    SECTION("payment into pool would be larger than INT64_MAX")
    {
        auto a1 = root.create("a1", minBal(10));
        a1.changeTrust(cur1, INT64_MAX);
        root.pay(a1, cur1, INT64_MAX);

        auto a2 = root.create("a2", minBal(10));
        a2.changeTrust(cur2, INT64_MAX);

        auto mm12 = root.create("mm12", minBal(10));
        mm12.changeTrust(cur1, INT64_MAX);
        mm12.changeTrust(cur2, INT64_MAX);
        mm12.changeTrust(share12, INT64_MAX);
        root.pay(mm12, cur1, INT64_MAX / 2);
        root.pay(mm12, cur2, INT64_MAX / 2);
        depositIntoPool12(mm12, INT64_MAX / 2, INT64_MAX / 2,
                          Price{1, INT32_MAX}, Price{INT32_MAX, 1});

        // This isn't possible with PathPaymentStrictSend.

        SECTION("strict receive")
        {
            REQUIRE_THROWS_AS(
                a1.pay(a2, cur1, INT64_MAX, cur2, INT64_MAX / 2 - 1, {}),
                ex_PATH_PAYMENT_STRICT_RECEIVE_TOO_FEW_OFFERS);
        }
    }

    SECTION("cross the same pair twice in the same direction")
    {
        auto cur3 = makeAsset(root, "CUR3");

        auto a1 = root.create("a1", minBal(10));
        a1.changeTrust(cur1, INT64_MAX);
        root.pay(a1, cur1, 10000);

        auto a2 = root.create("a2", minBal(10));
        a2.changeTrust(cur2, INT64_MAX);

        auto mm12 = root.create("mm12", minBal(10));
        mm12.changeTrust(cur1, INT64_MAX);
        mm12.changeTrust(cur2, INT64_MAX);
        mm12.changeTrust(share12, INT64_MAX);
        root.pay(mm12, cur1, 10000);
        root.pay(mm12, cur2, 10000);

        auto mm23 = root.create("mm23", minBal(10));
        mm23.changeTrust(cur2, INT64_MAX);
        mm23.changeTrust(cur3, INT64_MAX);
        root.pay(mm23, cur2, 10000);
        root.pay(mm23, cur3, 10000);
        mm23.manageOffer(0, cur3, cur2, Price{1, 1}, 1000);

        auto mm31 = root.create("mm31", minBal(10));
        mm31.changeTrust(cur1, INT64_MAX);
        mm31.changeTrust(cur3, INT64_MAX);
        root.pay(mm31, cur1, 10000);
        root.pay(mm31, cur3, 10000);
        mm31.manageOffer(0, cur1, cur3, Price{1, 1}, 1000);

        SECTION("liquidity pool both times")
        {
            mm12.manageOffer(0, cur2, cur1, Price{1, 1}, 1000);
            depositIntoPool12(mm12, 1000, 2000, Price{1, INT32_MAX},
                              Price{INT32_MAX, 1});

            SECTION("strict send")
            {
                // Processing order cur1/cur2, cur2/cur3, cur3/cur1,
                // cur1/cur2
                //
                // The first pass sells 100 CUR1 leaving pool12 with
                // reserves 1100, 1819
                //
                // The second pass sells 181 CUR1 leaving pool12 with
                // reserves 1281, 1563
                REQUIRE_THROWS_AS(a1.pathPaymentStrictSend(a2, cur1, 100, cur2,
                                                           257,
                                                           {cur2, cur3, cur1}),
                                  ex_PATH_PAYMENT_STRICT_SEND_UNDER_DESTMIN);
                a1.pathPaymentStrictSend(a2, cur1, 100, cur2, 256,
                                         {cur2, cur3, cur1});
                checkLiquidityPool12(1281, 1563, 1414, 1);
            }

            SECTION("strict receive")
            {
                // Processing order cur1/cur2, cur3/cur1, cur2/cur3,
                // cur1/cur2
                //
                // The first pass (which is actually the second pass from
                // source to destination) buys 256 CUR2 leaving pool12 with
                // reserves 1148, 1744
                //
                // The second pass (which is actually the first pass from
                // source to destination) buys 148 CUR2 leaving pool12 with
                // reserves 1255, 1596
                REQUIRE_THROWS_AS(
                    a1.pay(a2, cur1, 106, cur2, 256, {cur2, cur3, cur1}),
                    ex_PATH_PAYMENT_STRICT_RECEIVE_OVER_SENDMAX);
                a1.pay(a2, cur1, 107, cur2, 256, {cur2, cur3, cur1});
                checkLiquidityPool12(1255, 1596, 1414, 1);
            }
        }

        SECTION("liquidity pool both times, fails on the second hop")
        {
            SECTION("strict send")
            {
                // Need higher balances for this test
                root.pay(mm12, cur1, INT64_MAX - 10000);
                root.pay(mm12, cur2, INT64_MAX - 10000);

                depositIntoPool12(mm12, INT64_MAX - 1000, INT64_MAX - 1000,
                                  Price{1, INT32_MAX}, Price{INT32_MAX, 1});

                // Processing order cur1/cur2, cur2/cur3, cur3/cur1,
                // cur1/cur2
                //
                // The first pass sells 501 CUR1 leaving pool12 with
                // reserves INT64_MAX + 501, INT64_MAX - 1499
                //
                // The second pass sells 499 CUR1 leaving pool12 with
                // reserves INT64_MAX, INT64_MAX - 1996
                REQUIRE_THROWS_AS(a1.pathPaymentStrictSend(a2, cur1, 502, cur2,
                                                           1,
                                                           {cur2, cur3, cur1}),
                                  ex_PATH_PAYMENT_STRICT_SEND_TOO_FEW_OFFERS);
                a1.pathPaymentStrictSend(a2, cur1, 501, cur2, 1,
                                         {cur2, cur3, cur1});
                checkLiquidityPool12(INT64_MAX, INT64_MAX - 1996,
                                     INT64_MAX - 1000, 1);
            }

            SECTION("strict receive")
            {
                // Need higher balances for this test
                root.pay(a1, cur1, INT64_MAX - 10000);

                depositIntoPool12(mm12, 1000, 2000, Price{1, INT32_MAX},
                                  Price{INT32_MAX, 1});

                // Processing order cur1/cur2, cur3/cur1, cur2/cur3,
                // cur1/cur2
                //
                // The first pass (which is actually the second pass from
                // source to destination) buys 998 CUR2 leaving pool12 with
                // reserves 2000, 1002
                //
                // The second pass (which is actually the first pass from
                // source to destination) buys 1000 CUR2 leaving pool12 with
                // reserves 1005010, 2
                REQUIRE_THROWS_AS(
                    a1.pay(a2, cur1, INT64_MAX, cur2, 999, {cur2, cur3, cur1}),
                    ex_PATH_PAYMENT_STRICT_RECEIVE_TOO_FEW_OFFERS);
                a1.pay(a2, cur1, INT64_MAX, cur2, 998, {cur2, cur3, cur1});
                checkLiquidityPool12(1005010, 2, 1414, 1);
            }
        }

        SECTION("liquidity pool once, order book once, strict send "
                "prefers liquidity pool on first hop")
        {
            mm12.manageOffer(0, cur2, cur1, Price{11, 20}, 1000);
            depositIntoPool12(mm12, 1000, 2000, Price{1, INT32_MAX},
                              Price{INT32_MAX, 1});

            SECTION("strict send")
            {
                // Processing order cur1/cur2, cur2/cur3, cur3/cur1,
                // cur1/cur2
                //
                // The first pass sells 100 CUR1 leaving pool12 with
                // reserves 1100, 1819
                //
                // The second pass sells 181 CUR1 to the order book
                REQUIRE_THROWS_AS(a1.pathPaymentStrictSend(a2, cur1, 100, cur2,
                                                           330,
                                                           {cur2, cur3, cur1}),
                                  ex_PATH_PAYMENT_STRICT_SEND_UNDER_DESTMIN);
                a1.pathPaymentStrictSend(a2, cur1, 100, cur2, 329,
                                         {cur2, cur3, cur1});
                checkLiquidityPool12(1100, 1819, 1414, 1);
            }

            SECTION("strict receive")
            {
                // Processing order cur1/cur2, cur3/cur1, cur2/cur3,
                // cur1/cur2
                //
                // The first pass (which is actually the second pass from
                // source to destination) buys 329 CUR2 from the order book
                //
                // The second pass (which is actually the first pass from
                // source to destination) buys 181 CUR2 leaving pool12 with
                // reserves 1100, 1819
                REQUIRE_THROWS_AS(
                    a1.pay(a2, cur1, 99, cur2, 329, {cur2, cur3, cur1}),
                    ex_PATH_PAYMENT_STRICT_RECEIVE_OVER_SENDMAX);
                a1.pay(a2, cur1, 100, cur2, 329, {cur2, cur3, cur1});
                checkLiquidityPool12(1100, 1819, 1414, 1);
            }
        }

        SECTION("liquidity pool once, order book once, strict send "
                "indifferent on first hop")
        {
            mm12.manageOffer(0, cur2, cur1, Price{100, 181}, 1000);
            depositIntoPool12(mm12, 1000, 2000, Price{1, INT32_MAX},
                              Price{INT32_MAX, 1});

            SECTION("strict send")
            {
                // Processing order cur1/cur2, cur2/cur3, cur3/cur1,
                // cur1/cur2
                //
                // The first pass sells 100 CUR1 leaving pool12 with
                // reserves 1100, 1819
                //
                // The second pass sells 181 CUR1 to the order book
                REQUIRE_THROWS_AS(a1.pathPaymentStrictSend(a2, cur1, 100, cur2,
                                                           328,
                                                           {cur2, cur3, cur1}),
                                  ex_PATH_PAYMENT_STRICT_SEND_UNDER_DESTMIN);
                a1.pathPaymentStrictSend(a2, cur1, 100, cur2, 327,
                                         {cur2, cur3, cur1});
                checkLiquidityPool12(1100, 1819, 1414, 1);
            }

            SECTION("strict receive")
            {
                // Processing order cur1/cur2, cur3/cur1, cur2/cur3,
                // cur1/cur2
                //
                // The first pass (which is actually the second pass from
                // source to destination) buys 327 CUR2 from the order book
                //
                // The second pass (which is actually the first pass from
                // source to destination) buys 181 CUR2 leaving pool12 with
                // reserves 1100, 1819
                REQUIRE_THROWS_AS(
                    a1.pay(a2, cur1, 99, cur2, 327, {cur2, cur3, cur1}),
                    ex_PATH_PAYMENT_STRICT_RECEIVE_OVER_SENDMAX);
                a1.pay(a2, cur1, 100, cur2, 327, {cur2, cur3, cur1});
                checkLiquidityPool12(1100, 1819, 1414, 1);
            }
        }

        SECTION("liquidity pool once, order book once, strict send "
                "prefers order book on first hop")
        {
            // Enough to do this as the first pass for both operations, but
            // not enough to do this as the second pass for either operation
            mm12.manageOffer(0, cur2, cur1, Price{1, 2}, 350);
            depositIntoPool12(mm12, 1000, 2000, Price{1, INT32_MAX},
                              Price{INT32_MAX, 1});

            SECTION("strict send")
            {
                // Processing order cur1/cur2, cur2/cur3, cur3/cur1,
                // cur1/cur2
                //
                // The first pass sells 100 CUR1 to the order book
                //
                // The second pass sells 200 CUR1 leaving pool12 with
                // reserves 1200, 1668
                REQUIRE_THROWS_AS(a1.pathPaymentStrictSend(a2, cur1, 100, cur2,
                                                           333,
                                                           {cur2, cur3, cur1}),
                                  ex_PATH_PAYMENT_STRICT_SEND_UNDER_DESTMIN);
                a1.pathPaymentStrictSend(a2, cur1, 100, cur2, 332,
                                         {cur2, cur3, cur1});
                checkLiquidityPool12(1200, 1668, 1414, 1);
            }

            SECTION("strict receive")
            {
                // Processing order cur1/cur2, cur3/cur1, cur2/cur3,
                // cur1/cur2
                //
                // The first pass (which is actually the second pass from
                // source to destination) buys 332 CUR2 from the order book
                //
                // The second pass (which is actually the first pass from
                // source to destination) buys 166 CUR2 leaving pool12 with
                // reserves 1091, 1834
                REQUIRE_THROWS_AS(
                    a1.pay(a2, cur1, 90, cur2, 332, {cur2, cur3, cur1}),
                    ex_PATH_PAYMENT_STRICT_RECEIVE_OVER_SENDMAX);
                a1.pay(a2, cur1, 91, cur2, 332, {cur2, cur3, cur1});
                checkLiquidityPool12(1091, 1834, 1414, 1);
            }
        }

        SECTION("order book both times")
        {
            mm12.manageOffer(0, cur2, cur1, Price{1, 2}, 1000);
            depositIntoPool12(mm12, 1000, 2000, Price{1, INT32_MAX},
                              Price{INT32_MAX, 1});

            SECTION("strict send")
            {
                // Processing order cur1/cur2, cur2/cur3, cur3/cur1,
                // cur1/cur2
                //
                // The first pass sells 100 CUR1 to the order book
                //
                // The second pass sells 200 CUR1 to the order book
                REQUIRE_THROWS_AS(a1.pathPaymentStrictSend(a2, cur1, 100, cur2,
                                                           401,
                                                           {cur2, cur3, cur1}),
                                  ex_PATH_PAYMENT_STRICT_SEND_UNDER_DESTMIN);
                a1.pathPaymentStrictSend(a2, cur1, 100, cur2, 400,
                                         {cur2, cur3, cur1});
                checkLiquidityPool12(1000, 2000, 1414, 1);
            }

            SECTION("strict receive")
            {
                // Processing order cur1/cur2, cur3/cur1, cur2/cur3,
                // cur1/cur2
                //
                // The first pass (which is actually the second pass from
                // source to destination) buys 400 CUR2 from the order book
                //
                // The second pass (which is actually the first pass from
                // source to destination) buys 200 CUR2 from the order book
                REQUIRE_THROWS_AS(
                    a1.pay(a2, cur1, 99, cur2, 400, {cur2, cur3, cur1}),
                    ex_PATH_PAYMENT_STRICT_RECEIVE_OVER_SENDMAX);
                a1.pay(a2, cur1, 100, cur2, 400, {cur2, cur3, cur1});
                checkLiquidityPool12(1000, 2000, 1414, 1);
            }
        }
    }

    SECTION("cross the same pair twice in opposite directions")
    {
        auto a1 = root.create("a1", minBal(10));
        a1.changeTrust(cur1, INT64_MAX);
        root.pay(a1, cur1, 10000);

        auto a2 = root.create("a2", minBal(10));
        a2.changeTrust(cur1, INT64_MAX);

        auto mm12 = root.create("mm12", minBal(10));
        mm12.changeTrust(cur1, INT64_MAX);
        mm12.changeTrust(cur2, INT64_MAX);
        mm12.changeTrust(share12, INT64_MAX);
        root.pay(mm12, cur1, 10000);
        root.pay(mm12, cur2, 10000);

        SECTION("liquidity pool both times")
        {
            depositIntoPool12(mm12, 1000, 2000, Price{1, INT32_MAX},
                              Price{INT32_MAX, 1});

            SECTION("strict send")
            {
                // Processing order cur1/cur2, cur2/cur1
                //
                // The first pass sells 100 CUR1 leaving pool12 with
                // reserves 1100, 1819
                //
                // The second pass sells 181 CUR2 leaving pool12 with
                // reserves 1001, 2000
                REQUIRE_THROWS_AS(
                    a1.pathPaymentStrictSend(a2, cur1, 100, cur1, 100, {cur2}),
                    ex_PATH_PAYMENT_STRICT_SEND_UNDER_DESTMIN);
                a1.pathPaymentStrictSend(a2, cur1, 100, cur1, 99, {cur2});
                checkLiquidityPool12(1001, 2000, 1414, 1);
            }

            SECTION("strict receive")
            {
                // Processing order cur1/cur2, cur2/cur1
                //
                // The first pass (which is actually the first pass from
                // source to destination) buys 99 CUR1 leaving pool12 with
                // reserves 901, 2221
                //
                // The second pass (which is actually the first pass from
                // source to destination) buys 221 CUR2 leaving pool12 with
                // reserves 1001, 2000
                REQUIRE_THROWS_AS(a1.pay(a2, cur1, 100, cur1, 100, {cur2}),
                                  ex_PATH_PAYMENT_STRICT_RECEIVE_OVER_SENDMAX);
                a1.pay(a2, cur1, 100, cur1, 99, {cur2});
                checkLiquidityPool12(1001, 2000, 1414, 1);
            }
        }
    }
}

TEST_CASE_VERSIONS("liquidity pool trade", "[tx][liquiditypool]")
{
    VirtualClock clock;
    auto app = createTestApplication(
        clock, getTestConfig(0, Config::TESTDB_IN_MEMORY));

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

        SECTION("CUR1, CUR2")
        {
            testLiquidityPoolTrading(*app, cur1, cur2);
        }

        SECTION("CUR2, CUR1")
        {
            testLiquidityPoolTrading(*app, cur2, cur1);
        }
    });
}

static void
testGetPoolID(Asset const& x, Asset const& y, std::string const& hex)
{
    int64_t const feeBps = LIQUIDITY_POOL_FEE_V18;

    REQUIRE(x < y);
    if (hex.empty())
    {
        CLOG_ERROR(Tx, "{}", binToHex(getPoolID(x, y, feeBps)));
    }
    else
    {
        REQUIRE(getPoolID(x, y, feeBps) == hexToBin256(hex));
        REQUIRE(getPoolID(y, x, feeBps) == hexToBin256(hex));
    }
}

static Asset
makeAsset(std::string const& code, AccountID const& issuer)
{
    REQUIRE(!code.empty());
    REQUIRE(code.size() <= 12);

    Asset asset;
    if (code.size() <= 4)
    {
        asset.type(ASSET_TYPE_CREDIT_ALPHANUM4);
        strToAssetCode(asset.alphaNum4().assetCode, code);
        asset.alphaNum4().issuer = issuer;
    }
    else
    {
        asset.type(ASSET_TYPE_CREDIT_ALPHANUM12);
        strToAssetCode(asset.alphaNum12().assetCode, code);
        asset.alphaNum12().issuer = issuer;
    }
    return asset;
}

TEST_CASE("liquidity pool id", "[tx][liquiditypool]")
{
    AccountID acc1(PUBLIC_KEY_TYPE_ED25519);
    acc1.ed25519() = hexToBin256("0123456789abcdef0123456789abcdef"
                                 "0123456789abcdef0123456789abcdef");

    AccountID acc2(PUBLIC_KEY_TYPE_ED25519);
    acc2.ed25519() = hexToBin256("abcdef0123456789abcdef0123456789"
                                 "abcdef0123456789abcdef0123456789");

    // NATIVE and ALPHANUM4 (short and full length)
    testGetPoolID(
        makeNativeAsset(), makeAsset("AbC", acc1),
        "c17f36fbd210e43dca1cda8edc5b6c0f825fcb72b39f0392fd6309844d77ff7d");
    testGetPoolID(
        makeNativeAsset(), makeAsset("AbCd", acc1),
        "80e0c5dc79ed76bb7e63681f6456136762f0d01ede94bb379dbc793e66db35e6");

    // NATIVE and ALPHANUM12 (short and full length)
    testGetPoolID(
        makeNativeAsset(), makeAsset("AbCdEfGhIjK", acc1),
        "d2306c6e8532f99418e9d38520865e1c1059cddb6793da3cc634224f2ffb5bd4");
    testGetPoolID(
        makeNativeAsset(), makeAsset("AbCdEfGhIjKl", acc1),
        "807e9e66653b5fda4dd4e672ff64a929fc5fdafe152eeadc07bb460c4849d711");

    // ALPHANUM4 and ALPHANUM4 (short and full length)
    testGetPoolID(
        makeAsset("AbC", acc1), makeAsset("aBc", acc1),
        "0239013ab016985fc3ab077d165a9b21b822efa013fdd422381659e76aec505b");
    testGetPoolID(
        makeAsset("AbCd", acc1), makeAsset("aBc", acc1),
        "cadb490d15b4333890377cd17400acf7681e14d6d949869ffa1fbbad7a6d2fde");
    testGetPoolID(
        makeAsset("AbC", acc1), makeAsset("aBcD", acc1),
        "a938f8f346f3aff41d2e03b05137ef1955a723861802a4042f51f0f816e0db36");
    testGetPoolID(
        makeAsset("AbCd", acc1), makeAsset("aBcD", acc1),
        "c89646bb6db726bfae784ab66041abbf54747cf4b6b16dff2a5c05830ad9c16b");

    // ALPHANUM12 and ALPHANUM12 (short and full length)
    testGetPoolID(
        makeAsset("AbCdEfGhIjK", acc1), makeAsset("aBcDeFgHiJk", acc1),
        "88dc054dd0f8146bac0e691095ce2b90cd902b499761d22b1c94df120ca0b060");
    testGetPoolID(
        makeAsset("AbCdEfGhIjKl", acc1), makeAsset("aBcDeFgHiJk", acc1),
        "09672910d891e658219d2f33a8885a542b2a5a09e9f486461201bd278a3e92a4");
    testGetPoolID(
        makeAsset("AbCdEfGhIjK", acc1), makeAsset("aBcDeFgHiJkl", acc1),
        "63501addf8a5a6522eac996226069190b5226c71cfdda22347022418af1948a0");
    testGetPoolID(
        makeAsset("AbCdEfGhIjKl", acc1), makeAsset("aBcDeFgHiJkl", acc1),
        "e851197a0148e949bdc03d52c53821b9afccc0fadfdc41ae01058c14c252e03b");

    // ALPHANUM4 same code different issuer (short and full length)
    testGetPoolID(
        makeAsset("aBc", acc1), makeAsset("aBc", acc2),
        "5d7188454299529856586e81ea385d2c131c6afdd9d58c82e9aa558c16522fea");
    testGetPoolID(
        makeAsset("aBcD", acc1), makeAsset("aBcD", acc2),
        "00d152f5f6b7e46eaf558576512207ea835a332f17ca777fba3cb835ef7dc1ef");

    // ALPHANUM12 same code different issuer (short and full length)
    testGetPoolID(
        makeAsset("aBcDeFgHiJk", acc1), makeAsset("aBcDeFgHiJk", acc2),
        "cad65154300f087e652981fa5f76aa469b43ad53e9a5d348f1f93da57193d022");
    testGetPoolID(
        makeAsset("aBcDeFgHiJkL", acc1), makeAsset("aBcDeFgHiJkL", acc2),
        "93fa82ecaabe987461d1e3c8e0fd6510558b86ac82a41f7c70b112281be90c71");

    // ALPHANUM4 before ALPHANUM12 (short and full length) doesn't depend on
    // issuer or code
    testGetPoolID(
        makeAsset("aBc", acc1), makeAsset("aBcDeFgHiJk", acc2),
        "c0d4c87bbaade53764b904fde2901a0353af437e9d3a976f1252670b85a36895");
    testGetPoolID(
        makeAsset("aBcD", acc1), makeAsset("aBcDeFgHiJk", acc2),
        "1ee5aa0f0e6b8123c2da6592389481f64d816bfe3c3c06be282b0cdb0971f840");
    testGetPoolID(
        makeAsset("aBc", acc1), makeAsset("aBcDeFgHiJkL", acc2),
        "a87bc151b119c1ea289905f0cb3cf95be7b0f096a0b6685bf2dcae70f9515d53");
    testGetPoolID(
        makeAsset("aBcD", acc1), makeAsset("aBcDeFgHiJkL", acc2),
        "3caf78118d6cabd42618eef47bbc2da8abe7fe42539b4b502f08766485592a81");
    testGetPoolID(
        makeAsset("aBc", acc2), makeAsset("aBcDeFgHiJk", acc1),
        "befb7f966ae63adcfde6a6670478bb7d936c29849e25e3387bb9e74566e3a29f");
    testGetPoolID(
        makeAsset("aBcD", acc2), makeAsset("aBcDeFgHiJk", acc1),
        "593cc996c3f0d32e165fcbee9fdc5dba6ab05140a4a9254e08ad8cb67fe657a1");
    testGetPoolID(
        makeAsset("aBc", acc2), makeAsset("aBcDeFgHiJkL", acc1),
        "d66af9b7417547c3dc000617533405349d1f622015daf3e9bad703ea34ee1d17");
    testGetPoolID(
        makeAsset("aBcD", acc2), makeAsset("aBcDeFgHiJkL", acc1),
        "c1c7a4b9db6e3754cae3017f72b6b7c93198f593182c541bcab3795c6413a677");
}
