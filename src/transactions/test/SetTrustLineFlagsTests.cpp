// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTxn.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "test/TestAccount.h"
#include "test/TestExceptions.h"
#include "test/TestMarket.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/SponsorshipUtils.h"
#include "transactions/TransactionUtils.h"
#include "transactions/test/SponsorshipTestUtils.h"
#include "util/Timer.h"

using namespace stellar;
using namespace stellar::txtest;

static ClaimableBalanceID
getRevokeBalanceID(TestAccount& testAccount, SequenceNumber sn,
                   Asset const& asset, PoolID const& poolID, uint32_t opNum,
                   EnvelopeType envelopeType = ENVELOPE_TYPE_POOL_REVOKE_OP_ID)
{
    HashIDPreimage hashPreimage;
    hashPreimage.type(envelopeType);

    if (envelopeType == ENVELOPE_TYPE_POOL_REVOKE_OP_ID)
    {
        hashPreimage.revokeID().sourceAccount = testAccount;
        hashPreimage.revokeID().seqNum = sn;
        hashPreimage.revokeID().opNum = opNum;
        hashPreimage.revokeID().liquidityPoolID = poolID;
        hashPreimage.revokeID().asset = asset;
    }
    else
    {
        hashPreimage.operationID().sourceAccount = testAccount;
        hashPreimage.operationID().seqNum = sn;
        hashPreimage.operationID().opNum = opNum;
    }

    ClaimableBalanceID balanceID;
    balanceID.type(CLAIMABLE_BALANCE_ID_TYPE_V0);
    balanceID.v0() = xdrSha256(hashPreimage);

    return balanceID;
}

static void
checkPoolUseCounts(TestAccount const& account, Asset const& asset,
                   int32_t count)
{
    if (asset.type() == ASSET_TYPE_NATIVE)
    {
        return;
    }

    auto tl = account.loadTrustLine(asset);
    REQUIRE(getTrustLineEntryExtensionV2(tl).liquidityPoolUseCount == count);
}

static int64_t
getBalance(TestAccount const& account, Asset const& asset)
{
    return asset.type() == ASSET_TYPE_NATIVE
               ? account.getBalance()
               : account.getTrustlineBalance(asset);
}

static uint32_t
getNumSponsoring(Application& app, TestAccount const& account)
{
    LedgerTxn ltx(app.getLedgerTxnRoot());

    auto ltxe = loadAccount(ltx, account.getPublicKey(), true);
    return getNumSponsoring(ltxe.current());
}

static uint32_t
getNumSponsored(Application& app, TestAccount const& account)
{
    LedgerTxn ltx(app.getLedgerTxnRoot());

    auto ltxe = loadAccount(ltx, account.getPublicKey(), true);
    return getNumSponsored(ltxe.current());
}

static void
checkNumSponsoring(Application& app, TestAccount const& account,
                   uint32_t numSponsoring)
{
    REQUIRE(getNumSponsoring(app, account) == numSponsoring);
}

static uint32_t
getNumOffers(Application& app, TestAccount const& account, Asset const& asset)
{
    LedgerTxn ltx(app.getLedgerTxnRoot());
    auto s = ltx.getOffersByAccountAndAsset(account, asset).size();
    return static_cast<uint32_t>(s);
}

