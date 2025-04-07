// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "SorobanTxTestUtils.h"
#include "lib/catch.hpp"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/TransactionUtils.h"
#include "util/Logging.h"
#include "util/ProtocolVersion.h"
#include <xdrpp/printer.h>

using namespace stellar;
using namespace stellar::txtest;

TEST_CASE_VERSIONS("payment events", "[tx][event]")
{
    Config cfg = getTestConfig(0, Config::TESTDB_IN_MEMORY);
    cfg.EMIT_CLASSIC_EVENTS = true;
    cfg.BACKFILL_STELLAR_ASSET_EVENTS = true;

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);

    // set up world
    auto root = app->getRoot();

    const int64_t paymentAmount = app->getLedgerManager().getLastReserve() * 10;

    auto gateway = root->create("gate", paymentAmount);
    Asset idr = makeAsset(gateway, "IDR");
    auto lumenContractID =
        getLumenContractInfo(app->getNetworkID()).mLumenContractID;
    auto idrContractID = getAssetContractID(app->getNetworkID(), idr);

    SECTION("a pays b")
    {
        auto a1 = root->create("A", paymentAmount);
        auto amount = app->getLedgerManager().getLastMinBalance(0) + 1000000;
        auto b1 = root->create("B", amount);
        auto a1ID = KeyUtils::fromStrKey<PublicKey>(a1.getAccountId());
        auto b1ID = KeyUtils::fromStrKey<PublicKey>(b1.getAccountId());

        for_all_versions(*app, [&] {
            int64_t amt = 200;
            auto txFrame = a1.tx({payment(b1, amt)});
            auto resultSet = closeLedger(*app, {txFrame});
            REQUIRE(txFrame->getResultCode() == txSUCCESS);

            auto lastMeta =
                app->getLedgerManager().getLastClosedLedgerTxMeta()[0];
            auto paymentEvent = app->getLedgerManager()
                                    .getLastClosedLedgerTxMeta()[0]
                                    .getOpEventsAtOp(0)[0];
            LOG_INFO(DEFAULT_LOG, "events: {}",
                     xdr::xdr_to_string(paymentEvent));
            auto expectedEvent = makeTransferEvent(
                lumenContractID, Asset(AssetType::ASSET_TYPE_NATIVE),
                makeAccountAddress(a1ID), makeAccountAddress(b1ID), amt,
                std::nullopt, std::nullopt);
            REQUIRE(paymentEvent == expectedEvent);
        });
    }

    SECTION("a pays b, a is mux")
    {
        auto a = root->create("A", paymentAmount);
        auto amount = app->getLedgerManager().getLastMinBalance(0) + 1000000;
        auto b = root->create("B", amount);
        auto aID = KeyUtils::fromStrKey<PublicKey>(a.getAccountId());
        auto bID = KeyUtils::fromStrKey<PublicKey>(b.getAccountId());

        for_versions_from(13, *app, [&] {
            int64_t amt = 200;
            uint64_t aMemoID = 1;
            auto txFrame = transactionFromOperationsV1(
                *app, a, a.nextSequenceNumber(), {payment(b, amt)}, 0 /*fee*/,
                std::nullopt, aMemoID);
            auto resultSet = closeLedger(*app, {txFrame});
            REQUIRE(txFrame->getResultCode() == txSUCCESS);

            auto lastMeta =
                app->getLedgerManager().getLastClosedLedgerTxMeta()[0];
            auto paymentEvent = app->getLedgerManager()
                                    .getLastClosedLedgerTxMeta()[0]
                                    .getOpEventsAtOp(0)[0];
            LOG_INFO(DEFAULT_LOG, "events: {}",
                     xdr::xdr_to_string(paymentEvent));
            auto expectedEvent = makeTransferEvent(
                lumenContractID, Asset(AssetType::ASSET_TYPE_NATIVE),
                makeAccountAddress(aID), makeAccountAddress(bID), amt, aMemoID,
                std::nullopt);
            REQUIRE(paymentEvent == expectedEvent);
        });
    }

    SECTION("a pays b, b is mux")
    {
        auto toMux = [](MuxedAccount& id, uint64 memoID) {
            MuxedAccount muxedID(KEY_TYPE_MUXED_ED25519);
            auto& mid = muxedID.med25519();
            mid.ed25519 = id.ed25519();
            mid.id = memoID;
            id = muxedID;
        };

        auto a = root->create("A", paymentAmount);
        auto amount = app->getLedgerManager().getLastMinBalance(0) + 1000000;
        auto b = root->create("B", amount);

        auto aID = KeyUtils::fromStrKey<PublicKey>(a.getAccountId());
        auto bID = KeyUtils::fromStrKey<PublicKey>(b.getAccountId());

        for_versions_from(13, *app, [&] {
            int64_t amt = 200;
            uint64_t bMemoID = 2;
            Operation op = payment(b, amt);
            auto& recv = op.body.paymentOp();
            toMux(recv.destination, 2);

            auto txFrame = a.tx({op});
            auto resultSet = closeLedger(*app, {txFrame});
            REQUIRE(txFrame->getResultCode() == txSUCCESS);

            auto lastMeta =
                app->getLedgerManager().getLastClosedLedgerTxMeta()[0];
            auto paymentEvent = app->getLedgerManager()
                                    .getLastClosedLedgerTxMeta()[0]
                                    .getOpEventsAtOp(0)[0];
            LOG_INFO(DEFAULT_LOG, "events: {}",
                     xdr::xdr_to_string(paymentEvent));

            auto expectedEvent = makeTransferEvent(
                lumenContractID, Asset(AssetType::ASSET_TYPE_NATIVE),
                makeAccountAddress(aID), makeAccountAddress(bID), amt,
                std::nullopt, bMemoID);
            REQUIRE(paymentEvent == expectedEvent);
        });
    }

    SECTION("with tx memo")
    {
        // regular accounts, with tx memo set
        auto a = root->create("A", paymentAmount);
        auto amount = app->getLedgerManager().getLastMinBalance(0) + 1000000;
        auto b = root->create("B", amount);
        auto a1ID = KeyUtils::fromStrKey<PublicKey>(a.getAccountId());
        auto b1ID = KeyUtils::fromStrKey<PublicKey>(b.getAccountId());

        for_all_versions(*app, [&] {
            auto memo = Memo(MemoType::MEMO_TEXT);
            auto amt = 200;
            memo.text() = "test memo";

            auto txFrame = transactionFromOperationsV1(
                *app, a, a.nextSequenceNumber(), {payment(b, amt)}, 0 /*fee*/,
                std::nullopt, std::nullopt, memo);
            auto resultSet = closeLedger(*app, {txFrame});
            REQUIRE(txFrame->getResultCode() == txSUCCESS);

            auto lastMeta =
                app->getLedgerManager().getLastClosedLedgerTxMeta()[0];
            auto paymentEvent = app->getLedgerManager()
                                    .getLastClosedLedgerTxMeta()[0]
                                    .getOpEventsAtOp(0)[0];
            LOG_INFO(DEFAULT_LOG, "events: {}",
                     xdr::xdr_to_string(paymentEvent));

            auto expectedEvent = makeTransferEvent(
                lumenContractID, Asset(AssetType::ASSET_TYPE_NATIVE),
                makeAccountAddress(a1ID), makeAccountAddress(b1ID), amt,
                std::nullopt, std::nullopt, memo);
            REQUIRE(paymentEvent == expectedEvent);
        });
    }

    // SECTION("test order of precedence")
    // {
    //     // all three set
    //     // two out of three set
    // }

    SECTION("payer is issuer")
    {
        auto a1 = root->create("A", paymentAmount);
        a1.changeTrust(idr, 1000);
        auto amt = 200;
        auto a1ID = KeyUtils::fromStrKey<PublicKey>(a1.getAccountId());

        for_all_versions(*app, [&] {
            auto txFrame = transactionFromOperationsV1(
                *app, gateway, gateway.nextSequenceNumber(),
                {payment(a1, idr, amt)}, 0 /*fee*/);
            auto resultSet = closeLedger(*app, {txFrame});
            REQUIRE(txFrame->getResultCode() == txSUCCESS);

            auto lastMeta =
                app->getLedgerManager().getLastClosedLedgerTxMeta()[0];
            auto mintEvent = app->getLedgerManager()
                                 .getLastClosedLedgerTxMeta()[0]
                                 .getOpEventsAtOp(0)[0];
            LOG_INFO(DEFAULT_LOG, "events: {}", xdr::xdr_to_string(mintEvent));

            auto expectedEvent = makeMintOrBurnEvent(
                true, idrContractID, idr, makeAccountAddress(a1ID), amt);
            REQUIRE(mintEvent == expectedEvent);
        });
    }

    SECTION("payee is issuer")
    {
        auto a1 = root->create("A", paymentAmount);
        a1.changeTrust(idr, 1000);
        gateway.pay(a1, idr, 1000);
        auto amt = 200;
        auto a1ID = KeyUtils::fromStrKey<PublicKey>(a1.getAccountId());

        for_all_versions(*app, [&] {
            auto txFrame = transactionFromOperationsV1(
                *app, a1, a1.nextSequenceNumber(), {payment(gateway, idr, amt)},
                0 /*fee*/);
            auto resultSet = closeLedger(*app, {txFrame});
            REQUIRE(txFrame->getResultCode() == txSUCCESS);

            auto lastMeta =
                app->getLedgerManager().getLastClosedLedgerTxMeta()[0];
            auto burnEvent = app->getLedgerManager()
                                 .getLastClosedLedgerTxMeta()[0]
                                 .getOpEventsAtOp(0)[0];
            LOG_INFO(DEFAULT_LOG, "events: {}", xdr::xdr_to_string(burnEvent));

            auto expectedEvent = makeMintOrBurnEvent(
                false, idrContractID, idr, makeAccountAddress(a1ID), amt);
            REQUIRE(burnEvent == expectedEvent);
        });
    }
}

// TEST_CASE_VERSIONS("claimable balance", "[tx][event]")
// {

// }

// TEST_CASE_VERSIONS("claim claimable balance", "[tx][event]")
// {

// }

// TEST_CASE_VERSIONS("clawback events", "[tx][event]")
// {

// }
// TEST_CASE_VERSIONS("create account", "[tx][event]")
// {

// }
// TEST_CASE_VERSIONS("inflation op", "[tx][event]")
// {

// }
// TEST_CASE_VERSIONS("liquidity pool", "[tx][event]")
// {
//     // create
//     // withdraw
// }
// TEST_CASE_VERSIONS("lumen mint event", "[tx][event]")
// {

// }
// TEST_CASE_VERSIONS("manage offer", "[tx][event]")
// {

// }
// TEST_CASE_VERSIONS("merge account", "[tx][event]")
// {

// }
// TEST_CASE_VERSIONS("some nontrial path payment", "[tx][event]")
// {

// }
// TEST_CASE_VERSIONS("trust flags op", "[tx][event]")
// {

// }
// TEST_CASE_VERSIONS("soroban event pre23", "[tx][event]")
// {

// }