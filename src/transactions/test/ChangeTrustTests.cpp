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
#include "transactions/test/SponsorshipTestUtils.h"

using namespace stellar;
using namespace stellar::txtest;

TEST_CASE_VERSIONS("change trust", "[tx][changetrust]")
{
    Config const& cfg = getTestConfig(0, Config::TESTDB_IN_MEMORY);

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);

    // set up world
    auto root = TestAccount::createRoot(*app);
    auto const& lm = app->getLedgerManager();
    auto const minBalance2 = lm.getLastMinBalance(2);
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

        auto validatePayingSelf = [&]() {
            auto gatewayAccountBefore = loadAccount(gateway);
            gateway.pay(gateway, idr, 50);
            validateTrustLineIsConst();
            auto gatewayAccountAfter = loadAccount(gateway);
            REQUIRE(gatewayAccountAfter.balance ==
                    (gatewayAccountBefore.balance -
                     app->getLedgerManager().getLastTxFee()));
        };

        validateTrustLineIsConst();

        for_versions_to(2, *app, [&] {
            // create a trustline with a limit of INT64_MAX - 1 will fail
            REQUIRE_THROWS_AS(gateway.changeTrust(idr, INT64_MAX - 1),
                              ex_CHANGE_TRUST_INVALID_LIMIT);
            validateTrustLineIsConst();

            // create a trustline with a limit of INT64_MAX
            gateway.changeTrust(idr, INT64_MAX);
            validateTrustLineIsConst();

            validatePayingSelf();

            // lower the limit will fail, because it is still INT64_MAX
            REQUIRE_THROWS_AS(gateway.changeTrust(idr, 50),
                              ex_CHANGE_TRUST_INVALID_LIMIT);
            validateTrustLineIsConst();

            // delete the trust line will fail
            REQUIRE_THROWS_AS(gateway.changeTrust(idr, 0),
                              ex_CHANGE_TRUST_INVALID_LIMIT);
            validateTrustLineIsConst();
        });

        for_versions(3, 15, *app, [&] {
            REQUIRE_THROWS_AS(gateway.changeTrust(idr, INT64_MAX - 1),
                              ex_CHANGE_TRUST_SELF_NOT_ALLOWED);
            validateTrustLineIsConst();

            REQUIRE_THROWS_AS(gateway.changeTrust(idr, INT64_MAX),
                              ex_CHANGE_TRUST_SELF_NOT_ALLOWED);
            validateTrustLineIsConst();

            validatePayingSelf();

            // lower the limit will fail, because it is still INT64_MAX
            REQUIRE_THROWS_AS(gateway.changeTrust(idr, 50),
                              ex_CHANGE_TRUST_SELF_NOT_ALLOWED);
            validateTrustLineIsConst();

            // delete the trust line will fail
            REQUIRE_THROWS_AS(gateway.changeTrust(idr, 0),
                              ex_CHANGE_TRUST_SELF_NOT_ALLOWED);
            validateTrustLineIsConst();
        });

        for_versions_from(16, *app, [&] {
            REQUIRE_THROWS_AS(gateway.changeTrust(idr, 50),
                              ex_CHANGE_TRUST_MALFORMED);
            validateTrustLineIsConst();

            validatePayingSelf();
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

    SECTION("sponsorship")
    {
        auto const minBalance0 = app->getLedgerManager().getLastMinBalance(0);
        auto const minBalance1 = app->getLedgerManager().getLastMinBalance(1);
        auto acc1 = root.create("a1", minBalance1 - 1);
        auto acc2 = root.create("a2", minBalance0);
        createSponsoredEntryButSponsorHasInsufficientBalance(
            *app, acc1, acc2, changeTrust(idr, 1000),
            [](OperationResult const& opRes) {
                return opRes.tr().changeTrustResult().code() ==
                       CHANGE_TRUST_LOW_RESERVE;
            });

        createModifyAndRemoveSponsoredEntry(
            *app, acc2, changeTrust(idr, 1000), changeTrust(idr, 999),
            changeTrust(idr, 1001), changeTrust(idr, 0),
            trustlineKey(acc2, idr), 14);
    }

    SECTION("too many")
    {
        auto acc1 =
            root.create("acc1", app->getLedgerManager().getLastMinBalance(0));
        auto usd = makeAsset(gateway, "USD");

        SECTION("too many sponsoring")
        {
            tooManySponsoring(*app, acc1, acc1.op(changeTrust(idr, 1)),
                              acc1.op(changeTrust(usd, 1)), 1);
        }
        SECTION("too many subentries")
        {
            tooManySubentries(*app, acc1, changeTrust(idr, 1),
                              changeTrust(usd, 1));
        }
    }

    SECTION("create and delete trustline in same tx")
    {
        for_versions_from(13, *app, [&] {
            auto tx = transactionFrameFromOps(
                app->getNetworkID(), root,
                {root.op(changeTrust(idr, 100)), root.op(changeTrust(idr, 0))},
                {});

            LedgerTxn ltx(app->getLedgerTxnRoot());
            TransactionMetaFrame txm(ltx.loadHeader().current().ledgerVersion);
            REQUIRE(
                tx->checkValidForTesting(app->getAppConnector(), ltx, 0, 0, 0));
            REQUIRE(tx->apply(app->getAppConnector(), ltx, txm));
        });
    }
}

TEST_CASE_VERSIONS("change trust pool share trustline",
                   "[tx][changetrust][liquiditypool]")
{
    Config const& cfg = getTestConfig(0, Config::TESTDB_IN_MEMORY);

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);

    // set up world
    auto root = TestAccount::createRoot(*app);
    auto const& lm = app->getLedgerManager();
    auto const minBalance2 = lm.getLastMinBalance(2);
    auto gateway = root.create("gw", minBalance2);
    Asset idr = makeAsset(gateway, "IDR");

    SECTION("pool trustline sponsorship")
    {
        auto usd = makeAsset(gateway, "USD");
        auto idrUsd =
            makeChangeTrustAssetPoolShare(idr, usd, LIQUIDITY_POOL_FEE_V18);

        auto acc1 =
            root.create("a1", app->getLedgerManager().getLastMinBalance(3));
        acc1.changeTrust(idr, 10);
        acc1.changeTrust(usd, 10);

        createModifyAndRemoveSponsoredEntry(
            *app, acc1, changeTrust(idrUsd, 1000), changeTrust(idrUsd, 999),
            changeTrust(idrUsd, 1001), changeTrust(idrUsd, 0),
            trustlineKey(acc1, changeTrustAssetToTrustLineAsset(idrUsd)), 18);
    }

    SECTION("pool trustline")
    {
        auto getNumSubEntries = [&](AccountID const& accountID) {
            LedgerTxn ltx(app->getLedgerTxnRoot());
            auto acc = stellar::loadAccount(ltx, accountID);
            return acc.current().data.account().numSubEntries;
        };

        auto poolShareTest = [&](Asset const& assetA, Asset const& assetB,
                                 bool startWithPool) {
            auto poolShareAsset = makeChangeTrustAssetPoolShare(
                assetA, assetB, LIQUIDITY_POOL_FEE_V18);

            if (startWithPool)
            {
                auto acc1 = root.create(
                    "a1",
                    app->getLedgerManager().getLastMinBalance(4) + 3 * 100);
                if (assetA.type() != ASSET_TYPE_NATIVE)
                {
                    acc1.changeTrust(assetA, 10);
                }
                if (assetB.type() != ASSET_TYPE_NATIVE)
                {
                    acc1.changeTrust(assetB, 10);
                }
                acc1.changeTrust(poolShareAsset, 10);
            }

            // trustlines will be deauthorized initially
            gateway.setOptions(setFlags(AUTH_REQUIRED_FLAG));

            bool rootIsIssuerA = isIssuer(root, assetA);
            bool rootIsIssuerB = isIssuer(root, assetB);

            if (!rootIsIssuerA || !rootIsIssuerB)
            {
                // root is missing trustline(s)
                REQUIRE_THROWS_AS(root.changeTrust(poolShareAsset, 10),
                                  ex_CHANGE_TRUST_TRUST_LINE_MISSING);
            }

            bool hasTrustA =
                assetA.type() != ASSET_TYPE_NATIVE && !rootIsIssuerA;
            bool hasTrustB =
                assetB.type() != ASSET_TYPE_NATIVE && !rootIsIssuerB;

            if (hasTrustA)
            {
                root.changeTrust(assetA, 10);

                REQUIRE_THROWS_AS(
                    root.changeTrust(poolShareAsset, 10),
                    ex_CHANGE_TRUST_NOT_AUTH_MAINTAIN_LIABILITIES);
                gateway.allowMaintainLiabilities(assetA, root);
            }

            if (hasTrustB)
            {
                // root is still missing assetB trustline
                REQUIRE_THROWS_AS(root.changeTrust(poolShareAsset, 10),
                                  ex_CHANGE_TRUST_TRUST_LINE_MISSING);
                root.changeTrust(assetB, 10);

                // assetB trustline is not authorized to maintain liabilities
                REQUIRE_THROWS_AS(
                    root.changeTrust(poolShareAsset, 10),
                    ex_CHANGE_TRUST_NOT_AUTH_MAINTAIN_LIABILITIES);
                gateway.allowMaintainLiabilities(assetB, root);
            }

            // this should create a LiquidityPoolEntry, and modify
            // liquidityPoolUseCount on the asset trustlines
            auto prePoolNumSubEntries = getNumSubEntries(root);
            root.changeTrust(poolShareAsset, 10);

            REQUIRE(getNumSubEntries(root) - prePoolNumSubEntries == 2);

            auto poolShareTlAsset =
                changeTrustAssetToTrustLineAsset(poolShareAsset);

            // pool share trustline shouldn't have any flags set
            REQUIRE(root.loadTrustLine(poolShareTlAsset).flags == 0);

            if (hasTrustA)
            {
                auto assetATl = root.loadTrustLine(assetA);
                REQUIRE(getTrustLineEntryExtensionV2(assetATl)
                            .liquidityPoolUseCount == 1);
            }

            if (hasTrustB)
            {
                auto assetBTl = root.loadTrustLine(assetB);
                REQUIRE(getTrustLineEntryExtensionV2(assetBTl)
                            .liquidityPoolUseCount == 1);
            }

            {
                LedgerTxn ltx(app->getLedgerTxnRoot());
                auto pool =
                    loadLiquidityPool(ltx, poolShareTlAsset.liquidityPoolID());
                REQUIRE(pool);
                REQUIRE(pool.current()
                            .data.liquidityPool()
                            .body.constantProduct()
                            .poolSharesTrustLineCount ==
                        (startWithPool ? 2 : 1));
            }

            // can't delete asset trustlines while they are used in a pool
            if (hasTrustA)
            {
                REQUIRE_THROWS_AS(root.changeTrust(assetA, 0),
                                  ex_CHANGE_TRUST_CANNOT_DELETE);
            }
            if (hasTrustB)
            {
                REQUIRE_THROWS_AS(root.changeTrust(assetB, 0),
                                  ex_CHANGE_TRUST_CANNOT_DELETE);
            }

            // create and delete a different pool share trustline using assetA
            if (hasTrustA)
            {
                gateway.setOptions(clearFlags(AUTH_REQUIRED_FLAG));
                auto assetZ = makeAssetAlphanum12(gateway, "ZZZ12");
                root.changeTrust(assetZ, 10);

                auto poolAZ = makeChangeTrustAssetPoolShare(
                    assetA, assetZ, LIQUIDITY_POOL_FEE_V18);
                root.changeTrust(poolAZ, 10);

                auto assetATl = root.loadTrustLine(assetA);
                REQUIRE(getTrustLineEntryExtensionV2(assetATl)
                            .liquidityPoolUseCount == 2);
                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    auto pool = loadLiquidityPool(
                        ltx, changeTrustAssetToTrustLineAsset(poolAZ)
                                 .liquidityPoolID());
                    REQUIRE(pool);
                    REQUIRE(pool.current()
                                .data.liquidityPool()
                                .body.constantProduct()
                                .poolSharesTrustLineCount == 1);
                }

                root.changeTrust(poolAZ, 0);
            }

            // try to reduce limit of pool share trust line below the balance
            if (hasTrustA)
            {
                gateway.allowTrust(assetA, root);
                gateway.pay(root, assetA, 10);
            }
            if (hasTrustB)
            {
                gateway.allowTrust(assetB, root);
                gateway.pay(root, assetB, 10);
            }

            auto poolID = xdrSha256(poolShareAsset.liquidityPool());
            root.liquidityPoolDeposit(poolID, 10, 10, Price{1, 1}, Price{1, 1});

            REQUIRE_THROWS_AS(root.changeTrust(poolShareAsset, 9),
                              ex_CHANGE_TRUST_INVALID_LIMIT);
            REQUIRE_THROWS_AS(root.changeTrust(poolShareAsset, 0),
                              ex_CHANGE_TRUST_INVALID_LIMIT);

            // increase the limit
            root.changeTrust(poolShareAsset, 11);
            root.liquidityPoolWithdraw(poolID, 10, 10, 10);

            // delete the pool sharetrust line
            auto postPoolNumSubEntries = getNumSubEntries(root);
            root.changeTrust(poolShareAsset, 0);
            REQUIRE(!root.hasTrustLine(poolShareTlAsset));

            REQUIRE(getNumSubEntries(root) == postPoolNumSubEntries - 2);

            if (hasTrustA)
            {
                auto assetATl = root.loadTrustLine(assetA);
                REQUIRE(getTrustLineEntryExtensionV2(assetATl)
                            .liquidityPoolUseCount == 0);
            }
            if (hasTrustB)
            {
                auto assetBTl = root.loadTrustLine(assetB);
                REQUIRE(getTrustLineEntryExtensionV2(assetBTl)
                            .liquidityPoolUseCount == 0);
            }

            {
                LedgerTxn ltx(app->getLedgerTxnRoot());
                auto pool =
                    loadLiquidityPool(ltx, poolShareTlAsset.liquidityPoolID());
                REQUIRE(startWithPool == (bool)pool);
            }

            // now the asset trustlines can be deleted
            if (hasTrustA)
            {
                root.pay(gateway, assetA, 10);
                root.changeTrust(assetA, 0);
            }
            if (hasTrustB)
            {
                root.pay(gateway, assetB, 10);
                root.changeTrust(assetB, 0);
            }
        };

        auto usd = makeAsset(gateway, "USD");
        auto idrUsd =
            makeChangeTrustAssetPoolShare(idr, usd, LIQUIDITY_POOL_FEE_V18);

        // Liquidity pools not supported pre V18
        for_versions_to(17, *app, [&] {
            REQUIRE_THROWS_AS(root.changeTrust(idrUsd, 10),
                              ex_CHANGE_TRUST_MALFORMED);
        });

        for_versions_from(18, *app, [&] {
            // CHANGE_TRUST_MALFORMED tests
            auto invalidFee = makeChangeTrustAssetPoolShare(
                idr, usd, LIQUIDITY_POOL_FEE_V18 - 1);
            REQUIRE_THROWS_AS(root.changeTrust(invalidFee, 10),
                              ex_CHANGE_TRUST_MALFORMED);

            // makeChangeTrustAssetPoolShare forbids invalid order so need to
            // make it manually
            ChangeTrustAsset invalidOrder;
            invalidOrder.type(ASSET_TYPE_POOL_SHARE);
            invalidOrder.liquidityPool().constantProduct().assetA = usd;
            invalidOrder.liquidityPool().constantProduct().assetB = idr;
            invalidOrder.liquidityPool().constantProduct().fee =
                LIQUIDITY_POOL_FEE_V18;
            REQUIRE_THROWS_AS(root.changeTrust(invalidOrder, 10),
                              ex_CHANGE_TRUST_MALFORMED);

            // makeChangeTrustAssetPoolShare forbids invalid order so need to
            // make it manually
            ChangeTrustAsset invalidOrderNative;
            invalidOrderNative.type(ASSET_TYPE_POOL_SHARE);
            invalidOrderNative.liquidityPool().constantProduct().assetA = idr;
            invalidOrderNative.liquidityPool().constantProduct().assetB =
                makeNativeAsset();
            invalidOrderNative.liquidityPool().constantProduct().fee =
                LIQUIDITY_POOL_FEE_V18;
            REQUIRE_THROWS_AS(root.changeTrust(invalidOrderNative, 10),
                              ex_CHANGE_TRUST_MALFORMED);

            // makeChangeTrustAssetPoolShare forbids invalid order so need to
            // make it manually
            ChangeTrustAsset sameAssets;
            sameAssets.type(ASSET_TYPE_POOL_SHARE);
            sameAssets.liquidityPool().constantProduct().assetA = idr;
            sameAssets.liquidityPool().constantProduct().assetB = idr;
            sameAssets.liquidityPool().constantProduct().fee =
                LIQUIDITY_POOL_FEE_V18;
            REQUIRE_THROWS_AS(root.changeTrust(sameAssets, 10),
                              ex_CHANGE_TRUST_MALFORMED);

            // create a pool trustline with a limit of 0
            REQUIRE_THROWS_AS(root.changeTrust(idrUsd, 0),
                              ex_CHANGE_TRUST_INVALID_LIMIT);

            SECTION("new pool with two non-native assets")
            {
                poolShareTest(idr, usd, false);
            }
            SECTION("existing pool with two non-native assets")
            {
                poolShareTest(idr, usd, true);
            }
            SECTION("new pool with a native asset")
            {
                poolShareTest(makeNativeAsset(), idr, false);
            }
            SECTION("existing pool with a native asset")
            {
                poolShareTest(makeNativeAsset(), idr, true);
            }
            SECTION("pool with two alphanum12 assets")
            {
                poolShareTest(makeAssetAlphanum12(gateway, "IDR12"),
                              makeAssetAlphanum12(gateway, "USD12"), false);
            }
            SECTION("new pool with one issuer asset")
            {
                poolShareTest(idr, makeAsset(root, "USD"), false);
            }
            SECTION("existing pool with one issuer asset")
            {
                poolShareTest(idr, makeAsset(root, "USD"), true);
            }
            SECTION("new pool with two issuer assets")
            {
                poolShareTest(makeAsset(root, "IDR"), makeAsset(root, "USD"),
                              false);
            }
            SECTION("existing pool with two issuer assets")
            {
                poolShareTest(makeAsset(root, "IDR"), makeAsset(root, "USD"),
                              true);
            }
            SECTION("too many")
            {
                auto acc1 = root.create(
                    "acc1", app->getLedgerManager().getLastMinBalance(3));
                auto native = makeNativeAsset();
                auto shareNative1 = makeChangeTrustAssetPoolShare(
                    native, idr, LIQUIDITY_POOL_FEE_V18);

                acc1.changeTrust(idr, 1);
                acc1.changeTrust(usd, 1);

                SECTION("too many sponsoring")
                {
                    tooManySponsoring(*app, acc1,
                                      acc1.op(changeTrust(idrUsd, 1)),
                                      acc1.op(changeTrust(shareNative1, 1)), 2);
                }
                SECTION("too many subentries")
                {
                    tooManySubentries(*app, acc1, changeTrust(idrUsd, 1),
                                      changeTrust(shareNative1, 1));
                }
            }
            SECTION("low reserve")
            {
                auto acc1 = root.create("acc1", lm.getLastMinBalance(3));

                acc1.changeTrust(idr, 10);
                acc1.changeTrust(usd, 10);

                REQUIRE_THROWS_AS(acc1.changeTrust(idrUsd, 10),
                                  ex_CHANGE_TRUST_LOW_RESERVE);

                root.pay(acc1, lm.getLastMinBalance(0));
                acc1.changeTrust(idrUsd, 10);
            }
            SECTION("sponsored pool share trustline where sponsor is issuer of "
                    "both assets")
            {
                auto acc1 = root.create("acc1", lm.getLastMinBalance(4));

                // gateway is the issuer of usd and idr
                acc1.changeTrust(idr, 10);
                acc1.changeTrust(usd, 10);

                // get rid of available balance so acc1 needs a sponsor for new
                // entries
                acc1.pay(root, acc1.getAvailableBalance() - 100);

                {
                    auto tx = transactionFrameFromOps(
                        app->getNetworkID(), gateway,
                        {gateway.op(beginSponsoringFutureReserves(acc1)),
                         acc1.op(changeTrust(idrUsd, 10)),
                         acc1.op(endSponsoringFutureReserves())},
                        {acc1});

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMetaFrame txm(
                        ltx.loadHeader().current().ledgerVersion);
                    REQUIRE(tx->checkValidForTesting(app->getAppConnector(),
                                                     ltx, 0, 0, 0));
                    REQUIRE(tx->apply(app->getAppConnector(), ltx, txm));
                    REQUIRE(tx->getResultCode() == txSUCCESS);
                    ltx.commit();
                }

                auto tlAsset = changeTrustAssetToTrustLineAsset(idrUsd);
                REQUIRE(acc1.hasTrustLine(tlAsset));
                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    checkSponsorship(ltx, trustlineKey(acc1, tlAsset), 1,
                                     &gateway.getPublicKey());
                    checkSponsorship(ltx, acc1, 0, nullptr, 4, 2, 0, 2);
                    checkSponsorship(ltx, gateway, 0, nullptr, 0, 2, 2, 0);
                }

                // give gateway enough fees for three operations
                root.pay(gateway, 300);

                SECTION("try to revoke the sponsorship but fail")
                {
                    auto tx = transactionFrameFromOps(
                        app->getNetworkID(), gateway,
                        {gateway.op(
                            revokeSponsorship(trustlineKey(acc1, tlAsset)))},
                        {});

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMetaFrame txm(
                        ltx.loadHeader().current().ledgerVersion);
                    REQUIRE(tx->checkValidForTesting(app->getAppConnector(),
                                                     ltx, 0, 0, 0));
                    REQUIRE(!tx->apply(app->getAppConnector(), ltx, txm));
                    REQUIRE(tx->getResultCode() == txFAILED);

                    auto const& opRes = tx->getResult().result.results()[0];
                    REQUIRE(opRes.tr().revokeSponsorshipResult().code() ==
                            REVOKE_SPONSORSHIP_LOW_RESERVE);
                }

                SECTION("try to transfer the sponsorship but fail")
                {
                    auto acc2 = root.create(
                        "acc2", app->getLedgerManager().getLastMinBalance(1));

                    auto tx = transactionFrameFromOps(
                        app->getNetworkID(), gateway,
                        {acc2.op(beginSponsoringFutureReserves(gateway)),
                         gateway.op(
                             revokeSponsorship(trustlineKey(acc1, tlAsset))),
                         gateway.op(endSponsoringFutureReserves())},
                        {acc2});

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMetaFrame txm(
                        ltx.loadHeader().current().ledgerVersion);
                    REQUIRE(tx->checkValidForTesting(app->getAppConnector(),
                                                     ltx, 0, 0, 0));
                    REQUIRE(!tx->apply(app->getAppConnector(), ltx, txm));
                    REQUIRE(tx->getResultCode() == txFAILED);

                    auto const& opRes = tx->getResult().result.results()[1];
                    REQUIRE(opRes.tr().revokeSponsorshipResult().code() ==
                            REVOKE_SPONSORSHIP_LOW_RESERVE);
                }

                SECTION("give owner enough reserves to take on the pool share "
                        "trustline")
                {
                    root.pay(acc1,
                             app->getLedgerManager().getLastMinBalance(0));

                    auto tx = transactionFrameFromOps(
                        app->getNetworkID(), gateway,
                        {gateway.op(
                            revokeSponsorship(trustlineKey(acc1, tlAsset)))},
                        {});

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMetaFrame txm(
                        ltx.loadHeader().current().ledgerVersion);
                    REQUIRE(tx->checkValidForTesting(app->getAppConnector(),
                                                     ltx, 0, 0, 0));
                    REQUIRE(tx->apply(app->getAppConnector(), ltx, txm));
                    REQUIRE(tx->getResultCode() == txSUCCESS);
                }

                SECTION(
                    "give account enough reserves to transfer the sponsorship")
                {
                    auto acc2 = root.create(
                        "acc2", app->getLedgerManager().getLastMinBalance(2));

                    auto tx = transactionFrameFromOps(
                        app->getNetworkID(), gateway,
                        {acc2.op(beginSponsoringFutureReserves(gateway)),
                         gateway.op(
                             revokeSponsorship(trustlineKey(acc1, tlAsset))),
                         gateway.op(endSponsoringFutureReserves())},
                        {acc2});

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMetaFrame txm(
                        ltx.loadHeader().current().ledgerVersion);
                    REQUIRE(tx->checkValidForTesting(app->getAppConnector(),
                                                     ltx, 0, 0, 0));
                    REQUIRE(tx->apply(app->getAppConnector(), ltx, txm));
                    REQUIRE(tx->getResultCode() == txSUCCESS);
                }
            }

            SECTION("below reserve")
            {
                auto increaseReserve = [&]() {
                    // double the reserve
                    auto newReserve = lm.getLastReserve() * 2;
                    REQUIRE(
                        executeUpgrade(*app, makeBaseReserveUpgrade(newReserve))
                            .baseReserve == newReserve);
                };

                auto acc1 = root.create("acc1", lm.getLastMinBalance(5));

                auto deletePoolTl = [&]() {
                    // delete the pool share trustline. acc1 can't pay the fee
                    // so use root
                    auto tx = transactionFrameFromOps(
                        app->getNetworkID(), root,
                        {acc1.op(changeTrust(idrUsd, 0))}, {acc1});

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMetaFrame txm(
                        ltx.loadHeader().current().ledgerVersion);
                    REQUIRE(tx->checkValidForTesting(app->getAppConnector(),
                                                     ltx, 0, 0, 0));
                    REQUIRE(tx->apply(app->getAppConnector(), ltx, txm));
                    ltx.commit();
                };

                SECTION("delete pool share trustline while below the reserve")
                {
                    acc1.changeTrust(idr, 1);
                    acc1.changeTrust(usd, 1);
                    acc1.changeTrust(idrUsd, 1);

                    // get rid of rest of available native balance
                    acc1.pay(root, acc1.getAvailableBalance() - 100);

                    REQUIRE(acc1.getAvailableBalance() == 0);

                    increaseReserve();

                    // acc1 is responsible for 6 reserves (2 for the account, 2
                    // for the asset trustlines, and 2 for the pool share
                    // trustlines)
                    REQUIRE(acc1.getAvailableBalance() == -600000000);

                    deletePoolTl();

                    REQUIRE(acc1.getAvailableBalance() == -200000000);
                }

                SECTION("delete sponsored pool share trustline while below the "
                        "reserve")
                {
                    acc1.changeTrust(idr, 1);
                    acc1.changeTrust(usd, 1);

                    auto sponsoringAcc =
                        root.create("sponsoringAcc", lm.getLastMinBalance(5));

                    {
                        auto tx = transactionFrameFromOps(
                            app->getNetworkID(), sponsoringAcc,
                            {sponsoringAcc.op(
                                 beginSponsoringFutureReserves(acc1)),
                             acc1.op(changeTrust(idrUsd, 1)),
                             acc1.op(endSponsoringFutureReserves())},
                            {acc1});

                        LedgerTxn ltx(app->getLedgerTxnRoot());
                        TransactionMetaFrame txm(
                            ltx.loadHeader().current().ledgerVersion);
                        REQUIRE(tx->checkValidForTesting(app->getAppConnector(),
                                                         ltx, 0, 0, 0));
                        REQUIRE(tx->apply(app->getAppConnector(), ltx, txm));
                        ltx.commit();
                    }

                    // get rid of rest of available native balance
                    acc1.pay(root, acc1.getAvailableBalance() - 100);
                    sponsoringAcc.pay(
                        root, sponsoringAcc.getAvailableBalance() - 100);

                    REQUIRE(acc1.getAvailableBalance() == 0);
                    REQUIRE(sponsoringAcc.getAvailableBalance() == 0);

                    increaseReserve();

                    // acc1 is responsible for 4 reserves (2 for the account and
                    // 2 for the asset trustlines)
                    REQUIRE(acc1.getAvailableBalance() == -400000000);

                    /// sponsoringAcc is responsible for 4 reserves (2 for the
                    /// account and 2 for the pool share trustline)
                    REQUIRE(sponsoringAcc.getAvailableBalance() == -400000000);

                    deletePoolTl();

                    REQUIRE(acc1.getAvailableBalance() == -400000000);
                    REQUIRE(sponsoringAcc.getAvailableBalance() == 0);
                }
            }
        });
    }
}