TEST_CASE_VERSIONS("set trustline flags", "[tx][settrustlineflags]")
{
    auto const& cfg = getTestConfig(0, Config::TESTDB_IN_MEMORY);

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);

    const int64_t trustLineLimit = INT64_MAX;
    const int64_t trustLineStartingBalance = 20000;

    auto const minBalance4 = app->getLedgerManager().getLastMinBalance(4);

    // set up world
    auto root = TestAccount::createRoot(*app);
    auto gateway = root.create("gw", minBalance4);
    auto a1 = root.create("A1", minBalance4 + 10000);
    auto a2 = root.create("A2", minBalance4);

    auto idr = makeAsset(gateway, "IDR");
    auto native = makeNativeAsset();

    gateway.setOptions(setFlags(AUTH_REVOCABLE_FLAG));

    // gateway is not auth required, so trustline will be authorized
    a1.changeTrust(idr, trustLineLimit);

    SetTrustLineFlagsArguments emptyFlag;

    SECTION("not supported before version 17")
    {
        for_versions_to(16, *app, [&] {
            REQUIRE_THROWS_AS(gateway.setTrustLineFlags(idr, a1, emptyFlag),
                              ex_opNOT_SUPPORTED);
        });
    }

    for_versions_from(17, *app, [&] {
        // this lambda is used to verify offers are not pulled in non-revoke
        // scenarios
        auto market = TestMarket{*app};
        auto setFlagAndCheckOffer =
            [&](Asset const& asset, TestAccount& trustor,
                txtest::SetTrustLineFlagsArguments const& arguments,
                bool addOffer = true) {
                if (addOffer)
                {
                    auto offer = market.requireChangesWithOffer({}, [&] {
                        return market.addOffer(trustor,
                                               {native, asset, Price{1, 1}, 1});
                    });
                }

                // no offer should be deleted
                market.requireChanges({}, [&] {
                    gateway.setTrustLineFlags(asset, trustor, arguments);
                });
            };

        SECTION("small test")
        {
            gateway.pay(a1, idr, 5);
            a1.pay(gateway, idr, 1);

            auto flags =
                setTrustLineFlags(AUTHORIZED_TO_MAINTAIN_LIABILITIES_FLAG) |
                clearTrustLineFlags(AUTHORIZED_FLAG);

            setFlagAndCheckOffer(idr, a1, flags);

            REQUIRE_THROWS_AS(a1.pay(gateway, idr, trustLineStartingBalance),
                              ex_PAYMENT_SRC_NOT_AUTHORIZED);
        }

        SECTION("empty flags")
        {
            // verify that the setTrustLineFlags call is a noop
            auto flag = a1.getTrustlineFlags(idr);
            setFlagAndCheckOffer(idr, a1, emptyFlag);
            REQUIRE(flag == a1.getTrustlineFlags(idr));
        }

        SECTION("clear clawback")
        {
            gateway.setOptions(setFlags(AUTH_CLAWBACK_ENABLED_FLAG));
            a2.changeTrust(idr, trustLineLimit);
            gateway.pay(a2, idr, 100);

            gateway.clawback(a2, idr, 25);

            // clear the clawback flag and then try to clawback
            setFlagAndCheckOffer(
                idr, a2, clearTrustLineFlags(TRUSTLINE_CLAWBACK_ENABLED_FLAG));
            REQUIRE_THROWS_AS(gateway.clawback(a2, idr, 25),
                              ex_CLAWBACK_NOT_CLAWBACK_ENABLED);
        }

        SECTION("upgrade auth when not revocable")
        {
            SECTION("authorized -> authorized to maintain liabilities -> "
                    "authorized - with offers")
            {
                // authorized -> authorized to maintain liabilities
                auto maintainLiabilitiesflags =
                    setTrustLineFlags(AUTHORIZED_TO_MAINTAIN_LIABILITIES_FLAG) |
                    clearTrustLineFlags(AUTHORIZED_FLAG);

                setFlagAndCheckOffer(idr, a1, maintainLiabilitiesflags);

                gateway.setOptions(clearFlags(AUTH_REVOCABLE_FLAG));

                // authorized to maintain liabilities -> authorized
                auto authorizedFlags =
                    setTrustLineFlags(AUTHORIZED_FLAG) |
                    clearTrustLineFlags(
                        AUTHORIZED_TO_MAINTAIN_LIABILITIES_FLAG);
                setFlagAndCheckOffer(idr, a1, authorizedFlags, false);
            }

            SECTION("0 -> authorized")
            {
                gateway.denyTrust(idr, a1, TrustFlagOp::SET_TRUST_LINE_FLAGS);
                gateway.setOptions(clearFlags(AUTH_REVOCABLE_FLAG));

                gateway.setTrustLineFlags(idr, a1,
                                          setTrustLineFlags(AUTHORIZED_FLAG));
            }

            SECTION("0 -> authorized to maintain liabilities")
            {
                gateway.denyTrust(idr, a1, TrustFlagOp::SET_TRUST_LINE_FLAGS);
                gateway.setOptions(clearFlags(AUTH_REVOCABLE_FLAG));

                gateway.setTrustLineFlags(
                    idr, a1,
                    setTrustLineFlags(AUTHORIZED_TO_MAINTAIN_LIABILITIES_FLAG));
            }
        }

        SECTION("errors")
        {
            SECTION("invalid state")
            {
                gateway.setOptions(setFlags(AUTH_REQUIRED_FLAG));
                a2.changeTrust(idr, trustLineLimit);

                SECTION("set maintain liabilities when authorized")
                {
                    gateway.setTrustLineFlags(
                        idr, a2, setTrustLineFlags(AUTHORIZED_FLAG));
                    REQUIRE_THROWS_AS(
                        gateway.setTrustLineFlags(
                            idr, a2,
                            setTrustLineFlags(
                                AUTHORIZED_TO_MAINTAIN_LIABILITIES_FLAG)),
                        ex_SET_TRUST_LINE_FLAGS_INVALID_STATE);
                }
                SECTION("set authorized when maintain liabilities")
                {
                    gateway.setTrustLineFlags(
                        idr, a2,
                        setTrustLineFlags(
                            AUTHORIZED_TO_MAINTAIN_LIABILITIES_FLAG));
                    REQUIRE_THROWS_AS(
                        gateway.setTrustLineFlags(
                            idr, a2, setTrustLineFlags(AUTHORIZED_FLAG)),
                        ex_SET_TRUST_LINE_FLAGS_INVALID_STATE);
                }
            }

            SECTION("can't revoke")
            {
                // AllowTrustTests.cpp covers most cases.

                SECTION("authorized -> 0")
                {
                    gateway.setOptions(clearFlags(AUTH_REVOCABLE_FLAG));

                    REQUIRE_THROWS_AS(
                        gateway.setTrustLineFlags(
                            idr, a1, clearTrustLineFlags(AUTHORIZED_FLAG)),
                        ex_SET_TRUST_LINE_FLAGS_CANT_REVOKE);
                }
                SECTION("authorized to maintain liabilities -> 0")
                {
                    gateway.allowMaintainLiabilities(
                        idr, a1, TrustFlagOp::SET_TRUST_LINE_FLAGS);
                    gateway.setOptions(clearFlags(AUTH_REVOCABLE_FLAG));

                    REQUIRE_THROWS_AS(
                        gateway.setTrustLineFlags(
                            idr, a1,
                            clearTrustLineFlags(
                                AUTHORIZED_TO_MAINTAIN_LIABILITIES_FLAG)),
                        ex_SET_TRUST_LINE_FLAGS_CANT_REVOKE);
                }
            }

            SECTION("no trust")
            {
                REQUIRE_THROWS_AS(gateway.setTrustLineFlags(idr, a2, emptyFlag),
                                  ex_SET_TRUST_LINE_FLAGS_NO_TRUST_LINE);
            }

            SECTION("malformed")
            {
                // invalid auth flags
                REQUIRE_THROWS_AS(
                    gateway.setTrustLineFlags(
                        idr, a1, setTrustLineFlags(TRUSTLINE_AUTH_FLAGS)),
                    ex_SET_TRUST_LINE_FLAGS_MALFORMED);

                // can't set clawback
                REQUIRE_THROWS_AS(
                    gateway.setTrustLineFlags(
                        idr, a1,
                        setTrustLineFlags(TRUSTLINE_CLAWBACK_ENABLED_FLAG)),
                    ex_SET_TRUST_LINE_FLAGS_MALFORMED);

                // can't use native asset
                REQUIRE_THROWS_AS(
                    gateway.setTrustLineFlags(native, a1, emptyFlag),
                    ex_SET_TRUST_LINE_FLAGS_MALFORMED);

                // invalid asset
                auto invalidAssets = testutil::getInvalidAssets(gateway);
                for (auto const& asset : invalidAssets)
                {
                    REQUIRE_THROWS_AS(
                        gateway.setTrustLineFlags(asset, a1, emptyFlag),
                        ex_SET_TRUST_LINE_FLAGS_MALFORMED);
                }

                {
                    // set and clear flags can't overlap
                    auto setFlag = setTrustLineFlags(AUTHORIZED_FLAG);
                    auto clearFlag = clearTrustLineFlags(TRUSTLINE_AUTH_FLAGS);
                    REQUIRE_THROWS_AS(
                        gateway.setTrustLineFlags(idr, a1, setFlag | clearFlag),
                        ex_SET_TRUST_LINE_FLAGS_MALFORMED);

                    REQUIRE_THROWS_AS(
                        gateway.setTrustLineFlags(
                            idr, a1,
                            setFlag | clearTrustLineFlags(AUTHORIZED_FLAG)),
                        ex_SET_TRUST_LINE_FLAGS_MALFORMED);
                }

                // can't clear or set unsupported flags
                REQUIRE_THROWS_AS(
                    gateway.setTrustLineFlags(
                        idr, a1,
                        setTrustLineFlags(MASK_TRUSTLINE_FLAGS_V17 + 1)),
                    ex_SET_TRUST_LINE_FLAGS_MALFORMED);
                REQUIRE_THROWS_AS(
                    gateway.setTrustLineFlags(
                        idr, a1,
                        clearTrustLineFlags(MASK_TRUSTLINE_FLAGS_V17 + 1)),
                    ex_SET_TRUST_LINE_FLAGS_MALFORMED);

                // can't operate on self
                REQUIRE_THROWS_AS(
                    gateway.setTrustLineFlags(idr, gateway, emptyFlag),
                    ex_SET_TRUST_LINE_FLAGS_MALFORMED);

                // source account is not issuer
                REQUIRE_THROWS_AS(a1.setTrustLineFlags(idr, gateway, emptyFlag),
                                  ex_SET_TRUST_LINE_FLAGS_MALFORMED);
            }
        }
    });
}

