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

    SECTION("a pays b. Also validate fee event")
    {
        auto a1 = root->create("A", paymentAmount);
        auto amount = app->getLedgerManager().getLastMinBalance(0) + 1000000;
        auto b1 = root->create("B", amount);
        auto a1ID = KeyUtils::fromStrKey<PublicKey>(a1.getAccountId());
        auto b1ID = KeyUtils::fromStrKey<PublicKey>(b1.getAccountId());

        for_all_versions(*app, [&] {
            auto prePaymentA1Balance = a1.getBalance();
            int64_t amt = 200;
            auto txFrame = a1.tx({payment(b1, amt)});
            auto resultSet = closeLedger(*app, {txFrame});
            REQUIRE(txFrame->getResultCode() == txSUCCESS);

            auto lastMeta =
                app->getLedgerManager().getLastClosedLedgerTxMeta()[0];

            auto opEvents = app->getLedgerManager()
                                .getLastClosedLedgerTxMeta()[0]
                                .getOpEventsAtOp(0);
            xdr::xvector<ContractEvent> expected = {makeTransferEvent(
                lumenContractID, Asset(AssetType::ASSET_TYPE_NATIVE),
                makeAccountAddress(a1ID), makeAccountAddress(b1ID), amt)};
            REQUIRE(opEvents == expected);

            auto const& txEvents = app->getLedgerManager()
                                       .getLastClosedLedgerTxMeta()[0]
                                       .getTxEvents();
            REQUIRE(txEvents.size() == 1);
            validateFeeEvent(txEvents[0], a1,
                             prePaymentA1Balance - amt - a1.getBalance());
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
            auto opEvents = app->getLedgerManager()
                                .getLastClosedLedgerTxMeta()[0]
                                .getOpEventsAtOp(0);
            xdr::xvector<ContractEvent> expected = {makeTransferEvent(
                lumenContractID, Asset(AssetType::ASSET_TYPE_NATIVE),
                makeAccountAddress(aID), makeAccountAddress(bID), amt)};
            REQUIRE(opEvents == expected);
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
            auto opEvents = app->getLedgerManager()
                                .getLastClosedLedgerTxMeta()[0]
                                .getOpEventsAtOp(0);

            xdr::xvector<ContractEvent> expected = {makeTransferEvent(
                lumenContractID, Asset(AssetType::ASSET_TYPE_NATIVE),
                makeAccountAddress(aID), makeAccountAddress(bID), amt,
                SCMapEntry(makeSymbolSCVal("to_muxed_id"), makeU64(bMemoID)))};
            REQUIRE(opEvents == expected);
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

            auto txFrame =
                transactionFromOperations(*app, a, a.nextSequenceNumber(),
                                          {payment(b, amt)}, 0 /*fee*/, memo);
            auto resultSet = closeLedger(*app, {txFrame});
            REQUIRE(txFrame->getResultCode() == txSUCCESS);

            auto lastMeta =
                app->getLedgerManager().getLastClosedLedgerTxMeta()[0];
            auto opEvents = app->getLedgerManager()
                                .getLastClosedLedgerTxMeta()[0]
                                .getOpEventsAtOp(0);
            xdr::xvector<ContractEvent> expected = {makeTransferEvent(
                lumenContractID, Asset(AssetType::ASSET_TYPE_NATIVE),
                makeAccountAddress(a1ID), makeAccountAddress(b1ID), amt,
                SCMapEntry(makeSymbolSCVal("to_muxed_id"),
                           makeClassicMemoSCVal(memo)))};
            REQUIRE(opEvents == expected);
        });
    }

    SECTION("memo order of precedence")
    {
        // test memo order of precedence
        // in a transfer event, if both the receipient memo (toMemoID) and the
        // transaction memo are set, it follows order of precedence and the
        // toMemoID will be taken
        auto a = root->create("A", paymentAmount);
        auto amount = app->getLedgerManager().getLastMinBalance(0) + 1000000;
        auto b = root->create("B", amount);
        auto aID = KeyUtils::fromStrKey<PublicKey>(a.getAccountId());
        auto bID = KeyUtils::fromStrKey<PublicKey>(b.getAccountId());

        auto toMux = [](MuxedAccount& id, uint64 memoID) {
            MuxedAccount muxedID(KEY_TYPE_MUXED_ED25519);
            auto& mid = muxedID.med25519();
            mid.ed25519 = id.ed25519();
            mid.id = memoID;
            id = muxedID;
        };
        for_versions_from(13, *app, [&] {
            int64_t amt = 200;

            // Create a transaction with both a recipient memo and a transaction
            // memo
            uint64_t recipientMemoID = 42;
            Memo txMemo(MemoType::MEMO_TEXT);
            txMemo.text() = "transaction memo";

            // Create a payment operation with a recipient memo
            Operation op = payment(b, amt);
            auto& recv = op.body.paymentOp();
            toMux(recv.destination, recipientMemoID);

            // Create a transaction with the operation and a transaction memo
            auto txFrame = transactionFromOperations(
                *app, a, a.nextSequenceNumber(), {op}, 0 /*fee*/, txMemo);
            auto resultSet = closeLedger(*app, {txFrame});
            REQUIRE(txFrame->getResultCode() == txSUCCESS);

            // Get the operation events
            auto opEvents = app->getLedgerManager()
                                .getLastClosedLedgerTxMeta()[0]
                                .getOpEventsAtOp(0);

            // Verify that the recipient memo (toMemoID) takes precedence over
            // the transaction memo
            xdr::xvector<ContractEvent> expected = {makeTransferEvent(
                lumenContractID, Asset(AssetType::ASSET_TYPE_NATIVE),
                makeAccountAddress(aID), makeAccountAddress(bID), amt,
                SCMapEntry(makeSymbolSCVal("to_muxed_id"),
                           makeU64(recipientMemoID)))};
            REQUIRE(opEvents == expected);
        });
    }

    SECTION("mint event (payer is issuer)")
    {
        auto a1 = root->create("A", paymentAmount);
        a1.changeTrust(idr, 1000);
        auto amt = 200;
        auto a1ID = KeyUtils::fromStrKey<PublicKey>(a1.getAccountId());

        for_all_versions(*app, [&] {
            auto txFrame = transactionFromOperations(
                *app, gateway, gateway.nextSequenceNumber(),
                {payment(a1, idr, amt)}, 0 /*fee*/);
            auto resultSet = closeLedger(*app, {txFrame});
            REQUIRE(txFrame->getResultCode() == txSUCCESS);

            auto lastMeta =
                app->getLedgerManager().getLastClosedLedgerTxMeta()[0];
            auto opEvents = app->getLedgerManager()
                                .getLastClosedLedgerTxMeta()[0]
                                .getOpEventsAtOp(0);

            xdr::xvector<ContractEvent> expected = {makeMintOrBurnEvent(
                true, idrContractID, idr, makeAccountAddress(a1ID), amt)};
            REQUIRE(opEvents == expected);
        });
    }

    SECTION("mint event with memo ID")
    {
        auto a1 = root->create("A", paymentAmount);
        a1.changeTrust(idr, 1000);
        auto amt = 200;
        auto a1ID = KeyUtils::fromStrKey<PublicKey>(a1.getAccountId());
        uint64_t aMemoID = 42;

        // Convert the recipient to a muxed account
        auto toMux = [](MuxedAccount& id, uint64 memoID) {
            MuxedAccount muxedID(KEY_TYPE_MUXED_ED25519);
            auto& mid = muxedID.med25519();
            mid.ed25519 = id.ed25519();
            mid.id = memoID;
            id = muxedID;
        };

        for_versions_from(13, *app, [&] {
            // Create a payment operation with a muxed recipient
            Operation op = payment(a1, idr, amt);
            auto& recv = op.body.paymentOp();
            toMux(recv.destination, aMemoID);

            auto txFrame = transactionFromOperations(
                *app, gateway, gateway.nextSequenceNumber(), {op}, 0 /*fee*/);
            auto resultSet = closeLedger(*app, {txFrame});
            REQUIRE(txFrame->getResultCode() == txSUCCESS);

            auto lastMeta =
                app->getLedgerManager().getLastClosedLedgerTxMeta()[0];
            auto opEvents = app->getLedgerManager()
                                .getLastClosedLedgerTxMeta()[0]
                                .getOpEventsAtOp(0);

            xdr::xvector<ContractEvent> expected = {makeMintOrBurnEvent(
                true, idrContractID, idr, makeAccountAddress(a1ID), amt,
                SCMapEntry(makeSymbolSCVal("to_muxed_id"), makeU64(aMemoID)))};
            REQUIRE(opEvents == expected);
        });
    }

    SECTION("burn event (payee is issuer)")
    {
        auto a1 = root->create("A", paymentAmount);
        a1.changeTrust(idr, 1000);
        gateway.pay(a1, idr, 1000);
        auto amt = 200;
        auto a1ID = KeyUtils::fromStrKey<PublicKey>(a1.getAccountId());

        for_all_versions(*app, [&] {
            auto txFrame = transactionFromOperations(
                *app, a1, a1.nextSequenceNumber(), {payment(gateway, idr, amt)},
                0 /*fee*/);
            auto resultSet = closeLedger(*app, {txFrame});
            REQUIRE(txFrame->getResultCode() == txSUCCESS);

            auto lastMeta =
                app->getLedgerManager().getLastClosedLedgerTxMeta()[0];
            auto opEvents = app->getLedgerManager()
                                .getLastClosedLedgerTxMeta()[0]
                                .getOpEventsAtOp(0);

            xdr::xvector<ContractEvent> expected = {makeMintOrBurnEvent(
                false, idrContractID, idr, makeAccountAddress(a1ID), amt)};
            REQUIRE(opEvents == expected);
        });
    }
}