TEST_CASE_VERSIONS("revoke from pool",
                   "[tx][settrustlineflags][allowtrust][liquiditypool]")
{
    VirtualClock clock;
    auto app = createTestApplication(
        clock, getTestConfig(0, Config::TESTDB_IN_MEMORY));

    // set up world
    auto root = TestAccount::createRoot(*app);

    auto& lm = app->getLedgerManager();
    auto txFee = lm.getLastTxFee();

    auto minBal = [&](int32_t n) { return lm.getLastMinBalance(n); };

    auto acc1 = root.create("acc1", minBal(10));
    auto wrongBalanceIDAcc = root.create("wrong", minBal(0));

    auto native = makeNativeAsset();
    auto cur1 = makeAsset(root, "CUR1");
    auto cur2 = makeAsset(root, "CUR2");

    root.setOptions(setFlags(AUTH_REVOCABLE_FLAG));

    auto share12 =
        makeChangeTrustAssetPoolShare(cur1, cur2, LIQUIDITY_POOL_FEE_V18);
    auto pool12 = xdrSha256(share12.liquidityPool());

    auto shareNative1 =
        makeChangeTrustAssetPoolShare(native, cur1, LIQUIDITY_POOL_FEE_V18);
    auto poolNative1 = xdrSha256(shareNative1.liquidityPool());

    auto redeemBalance = [&](bool testClawback, TestAccount& claimant,
                             TestAccount& txSourceAccount, Asset const& asset,
                             PoolID const& poolID, SequenceNumber revokeSeqNum,
                             uint32_t opIndex, int64_t expectedAmount) {
        auto balanceID = getRevokeBalanceID(txSourceAccount, revokeSeqNum,
                                            asset, poolID, opIndex);

        auto wrongEnvelopeTypeBalanceID =
            getRevokeBalanceID(txSourceAccount, revokeSeqNum, asset, poolID,
                               opIndex, ENVELOPE_TYPE_OP_ID);

        auto wrongOpIndexBalanceID = getRevokeBalanceID(
            txSourceAccount, revokeSeqNum, asset, poolID, opIndex + 1);

        auto wrongSourceAccountBalanceID = getRevokeBalanceID(
            wrongBalanceIDAcc, revokeSeqNum, asset, poolID, opIndex);

        if (testClawback)
        {
            // try the incorrect balance ID
            REQUIRE_THROWS_AS(
                root.clawbackClaimableBalance(wrongEnvelopeTypeBalanceID),
                ex_CLAWBACK_CLAIMABLE_BALANCE_DOES_NOT_EXIST);

            REQUIRE_THROWS_AS(
                root.clawbackClaimableBalance(wrongOpIndexBalanceID),
                ex_CLAWBACK_CLAIMABLE_BALANCE_DOES_NOT_EXIST);

            REQUIRE_THROWS_AS(
                root.clawbackClaimableBalance(wrongSourceAccountBalanceID),
                ex_CLAWBACK_CLAIMABLE_BALANCE_DOES_NOT_EXIST);

            if (asset.type() == ASSET_TYPE_NATIVE)
            {
                REQUIRE_THROWS_AS(root.clawbackClaimableBalance(balanceID),
                                  ex_CLAWBACK_CLAIMABLE_BALANCE_NOT_ISSUER);
            }
            else
            {
                root.clawbackClaimableBalance(balanceID);
            }
        }
        else
        {
            // try the incorrect balance ID's
            REQUIRE_THROWS_AS(
                claimant.claimClaimableBalance(wrongEnvelopeTypeBalanceID),
                ex_CLAIM_CLAIMABLE_BALANCE_DOES_NOT_EXIST);

            REQUIRE_THROWS_AS(
                claimant.claimClaimableBalance(wrongOpIndexBalanceID),
                ex_CLAIM_CLAIMABLE_BALANCE_DOES_NOT_EXIST);

            REQUIRE_THROWS_AS(
                claimant.claimClaimableBalance(wrongSourceAccountBalanceID),
                ex_CLAIM_CLAIMABLE_BALANCE_DOES_NOT_EXIST);

            if (asset.type() != ASSET_TYPE_NATIVE)
            {
                REQUIRE_THROWS_AS(
                    root.clawbackClaimableBalance(balanceID),
                    ex_CLAWBACK_CLAIMABLE_BALANCE_NOT_CLAWBACK_ENABLED);
            }

            auto preRedeemBalance = getBalance(claimant, asset);
            claimant.claimClaimableBalance(balanceID);
            auto fee = asset.type() == ASSET_TYPE_NATIVE ? 100 : 0;
            REQUIRE(preRedeemBalance + expectedAmount - fee ==
                    getBalance(claimant, asset));
        }
    };

    auto depositIntoPool = [&](TestAccount& account, Asset const& assetA,
                               Asset const& assetB) {
        REQUIRE(assetA < assetB);

        // This method assumes that either root or acc1 is the issuer
        auto getIssuerAcc = [&](Asset const& asset) -> TestAccount& {
            if (getIssuer(asset) == acc1.getPublicKey())
            {
                return acc1;
            }

            REQUIRE(getIssuer(asset) == root.getPublicKey());
            return root;
        };

        if (assetA.type() != ASSET_TYPE_NATIVE && !isIssuer(account, assetA))
        {
            account.changeTrust(assetA, 200 * 2);
            getIssuerAcc(assetA).pay(account, assetA, 200);
        }

        // assetB can't be native
        if (!isIssuer(account, assetB))
        {
            account.changeTrust(assetB, 50 * 2);
            getIssuerAcc(assetB).pay(account, assetB, 50);
        }

        auto ctAsset = makeChangeTrustAssetPoolShare(assetA, assetB,
                                                     LIQUIDITY_POOL_FEE_V18);
        auto poolID = xdrSha256(ctAsset.liquidityPool());

        account.changeTrust(ctAsset, 100);
        account.liquidityPoolDeposit(poolID, 200, 50, Price{4, 1}, Price{4, 1});

        return ctAsset;
    };

    for_versions_from(18, *app, [&] {
        auto revokeTest = [&](TrustFlagOp flagOp) {
            auto revoke = [&](TestAccount const& account, Asset const& asset,
                              std::vector<ChangeTrustAsset> const& ctAssets,
                              uint32_t numClaimableBalancesCreated) {
                auto preRevokeNumSubEntries = account.getNumSubEntries();
                auto numOffers = getNumOffers(*app, account, asset);

                auto preRevokeNumSponsoring = getNumSponsoring(*app, account);

                root.denyTrust(asset, account, flagOp);
                REQUIRE(preRevokeNumSubEntries == account.getNumSubEntries() +
                                                      numOffers +
                                                      (2 * ctAssets.size()));

                REQUIRE(preRevokeNumSponsoring ==
                        getNumSponsoring(*app, account) -
                            numClaimableBalancesCreated);

                for (auto const& ctAsset : ctAssets)
                {
                    REQUIRE(!account.hasTrustLine(
                        changeTrustAssetToTrustLineAsset(ctAsset)));
                }
            };

            auto revokeAfterDeposit = [&](bool testClawback,
                                          Asset const& assetA,
                                          Asset const& assetB) {
                if (testClawback)
                {
                    root.setOptions(setFlags(AUTH_CLAWBACK_ENABLED_FLAG));
                }

                auto ctAsset = depositIntoPool(acc1, assetA, assetB);
                auto poolID = xdrSha256(ctAsset.liquidityPool());
                checkLiquidityPool(*app, poolID, 200, 50, 100, 1);

                SECTION("pool is deleted")
                {
                    // revoke
                    revoke(acc1, assetB, {ctAsset}, 2);
                    checkPoolUseCounts(acc1, assetA, 0);
                    checkPoolUseCounts(acc1, assetB, 0);

                    // Pool should be deleted since the last pool share
                    // trustline was deleted
                    {
                        LedgerTxn ltx(app->getLedgerTxnRoot());
                        REQUIRE(!loadLiquidityPool(ltx, poolID));
                    }

                    // this seqnum was used to create the balance ID's
                    auto revokeSeqNum = root.getLastSequenceNumber();
                    root.allowTrust(assetB, acc1);

                    redeemBalance(testClawback, acc1, root, assetA, poolID,
                                  revokeSeqNum, 0, 200);
                    redeemBalance(testClawback, acc1, root, assetB, poolID,
                                  revokeSeqNum, 0, 50);
                }

                SECTION("pool still exists")
                {
                    auto acc2 = root.create("acc2", minBal(10));

                    // deposit from second account
                    depositIntoPool(acc2, assetA, assetB);
                    checkLiquidityPool(*app, poolID, 400, 100, 200, 2);

                    // revoke
                    revoke(acc1, assetB, {ctAsset}, 2);
                    checkLiquidityPool(*app, poolID, 200, 50, 100, 1);
                    checkPoolUseCounts(acc1, assetA, 0);
                    checkPoolUseCounts(acc1, assetB, 0);

                    // this seqnum was used to create the balance ID's
                    auto revokeSeqNum = root.getLastSequenceNumber();
                    root.allowTrust(assetB, acc1);

                    redeemBalance(testClawback, acc1, root, assetA, poolID,
                                  revokeSeqNum, 0, 200);
                    redeemBalance(testClawback, acc1, root, assetB, poolID,
                                  revokeSeqNum, 0, 50);
                }
            };

            SECTION("claim - both non-native")
            {
                revokeAfterDeposit(false, cur1, cur2);
            }

            SECTION("clawback - both non-native")
            {
                revokeAfterDeposit(true, cur1, cur2);
            }

            SECTION("claim - one non-native")
            {
                revokeAfterDeposit(false, native, cur1);
            }

            SECTION("clawback - one non-native")
            {
                revokeAfterDeposit(true, native, cur1);
            }

            SECTION("validate op num and tx account is used in hash")
            {
                depositIntoPool(acc1, native, cur1);
                checkLiquidityPool(*app, poolNative1, 200, 50, 100, 1);

                // Allow acc1 to submit an op from root
                auto sk1 = makeSigner(acc1, 100);
                root.setOptions(setSigner(sk1));

                auto revokeOp =
                    flagOp == TrustFlagOp::ALLOW_TRUST
                        ? root.op(allowTrust(acc1, cur1, 0))
                        : root.op(setTrustLineFlags(
                              acc1, cur1,
                              clearTrustLineFlags(AUTHORIZED_FLAG)));
                applyCheck(acc1.tx({root.op(payment(acc1, 1)), revokeOp}),
                           *app);

                root.allowTrust(cur1, acc1);

                auto revokeSeqNum = acc1.getLastSequenceNumber();

                redeemBalance(false, acc1, acc1, native, poolNative1,
                              revokeSeqNum, 1, 200);
                redeemBalance(false, acc1, acc1, cur1, poolNative1,
                              revokeSeqNum, 1, 50);
            }

            SECTION("claimable balance created for issuer")
            {
                SECTION("assetA issuer")
                {
                    auto acc1Btc = makeAsset(acc1, "BTC");
                    auto shareBtc1 = depositIntoPool(acc1, acc1Btc, cur1);
                    auto poolBtc1 = xdrSha256(shareBtc1.liquidityPool());

                    revoke(acc1, cur1, {shareBtc1}, 1);

                    checkPoolUseCounts(acc1, cur1, 0);

                    // Pool should be deleted since the last pool share
                    // trustline was deleted
                    {
                        LedgerTxn ltx(app->getLedgerTxnRoot());
                        REQUIRE(!loadLiquidityPool(ltx, poolBtc1));
                    }

                    auto revokeSeqNum = root.getLastSequenceNumber();
                    root.allowTrust(cur1, acc1);

                    redeemBalance(false, acc1, root, cur1, poolBtc1,
                                  revokeSeqNum, 0, 50);

                    // A claimable balance was not created for btc because acc1
                    // is the issuer
                    auto btcBalanceID =
                        getRevokeBalanceID(root, root.getLastSequenceNumber(),
                                           acc1Btc, poolBtc1, 0);
                    REQUIRE_THROWS_AS(
                        acc1.claimClaimableBalance(btcBalanceID),
                        ex_CLAIM_CLAIMABLE_BALANCE_DOES_NOT_EXIST);

                    checkNumSponsoring(*app, acc1, 0);
                }
                SECTION("assetB issuer")
                {
                    auto acc1Usd = makeAsset(acc1, "USD1");
                    auto share1Usd = depositIntoPool(acc1, cur1, acc1Usd);
                    auto pool1Usd = xdrSha256(share1Usd.liquidityPool());

                    revoke(acc1, cur1, {share1Usd}, 1);

                    checkPoolUseCounts(acc1, cur1, 0);

                    // Pool should be deleted since the last pool share
                    // trustline was deleted
                    {
                        LedgerTxn ltx(app->getLedgerTxnRoot());
                        REQUIRE(!loadLiquidityPool(ltx, pool1Usd));
                    }

                    auto revokeSeqNum = root.getLastSequenceNumber();
                    root.allowTrust(cur1, acc1);

                    redeemBalance(false, acc1, root, cur1, pool1Usd,
                                  revokeSeqNum, 0, 200);

                    // A claimable balance was not created for usd because acc1
                    // is the issuer
                    auto usdBalanceID =
                        getRevokeBalanceID(root, root.getLastSequenceNumber(),
                                           acc1Usd, pool1Usd, 0);
                    REQUIRE_THROWS_AS(
                        acc1.claimClaimableBalance(usdBalanceID),
                        ex_CLAIM_CLAIMABLE_BALANCE_DOES_NOT_EXIST);

                    checkNumSponsoring(*app, acc1, 0);
                }
            }

            SECTION("revoke pool share trustline that results in one less "
                    "claimable balance due to rounding")
            {
                auto depositForRoundingRevoke = [&](Asset const& assetA,
                                                    Asset const& assetB) {
                    auto ctAsset = depositIntoPool(acc1, assetA, assetB);
                    auto poolID = xdrSha256(ctAsset.liquidityPool());

                    auto acc2 = root.create("acc2", minBal(10));
                    depositIntoPool(acc2, assetA, assetB);

                    checkLiquidityPool(*app, poolID, 400, 100, 200, 2);
                };

                auto tradeAndRevoke =
                    [&](Asset const& sendAsset, int64_t sendMax,
                        Asset const& destAsset, int64_t destAmount,
                        bool sendAssetA, ChangeTrustAsset const& ctAsset,
                        uint32_t numClaimableBalancesCreated) {
                        root.pay(acc1, sendAsset, sendMax, destAsset,
                                 destAmount, {});
                        auto poolID = xdrSha256(ctAsset.liquidityPool());

                        if (sendAssetA)
                        {
                            auto reserveA = 400 + sendMax;
                            checkLiquidityPool(*app, poolID, reserveA, 1, 200,
                                               2);

                            revoke(acc1, cur2, {ctAsset},
                                   numClaimableBalancesCreated);

                            // half of the pool shares were redeemed in the
                            // revoke, but reserveB was unchanged due to
                            // rounding
                            auto redeemed = reserveA / 2;
                            checkLiquidityPool(*app, poolID,
                                               reserveA - redeemed, 1, 100, 1);
                        }
                        else
                        {
                            auto reserveB = 100 + sendMax;
                            checkLiquidityPool(*app, poolID, 1, reserveB, 200,
                                               2);

                            revoke(acc1, cur2, {ctAsset},
                                   numClaimableBalancesCreated);

                            // half of the pool shares were redeemed in the
                            // revoke, but reserveA was unchanged due to
                            // rounding
                            auto redeemed = reserveB / 2;
                            checkLiquidityPool(*app, poolID, 1,
                                               reserveB - redeemed, 100, 1);
                        }
                    };

                auto validateClaimableBalances =
                    [&](PoolID poolID, Asset const& claimAsset,
                        Asset const& failClaimAsset, int64_t amount) {
                        auto revokeSeqNum = root.getLastSequenceNumber();

                        // we use cur2 to trigger the revoke, so authorize it
                        // here in case it's the claimAsset
                        root.allowTrust(cur2, acc1);

                        if (claimAsset.type() != ASSET_TYPE_NATIVE)
                        {
                            acc1.changeTrust(claimAsset, amount);
                        }

                        redeemBalance(false, acc1, root, claimAsset, poolID,
                                      revokeSeqNum, 0, amount);

                        // verify that no claimable balance was created for the
                        // other asset
                        auto balanceID = getRevokeBalanceID(
                            root, revokeSeqNum, failClaimAsset, poolID, 0);
                        REQUIRE_THROWS_AS(
                            acc1.claimClaimableBalance(balanceID),
                            ex_CLAIM_CLAIMABLE_BALANCE_DOES_NOT_EXIST);

                        // make sure this account isn't sponsoring any
                        // claimable balances (the one that was sponsored
                        // was claimed above)
                        checkNumSponsoring(*app, acc1, 0);
                    };

                SECTION("trade down assetB - non-native")
                {
                    depositForRoundingRevoke(cur1, cur2);

                    // ceil((400*99)/(100-99)/(1-.003)) = 39720
                    tradeAndRevoke(cur1, 39720, cur2, 99, true, share12, 1);

                    // reserveA was 400 before the trade
                    validateClaimableBalances(pool12, cur1, cur2,
                                              (39720 + 400) / 2);
                }

                SECTION("trade down assetA - non-native")
                {
                    depositForRoundingRevoke(cur1, cur2);

                    // ceil((100*399)/(400-399)/(1-.003)) = 40021
                    tradeAndRevoke(cur2, 40021, cur1, 399, false, share12, 1);

                    // reserveB was 100 before the trade
                    validateClaimableBalances(pool12, cur2, cur1,
                                              (40021 + 100) / 2);
                }

                // we use cur2 to trigger the revoke, so make it assetB here
                auto shareNative2 = makeChangeTrustAssetPoolShare(
                    native, cur2, LIQUIDITY_POOL_FEE_V18);
                auto poolNative2 = xdrSha256(shareNative2.liquidityPool());

                SECTION("trade down assetB - native")
                {
                    depositForRoundingRevoke(native, cur2);

                    // ceil((400*99)/(100-99)/(1-.003)) = 39720
                    tradeAndRevoke(native, 39720, cur2, 99, true, shareNative2,
                                   1);

                    // reserveA was 400 before the trade
                    validateClaimableBalances(poolNative2, native, cur2,
                                              (39720 + 400) / 2);
                }

                SECTION("trade down assetA - native")
                {
                    depositForRoundingRevoke(native, cur2);

                    // ceil((100*399)/(400-399)/(1-.003)) = 40021
                    tradeAndRevoke(cur2, 40021, native, 399, false,
                                   shareNative2, 1);

                    // reserveB was 100 before the trade
                    validateClaimableBalances(poolNative2, cur2, native,
                                              (40021 + 100) / 2);
                }

                auto noClaimableBalancesCreated = [&](PoolID poolID,
                                                      Asset const& assetA,
                                                      Asset const& assetB) {
                    auto revokeSeqNum = root.getLastSequenceNumber();

                    auto balanceIdAssetA = getRevokeBalanceID(
                        root, revokeSeqNum, assetA, poolID, 0);
                    auto balanceIdAssetB = getRevokeBalanceID(
                        root, revokeSeqNum, assetB, poolID, 0);

                    REQUIRE_THROWS_AS(
                        acc1.claimClaimableBalance(balanceIdAssetA),
                        ex_CLAIM_CLAIMABLE_BALANCE_DOES_NOT_EXIST);

                    REQUIRE_THROWS_AS(
                        acc1.claimClaimableBalance(balanceIdAssetB),
                        ex_CLAIM_CLAIMABLE_BALANCE_DOES_NOT_EXIST);

                    checkNumSponsoring(*app, acc1, 0);
                };

                SECTION("revoked account is issuer of assetA")
                {
                    auto acc1Btc = makeAsset(acc1, "BTC");
                    auto shareBtc2 = makeChangeTrustAssetPoolShare(
                        acc1Btc, cur2, LIQUIDITY_POOL_FEE_V18);
                    auto poolBtc2 = xdrSha256(shareBtc2.liquidityPool());

                    depositForRoundingRevoke(acc1Btc, cur2);

                    // the root account is used to make the trade in
                    // tradeAndRevoke
                    root.changeTrust(acc1Btc, 39720);
                    acc1.pay(root, acc1Btc, 39720);

                    // ceil((400*99)/(100-99)/(1-.003)) = 39720
                    tradeAndRevoke(acc1Btc, 39720, cur2, 99, true, shareBtc2,
                                   0);

                    noClaimableBalancesCreated(poolBtc2, acc1Btc, cur2);
                }

                SECTION("revoked account is issuer of assetB")
                {
                    auto acc1Usd = makeAsset(acc1, "USD1");

                    auto share2Usd = makeChangeTrustAssetPoolShare(
                        cur2, acc1Usd, LIQUIDITY_POOL_FEE_V18);
                    auto pool2Usd = xdrSha256(share2Usd.liquidityPool());

                    depositForRoundingRevoke(cur2, acc1Usd);

                    // the root account is used to make the trade in
                    // tradeAndRevoke
                    root.changeTrust(acc1Usd, 40021);
                    acc1.pay(root, acc1Usd, 40021);

                    // ceil((100*399)/(400-399)/(1-.003)) = 40021
                    tradeAndRevoke(acc1Usd, 40021, cur2, 399, false, share2Usd,
                                   0);

                    noClaimableBalancesCreated(pool2Usd, acc1Usd, cur2);
                }
            }

            SECTION("revoke from 0 balance pool share trustline")
            {
                acc1.changeTrust(cur1, 1);
                acc1.changeTrust(cur2, 1);
                acc1.changeTrust(share12, 1);

                checkLiquidityPool(*app, pool12, 0, 0, 0, 1);

                revoke(acc1, cur1, {share12}, 0);
                checkPoolUseCounts(acc1, cur1, 0);
                checkPoolUseCounts(acc1, cur2, 0);

                // no claimable balance should've been created
                auto cur1BalanceID = getRevokeBalanceID(
                    root, root.getLastSequenceNumber(), cur1, pool12, 0);
                auto cur2BalanceID = getRevokeBalanceID(
                    root, root.getLastSequenceNumber(), cur2, pool12, 0);

                REQUIRE_THROWS_AS(acc1.claimClaimableBalance(cur1BalanceID),
                                  ex_CLAIM_CLAIMABLE_BALANCE_DOES_NOT_EXIST);

                REQUIRE_THROWS_AS(acc1.claimClaimableBalance(cur1BalanceID),
                                  ex_CLAIM_CLAIMABLE_BALANCE_DOES_NOT_EXIST);

                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    REQUIRE(!loadLiquidityPool(ltx, pool12));

                    // make sure this account isn't sponsoring any claimable
                    // balances
                    auto account = ltx.load(accountKey(acc1.getPublicKey()));

                    auto& ae = account.current().data.account();
                    REQUIRE(
                        (!hasAccountEntryExtV2(ae) ||
                         getAccountEntryExtensionV2(ae).numSponsoring == 0));
                }
            }

            SECTION("trade with pool, revoke, then trade again")
            {
                auto tradeRevokeTest = [&](Asset const& assetA,
                                           Asset const& assetB) {
                    auto ctAsset = depositIntoPool(acc1, assetA, assetB);
                    auto poolID = xdrSha256(ctAsset.liquidityPool());

                    auto acc2 = root.create("acc2", minBal(10));
                    depositIntoPool(acc2, assetA, assetB);

                    checkLiquidityPool(*app, poolID, 400, 100, 200, 2);

                    // ceil((400*10)/(100-10)/(1-.003)) = 45
                    root.pay(acc2, assetA, 45, assetB, 10, {});

                    checkLiquidityPool(*app, poolID, 445, 90, 200, 2);

                    revoke(acc1, assetB, {ctAsset}, 2);

                    checkLiquidityPool(*app, poolID, 223, 45, 100, 1);

                    // redeem the claimable balances
                    auto revokeSeqNum = root.getLastSequenceNumber();
                    root.allowTrust(assetB, acc1);

                    redeemBalance(false, acc1, root, assetA, poolID,
                                  revokeSeqNum, 0, 222);
                    redeemBalance(false, acc1, root, assetB, poolID,
                                  revokeSeqNum, 0, 45);

                    // ceil((223*20)/(45-20)/(1-.003)) = 179
                    root.pay(acc2, assetA, 179, assetB, 20, {});

                    checkLiquidityPool(*app, poolID, 402, 25, 100, 1);

                    // now trade the other way

                    // ceil((25*40)/(402-40)/(1-.003)) = 3
                    root.pay(acc2, assetB, 3, assetA, 40, {});

                    checkLiquidityPool(*app, poolID, 362, 28, 100, 1);
                };

                SECTION("both non-native")
                {
                    tradeRevokeTest(cur1, cur2);
                }
                SECTION("one non-native")
                {
                    tradeRevokeTest(native, cur2);
                }
            }

            SECTION("revoke leads to redeemed pool shares and pulled offers")
            {
                auto poolAndOfferRevokeTest = [&](Asset const& assetA,
                                                  Asset const& assetB,
                                                  Asset const& assetToRevoke) {
                    auto ctAsset = depositIntoPool(acc1, assetA, assetB);
                    auto poolID = xdrSha256(ctAsset.liquidityPool());

                    root.pay(acc1, assetA, 10);

                    auto market = TestMarket{*app};
                    auto offer = market.requireChangesWithOffer({}, [&] {
                        return market.addOffer(
                            acc1, {assetA, assetB, Price{1, 1}, 10});
                    });

                    market.requireChanges(
                        {{offer.key, OfferState::DELETED}}, [&] {
                            revoke(acc1, assetToRevoke, {ctAsset}, 2);

                            auto revokeSeqNum = root.getLastSequenceNumber();
                            root.allowTrust(assetToRevoke, acc1);

                            redeemBalance(false, acc1, root, assetA, poolID,
                                          revokeSeqNum, 0, 200);
                            redeemBalance(false, acc1, root, assetB, poolID,
                                          revokeSeqNum, 0, 50);
                        });
                };

                SECTION("both non-native - revoke assetA")
                {
                    poolAndOfferRevokeTest(cur1, cur2, cur1);
                }
                SECTION("both non-native - revoke assetB")
                {
                    poolAndOfferRevokeTest(cur1, cur2, cur2);
                }
                SECTION("one non-native - revoke assetB")
                {
                    poolAndOfferRevokeTest(native, cur2, cur2);
                }
            }

            SECTION("revoke from multiple pools")
            {
                auto usd = makeAsset(root, "usd");
                auto btc = makeAsset(root, "btc");
                auto eur = makeAsset(root, "eur");

                auto shareBtcUsd = makeChangeTrustAssetPoolShare(
                    btc, usd, LIQUIDITY_POOL_FEE_V18);
                auto shareEurUsd = makeChangeTrustAssetPoolShare(
                    eur, usd, LIQUIDITY_POOL_FEE_V18);
                auto shareBtcEur = makeChangeTrustAssetPoolShare(
                    btc, eur, LIQUIDITY_POOL_FEE_V18);

                auto poolBtcUsd = xdrSha256(shareBtcUsd.liquidityPool());
                auto poolEurUsd = xdrSha256(shareEurUsd.liquidityPool());
                auto poolBtcEur = xdrSha256(shareBtcEur.liquidityPool());

                for (int i = 0; i < 3; ++i)
                {
                    auto acc =
                        root.create(fmt::format("account{}", i), minBal(10));

                    depositIntoPool(acc, btc, usd);
                    depositIntoPool(acc, eur, usd);
                    depositIntoPool(acc, btc, eur);

                    // revoke last account
                    if (i == 2)
                    {
                        checkLiquidityPool(*app, poolBtcUsd, 600, 150, 300, 3);
                        checkLiquidityPool(*app, poolEurUsd, 600, 150, 300, 3);
                        checkLiquidityPool(*app, poolBtcEur, 600, 150, 300, 3);

                        revoke(acc, btc, {shareBtcUsd, shareBtcEur}, 4);

                        auto revokeSeqNum = root.getLastSequenceNumber();
                        root.allowTrust(btc, acc);

                        redeemBalance(false, acc, root, btc, poolBtcUsd,
                                      revokeSeqNum, 0, 200);
                        redeemBalance(false, acc, root, btc, poolBtcEur,
                                      revokeSeqNum, 0, 200);
                        redeemBalance(false, acc, root, usd, poolBtcUsd,
                                      revokeSeqNum, 0, 50);
                        redeemBalance(false, acc, root, eur, poolBtcEur,
                                      revokeSeqNum, 0, 50);

                        checkPoolUseCounts(acc, btc, 0);
                        checkPoolUseCounts(acc, usd, 1);
                        checkPoolUseCounts(acc, eur, 1);
                    }
                }

                checkLiquidityPool(*app, poolBtcUsd, 400, 100, 200, 2);
                checkLiquidityPool(*app, poolEurUsd, 600, 150, 300, 3);
                checkLiquidityPool(*app, poolBtcEur, 400, 100, 200, 2);
            }

            SECTION("sponsorships")
            {
                auto acc2 = root.create("acc2", lm.getLastMinBalance(3));
                auto acc3 = root.create("acc3", lm.getLastMinBalance(3));

                auto depositIntoMaybeSponsoredPoolShare =
                    [&](bool poolShareTrustlineIsSponsored) {
                        acc1.changeTrust(cur1, 10);
                        acc1.changeTrust(cur2, 10);
                        root.pay(acc1, cur1, 10);
                        root.pay(acc1, cur2, 10);

                        if (poolShareTrustlineIsSponsored)
                        {
                            auto tx = transactionFrameFromOps(
                                app->getNetworkID(), acc3,
                                {acc3.op(beginSponsoringFutureReserves(acc1)),
                                 acc1.op(changeTrust(share12, 10)),
                                 acc1.op(endSponsoringFutureReserves())},
                                {acc1});

                            {
                                LedgerTxn ltx(app->getLedgerTxnRoot());
                                TransactionMetaFrame txm(
                                    ltx.loadHeader().current().ledgerVersion);
                                REQUIRE(tx->checkValidForTesting(
                                    app->getAppConnector(), ltx, 0, 0, 0));
                                REQUIRE(tx->apply(app->getAppConnector(), ltx,
                                                  txm));
                                REQUIRE(tx->getResultCode() == txSUCCESS);
                                ltx.commit();
                            }

                            {
                                LedgerTxn ltx(app->getLedgerTxnRoot());
                                auto tlAsset =
                                    changeTrustAssetToTrustLineAsset(share12);
                                checkSponsorship(ltx,
                                                 trustlineKey(acc1, tlAsset), 1,
                                                 &acc3.getPublicKey());
                                checkSponsorship(ltx, acc1, 0, nullptr, 4, 2, 0,
                                                 2);
                                checkSponsorship(ltx, acc3, 0, nullptr, 0, 2, 2,
                                                 0);
                            }

                            acc3.pay(root, acc3.getAvailableBalance() - txFee);
                        }
                        else
                        {
                            acc1.changeTrust(share12, 10);

                            REQUIRE(acc1.getNumSubEntries() == 4);
                        }

                        acc1.liquidityPoolDeposit(pool12, 10, 10, Price{1, 1},
                                                  Price{1, 1});

                        checkLiquidityPool(*app, pool12, 10, 10, 10, 1);

                        // get rid of rest of available native balance
                        acc1.pay(root, acc1.getAvailableBalance() - txFee);
                    };

                auto claimAndValidatePoolCounters =
                    [&](TestAccount& txSourceAcc, uint32_t opIndex) {
                        auto revokeSeqNum = txSourceAcc.getLastSequenceNumber();

                        // pay acc1 so it can pay the fee to claim the balances
                        root.pay(acc1, lm.getLastMinBalance(2));

                        root.allowTrust(cur1, acc1);

                        redeemBalance(false, acc1, txSourceAcc, cur1, pool12,
                                      revokeSeqNum, opIndex, 10);
                        redeemBalance(false, acc1, txSourceAcc, cur2, pool12,
                                      revokeSeqNum, opIndex, 10);

                        checkPoolUseCounts(acc1, cur1, 0);
                        checkPoolUseCounts(acc1, cur2, 0);

                        REQUIRE(!acc1.hasTrustLine(
                            changeTrustAssetToTrustLineAsset(share12)));

                        {
                            LedgerTxn ltx(app->getLedgerTxnRoot());
                            REQUIRE(!loadLiquidityPool(ltx, pool12));
                        }
                    };

                auto submitRevokeInSandwich =
                    [&](TestAccount& sponsoringAcc, TestAccount& sponsoredAcc,
                        bool success, int32_t numPoolShareTrustlines) {
                        auto op =
                            flagOp == TrustFlagOp::ALLOW_TRUST
                                ? allowTrust(acc1, cur1, 0)
                                : setTrustLineFlags(acc1, cur1,
                                                    clearTrustLineFlags(
                                                        TRUSTLINE_AUTH_FLAGS));

                        std::vector<SecretKey> opKeys = {sponsoredAcc};
                        if (sponsoringAcc.getAccountId() != root.getAccountId())
                        {
                            opKeys.emplace_back(root);
                        }

                        auto tx = transactionFrameFromOps(
                            app->getNetworkID(), sponsoringAcc,
                            {sponsoringAcc.op(
                                 beginSponsoringFutureReserves(sponsoredAcc)),
                             root.op(op),
                             sponsoredAcc.op(endSponsoringFutureReserves())},
                            opKeys);

                        auto preRevokeNumSubEntries = acc1.getNumSubEntries();
                        auto preRevokeNumSponsoring =
                            getNumSponsoring(*app, sponsoringAcc);
                        bool trustlineIsSponsored =
                            getNumSponsored(*app, acc1) > 0;

                        {
                            LedgerTxn ltx(app->getLedgerTxnRoot());
                            TransactionMetaFrame txm(
                                ltx.loadHeader().current().ledgerVersion);
                            REQUIRE(tx->checkValidForTesting(
                                app->getAppConnector(), ltx, 0, 0, 0));
                            REQUIRE(tx->apply(app->getAppConnector(), ltx,
                                              txm) == success);

                            if (success)
                            {
                                ltx.commit();
                                REQUIRE(tx->getResultCode() == txSUCCESS);
                            }
                            else
                            {
                                REQUIRE(tx->getResultCode() == txFAILED);
                                if (flagOp == TrustFlagOp::ALLOW_TRUST)
                                {
                                    REQUIRE(tx->getResult()
                                                .result.results()[1]
                                                .tr()
                                                .allowTrustResult()
                                                .code() ==
                                            ALLOW_TRUST_LOW_RESERVE);
                                }
                                else
                                {
                                    REQUIRE(tx->getResult()
                                                .result.results()[1]
                                                .tr()
                                                .setTrustLineFlagsResult()
                                                .code() ==
                                            SET_TRUST_LINE_FLAGS_LOW_RESERVE);
                                }
                            }
                        }

                        if (success)
                        {
                            REQUIRE(preRevokeNumSubEntries ==
                                    acc1.getNumSubEntries() +
                                        numPoolShareTrustlines * 2);

                            // sponsoringAcc will be sponsoring the two
                            // claimable balances, but if the same account was
                            // sponsoring the pool share trustline, then the
                            // delta is 0
                            auto delta = trustlineIsSponsored &&
                                                 sponsoringAcc.getPublicKey() ==
                                                     acc3.getPublicKey()
                                             ? 0
                                             : 2;
                            REQUIRE(preRevokeNumSponsoring ==
                                    getNumSponsoring(*app, sponsoringAcc) -
                                        delta);
                        }
                    };

                auto increaseReserve = [&]() {
                    // double the reserve
                    auto newReserve = lm.getLastReserve() * 2;
                    REQUIRE(
                        executeUpgrade(*app, makeBaseReserveUpgrade(newReserve))
                            .baseReserve == newReserve);
                };

                // same reserves
                SECTION("same reserve - no sandwich on revoke")
                {
                    depositIntoMaybeSponsoredPoolShare(false);

                    root.denyTrust(cur1, acc1, flagOp);
                    claimAndValidatePoolCounters(root, 0);
                }
                SECTION("same reserve - sponsored pool share trustline - no "
                        "sandwich on revoke")
                {
                    depositIntoMaybeSponsoredPoolShare(true);

                    root.denyTrust(cur1, acc1, flagOp);
                    {
                        LedgerTxn ltx(app->getLedgerTxnRoot());
                        // verify that acc1 lost two numSubEntries and 2
                        // numSponsored due to the deleted sponsored pool share
                        // trustline, while acc3 ends up with the same state due
                        // to the two claimable balances.
                        checkSponsorship(ltx, acc1, 0, nullptr, 2, 2, 0, 0);
                        checkSponsorship(ltx, acc3, 0, nullptr, 0, 2, 2, 0);
                    }

                    claimAndValidatePoolCounters(root, 0);
                }
                SECTION("same reserve - sponsored account is the sponsor "
                        "of the pool share trustline")
                {
                    depositIntoMaybeSponsoredPoolShare(true);

                    submitRevokeInSandwich(acc2, acc3, true, 1);
                    claimAndValidatePoolCounters(acc2, 1);
                }
                SECTION("same reserve - sandwich on revoke - success")
                {
                    depositIntoMaybeSponsoredPoolShare(false);
                    submitRevokeInSandwich(acc2, acc1, true, 1);
                    claimAndValidatePoolCounters(acc2, 1);
                }
                SECTION("same reserve - issuer sandwich on revoke - success")
                {
                    depositIntoMaybeSponsoredPoolShare(false);

                    submitRevokeInSandwich(root, acc1, true, 1);
                    claimAndValidatePoolCounters(root, 1);
                }
                SECTION("same reserve - issuer sandwich on revoke - fail")
                {
                    depositIntoMaybeSponsoredPoolShare(false);

                    // leave enough to pay for this tx and the sponsorship
                    // sandwich
                    root.pay(acc2, root.getAvailableBalance() - txFee * 4);
                    submitRevokeInSandwich(root, acc1, false, 1);
                }
                SECTION("same reserve - sandwich on revoke - fail")
                {
                    depositIntoMaybeSponsoredPoolShare(false);

                    // leave enough to pay for this tx and the sponsorship
                    // sandwich
                    acc2.pay(root, acc2.getAvailableBalance() - txFee * 4);
                    submitRevokeInSandwich(acc2, acc1, false, 1);
                }
                SECTION("same reserve - sponsoring account is the sponsor of "
                        "the pool share trustline")
                {
                    depositIntoMaybeSponsoredPoolShare(true);

                    // acc3 is the sponsor of the pool share trustline
                    root.pay(acc3, lm.getLastMinBalance(1));
                    submitRevokeInSandwich(acc3, acc1, true, 1);
                    claimAndValidatePoolCounters(acc3, 1);
                }
                SECTION("same reserve - multiple pool share trustlines - one "
                        "sponsored pool share trustline - "
                        "sandwich on revoke")
                {
                    depositIntoMaybeSponsoredPoolShare(true);

                    // pay acc1 so it can deposit into another pool
                    root.pay(acc1, lm.getLastMinBalance(6));
                    depositIntoPool(acc1, native, cur1);

                    submitRevokeInSandwich(acc2, acc3, true, 2);

                    auto revokeSeqNum = acc2.getLastSequenceNumber();

                    root.allowTrust(cur1, acc1);

                    redeemBalance(false, acc1, acc2, cur1, pool12, revokeSeqNum,
                                  1, 10);
                    redeemBalance(false, acc1, acc2, cur2, pool12, revokeSeqNum,
                                  1, 10);

                    redeemBalance(false, acc1, acc2, cur1, poolNative1,
                                  revokeSeqNum, 1, 50);
                    redeemBalance(false, acc1, acc2, native, poolNative1,
                                  revokeSeqNum, 1, 200);

                    checkPoolUseCounts(acc1, cur1, 0);
                    checkPoolUseCounts(acc1, cur2, 0);

                    REQUIRE(!acc1.hasTrustLine(
                        changeTrustAssetToTrustLineAsset(share12)));
                    REQUIRE(!acc1.hasTrustLine(
                        changeTrustAssetToTrustLineAsset(shareNative1)));

                    {
                        LedgerTxn ltx(app->getLedgerTxnRoot());
                        REQUIRE(!loadLiquidityPool(ltx, pool12));
                        REQUIRE(!loadLiquidityPool(ltx, poolNative1));

                        checkSponsorship(ltx, acc1, 0, nullptr, 2, 2, 0, 0);
                        checkSponsorship(ltx, acc2, 0, nullptr, 0, 2, 0, 0);
                        checkSponsorship(ltx, acc3, 0, nullptr, 0, 2, 0, 0);
                    }
                }

                // upgrade reserves
                SECTION("increase reserve - no sandwich on revoke - success")
                {
                    depositIntoMaybeSponsoredPoolShare(false);
                    increaseReserve();

                    root.denyTrust(cur1, acc1, flagOp);
                    claimAndValidatePoolCounters(root, 0);
                }
                SECTION("increase reserve - sandwich on revoke - success")
                {
                    depositIntoMaybeSponsoredPoolShare(false);
                    increaseReserve();

                    root.pay(acc2, lm.getLastMinBalance(1));
                    submitRevokeInSandwich(acc2, acc1, true, 1);
                    claimAndValidatePoolCounters(acc2, 1);
                }
                SECTION(
                    "increase reserve - issuer sandwich on revoke - success")
                {
                    depositIntoMaybeSponsoredPoolShare(false);
                    increaseReserve();

                    submitRevokeInSandwich(root, acc1, true, 1);
                    claimAndValidatePoolCounters(root, 1);
                }
                SECTION("increase reserve - issuer sandwich on revoke - fail")
                {
                    depositIntoMaybeSponsoredPoolShare(false);
                    root.pay(acc2, root.getAvailableBalance() -
                                       lm.getLastMinBalance(1));

                    increaseReserve();
                    submitRevokeInSandwich(root, acc1, false, 1);
                }
                SECTION("increase reserve - sandwich on revoke - fail")
                {
                    depositIntoMaybeSponsoredPoolShare(false);
                    increaseReserve();

                    submitRevokeInSandwich(acc2, acc1, false, 1);
                }
                SECTION("increase reserve - sponsored account is the sponsor "
                        "of the pool share trustline - fail")
                {
                    depositIntoMaybeSponsoredPoolShare(true);
                    increaseReserve();

                    submitRevokeInSandwich(acc2, acc3, false, 1);
                }
                SECTION("increase reserve - sponsoring account is the sponsor "
                        "of the pool share trustline")
                {
                    depositIntoMaybeSponsoredPoolShare(true);
                    increaseReserve();

                    // acc3 is the sponsor of the pool share trustline
                    root.pay(acc3, lm.getLastMinBalance(1));
                    submitRevokeInSandwich(acc3, acc1, true, 1);
                    claimAndValidatePoolCounters(acc3, 1);
                }

                SECTION("trustline is owned by one issuer and sponsored by the "
                        "other")
                {
                    auto usd = makeAsset(acc1, "USD");

                    auto share1Usd = makeChangeTrustAssetPoolShare(
                        cur1, usd, LIQUIDITY_POOL_FEE_V18);
                    auto poolID = xdrSha256(share1Usd.liquidityPool());

                    acc1.changeTrust(cur1, 100);
                    root.pay(acc1, cur1, 10);

                    {
                        auto tx = transactionFrameFromOps(
                            app->getNetworkID(), root,
                            {root.op(beginSponsoringFutureReserves(acc1)),
                             acc1.op(changeTrust(share1Usd, 10)),
                             acc1.op(endSponsoringFutureReserves())},
                            {acc1});

                        LedgerTxn ltx(app->getLedgerTxnRoot());
                        TransactionMetaFrame txm(
                            ltx.loadHeader().current().ledgerVersion);
                        REQUIRE(tx->checkValidForTesting(app->getAppConnector(),
                                                         ltx, 0, 0, 0));
                        REQUIRE(tx->apply(app->getAppConnector(), ltx, txm));
                        REQUIRE(tx->getResultCode() == txSUCCESS);

                        auto tlAsset =
                            changeTrustAssetToTrustLineAsset(share1Usd);
                        checkSponsorship(ltx, trustlineKey(acc1, tlAsset), 1,
                                         &root.getPublicKey());
                        checkSponsorship(ltx, acc1, 0, nullptr, 3, 2, 0, 2);
                        checkSponsorship(ltx, root, 0, nullptr, 0, 2, 2, 0);

                        ltx.commit();
                    }

                    acc1.liquidityPoolDeposit(poolID, 10, 10, Price{1, 1},
                                              Price{1, 1});

                    auto preRevokeNumSponsoring = getNumSponsoring(*app, root);

                    // No claimable balances are sponsored by acc1, which is why
                    // 0 is passed in here
                    revoke(acc1, cur1, {share1Usd}, 0);

                    // root is no longer sponsoring the trustline, but it is
                    // sponsoring one claimable balance after the revoke
                    REQUIRE(preRevokeNumSponsoring ==
                            getNumSponsoring(*app, root) + 1);

                    checkPoolUseCounts(acc1, cur1, 0);

                    // Pool should be deleted since the last pool share
                    // trustline was deleted
                    {
                        LedgerTxn ltx(app->getLedgerTxnRoot());
                        REQUIRE(!loadLiquidityPool(ltx, poolID));
                    }

                    auto revokeSeqNum = root.getLastSequenceNumber();
                    root.allowTrust(cur1, acc1);

                    redeemBalance(false, acc1, root, cur1, poolID, revokeSeqNum,
                                  0, 10);

                    auto usdBalanceID = getRevokeBalanceID(
                        root, root.getLastSequenceNumber(), usd, poolID, 0);

                    REQUIRE_THROWS_AS(
                        acc1.claimClaimableBalance(usdBalanceID),
                        ex_CLAIM_CLAIMABLE_BALANCE_DOES_NOT_EXIST);

                    checkNumSponsoring(*app, acc1, 0);
                }
            }
        };

        SECTION("revoke with set trustline flags")
        {
            revokeTest(TrustFlagOp::SET_TRUST_LINE_FLAGS);
        }
        SECTION("revoke with allow trust")
        {
            revokeTest(TrustFlagOp::ALLOW_TRUST);
        }
    });

    SECTION("too many sponsoring")
    {
        for_versions_from(18, *app, [&] {
            SECTION("one claimable balance")
            {
                // use two assets issued by acc1. No claimable balances will be
                // created for them on revoke
                auto eur = makeAsset(acc1, "eur");
                auto usd = makeAsset(acc1, "usd");

                depositIntoPool(acc1, cur1, eur);
                depositIntoPool(acc1, cur2, usd);

                SECTION("allow trust")
                {
                    tooManySponsoring(*app, acc1,
                                      root.op(allowTrust(acc1, cur1, 0)),
                                      root.op(allowTrust(acc1, cur2, 0)), 1);
                }
                SECTION("set trustline flags")
                {
                    tooManySponsoring(
                        *app, acc1,
                        root.op(setTrustLineFlags(
                            acc1, cur1, clearTrustLineFlags(AUTHORIZED_FLAG))),
                        root.op(setTrustLineFlags(
                            acc1, cur2, clearTrustLineFlags(AUTHORIZED_FLAG))),
                        1);
                }
            }

            SECTION("two claimable balances")
            {
                auto cur3 = makeAsset(root, "CUR3");

                depositIntoPool(acc1, cur1, cur2);
                depositIntoPool(acc1, cur2, cur3);

                SECTION("allow trust")
                {
                    tooManySponsoring(*app, acc1,
                                      root.op(allowTrust(acc1, cur1, 0)),
                                      root.op(allowTrust(acc1, cur2, 0)), 2);
                }
                SECTION("set trustline flags")
                {
                    tooManySponsoring(
                        *app, acc1,
                        root.op(setTrustLineFlags(
                            acc1, cur1, clearTrustLineFlags(AUTHORIZED_FLAG))),
                        root.op(setTrustLineFlags(
                            acc1, cur2, clearTrustLineFlags(AUTHORIZED_FLAG))),
                        2);
                }
            }
        });
    }
}