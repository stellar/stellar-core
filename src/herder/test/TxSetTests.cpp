// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/TxSetFrame.h"
#include "herder/test/TestTxSetUtils.h"
#include "ledger/LedgerManager.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "util/ProtocolVersion.h"

namespace stellar
{
namespace
{
using namespace txtest;

TEST_CASE("generalized tx set XDR validation", "[txset]")
{
    Config cfg(getTestConfig());
    cfg.LEDGER_PROTOCOL_VERSION =
        static_cast<uint32_t>(SOROBAN_PROTOCOL_VERSION);
    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
        static_cast<uint32_t>(SOROBAN_PROTOCOL_VERSION);
    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);

    GeneralizedTransactionSet xdrTxSet(1);
    xdrTxSet.v1TxSet().previousLedgerHash =
        app->getLedgerManager().getLastClosedLedgerHeader().hash;
    LedgerTxn ltx(app->getLedgerTxnRoot());
    SECTION("no phases")
    {
        auto txSet = TxSetXDRFrame::makeFromWire(xdrTxSet);
        REQUIRE(txSet->prepareForApply(*app) == nullptr);
    }
    SECTION("too many phases")
    {
        xdrTxSet.v1TxSet().phases.emplace_back();
        xdrTxSet.v1TxSet().phases.emplace_back();
        xdrTxSet.v1TxSet().phases.emplace_back();
        auto txSet = TxSetXDRFrame::makeFromWire(xdrTxSet);
        REQUIRE(txSet->prepareForApply(*app) == nullptr);
    }
    SECTION("incorrect base fee order")
    {
        xdrTxSet.v1TxSet().phases.emplace_back();
        xdrTxSet.v1TxSet().phases.emplace_back();
        for (int i = 0; i < xdrTxSet.v1TxSet().phases.size(); ++i)
        {
            SECTION("phase " + std::to_string(i))
            {
                SECTION("all components discounted")
                {

                    xdrTxSet.v1TxSet().phases[i].v0Components().emplace_back(
                        TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
                    xdrTxSet.v1TxSet()
                        .phases[i]
                        .v0Components()
                        .back()
                        .txsMaybeDiscountedFee()
                        .baseFee.activate() = 1500;
                    xdrTxSet.v1TxSet()
                        .phases[i]
                        .v0Components()
                        .back()
                        .txsMaybeDiscountedFee()
                        .txs.emplace_back();

                    xdrTxSet.v1TxSet().phases[i].v0Components().emplace_back(
                        TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
                    xdrTxSet.v1TxSet()
                        .phases[i]
                        .v0Components()
                        .back()
                        .txsMaybeDiscountedFee()
                        .baseFee.activate() = 1400;
                    xdrTxSet.v1TxSet()
                        .phases[i]
                        .v0Components()
                        .back()
                        .txsMaybeDiscountedFee()
                        .txs.emplace_back();

                    xdrTxSet.v1TxSet().phases[i].v0Components().emplace_back(
                        TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
                    xdrTxSet.v1TxSet()
                        .phases[i]
                        .v0Components()
                        .back()
                        .txsMaybeDiscountedFee()
                        .baseFee.activate() = 1600;
                    xdrTxSet.v1TxSet()
                        .phases[i]
                        .v0Components()
                        .back()
                        .txsMaybeDiscountedFee()
                        .txs.emplace_back();

                    auto txSet = TxSetXDRFrame::makeFromWire(xdrTxSet);
                    REQUIRE(txSet->prepareForApply(*app) == nullptr);
                }
                SECTION("non-discounted component out of place")
                {

                    xdrTxSet.v1TxSet().phases[i].v0Components().emplace_back(
                        TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
                    xdrTxSet.v1TxSet()
                        .phases[i]
                        .v0Components()
                        .back()
                        .txsMaybeDiscountedFee()
                        .baseFee.activate() = 1500;
                    xdrTxSet.v1TxSet()
                        .phases[i]
                        .v0Components()
                        .back()
                        .txsMaybeDiscountedFee()
                        .txs.emplace_back();

                    xdrTxSet.v1TxSet().phases[i].v0Components().emplace_back(
                        TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
                    xdrTxSet.v1TxSet()
                        .phases[i]
                        .v0Components()
                        .back()
                        .txsMaybeDiscountedFee()
                        .baseFee.activate() = 1600;
                    xdrTxSet.v1TxSet()
                        .phases[i]
                        .v0Components()
                        .back()
                        .txsMaybeDiscountedFee()
                        .txs.emplace_back();

                    xdrTxSet.v1TxSet().phases[i].v0Components().emplace_back(
                        TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);

                    auto txSet = TxSetXDRFrame::makeFromWire(xdrTxSet);
                    REQUIRE(txSet->prepareForApply(*app) == nullptr);
                }
                SECTION(
                    "with non-discounted component, discounted out of place")
                {
                    xdrTxSet.v1TxSet().phases[i].v0Components().emplace_back(
                        TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
                    xdrTxSet.v1TxSet()
                        .phases[i]
                        .v0Components()
                        .back()
                        .txsMaybeDiscountedFee()
                        .txs.emplace_back();

                    xdrTxSet.v1TxSet().phases[i].v0Components().emplace_back(
                        TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
                    xdrTxSet.v1TxSet()
                        .phases[i]
                        .v0Components()
                        .back()
                        .txsMaybeDiscountedFee()
                        .baseFee.activate() = 1500;
                    xdrTxSet.v1TxSet()
                        .phases[i]
                        .v0Components()
                        .back()
                        .txsMaybeDiscountedFee()
                        .txs.emplace_back();

                    xdrTxSet.v1TxSet().phases[i].v0Components().emplace_back(
                        TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
                    xdrTxSet.v1TxSet()
                        .phases[i]
                        .v0Components()
                        .back()
                        .txsMaybeDiscountedFee()
                        .baseFee.activate() = 1400;
                    xdrTxSet.v1TxSet()
                        .phases[i]
                        .v0Components()
                        .back()
                        .txsMaybeDiscountedFee()
                        .txs.emplace_back();

                    auto txSet = TxSetXDRFrame::makeFromWire(xdrTxSet);
                    REQUIRE(txSet->prepareForApply(*app) == nullptr);
                }
            }
        }
    }
    SECTION("duplicate base fee")
    {
        xdrTxSet.v1TxSet().phases.emplace_back();
        xdrTxSet.v1TxSet().phases.emplace_back();
        for (int i = 0; i < xdrTxSet.v1TxSet().phases.size(); ++i)
        {
            SECTION("phase " + std::to_string(i))
            {
                SECTION("duplicate discounts")
                {
                    xdrTxSet.v1TxSet().phases[i].v0Components().emplace_back(
                        TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
                    xdrTxSet.v1TxSet()
                        .phases[i]
                        .v0Components()
                        .back()
                        .txsMaybeDiscountedFee()
                        .txs.emplace_back();

                    xdrTxSet.v1TxSet().phases[i].v0Components().emplace_back(
                        TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
                    xdrTxSet.v1TxSet()
                        .phases[i]
                        .v0Components()
                        .back()
                        .txsMaybeDiscountedFee()
                        .baseFee.activate() = 1500;
                    xdrTxSet.v1TxSet()
                        .phases[i]
                        .v0Components()
                        .back()
                        .txsMaybeDiscountedFee()
                        .txs.emplace_back();

                    xdrTxSet.v1TxSet().phases[i].v0Components().emplace_back(
                        TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
                    xdrTxSet.v1TxSet()
                        .phases[i]
                        .v0Components()
                        .back()
                        .txsMaybeDiscountedFee()
                        .baseFee.activate() = 1500;
                    xdrTxSet.v1TxSet()
                        .phases[i]
                        .v0Components()
                        .back()
                        .txsMaybeDiscountedFee()
                        .txs.emplace_back();

                    xdrTxSet.v1TxSet().phases[i].v0Components().emplace_back(
                        TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
                    xdrTxSet.v1TxSet()
                        .phases[i]
                        .v0Components()
                        .back()
                        .txsMaybeDiscountedFee()
                        .baseFee.activate() = 1600;
                    xdrTxSet.v1TxSet()
                        .phases[i]
                        .v0Components()
                        .back()
                        .txsMaybeDiscountedFee()
                        .txs.emplace_back();

                    auto txSet = TxSetXDRFrame::makeFromWire(xdrTxSet);
                    REQUIRE(txSet->prepareForApply(*app) == nullptr);
                }
                SECTION("duplicate non-discounted components")
                {

                    xdrTxSet.v1TxSet().phases[i].v0Components().emplace_back(
                        TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
                    xdrTxSet.v1TxSet()
                        .phases[i]
                        .v0Components()
                        .back()
                        .txsMaybeDiscountedFee()
                        .txs.emplace_back();

                    xdrTxSet.v1TxSet().phases[i].v0Components().emplace_back(
                        TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
                    xdrTxSet.v1TxSet()
                        .phases[i]
                        .v0Components()
                        .back()
                        .txsMaybeDiscountedFee()
                        .txs.emplace_back();

                    xdrTxSet.v1TxSet().phases[i].v0Components().emplace_back(
                        TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
                    xdrTxSet.v1TxSet()
                        .phases[i]
                        .v0Components()
                        .back()
                        .txsMaybeDiscountedFee()
                        .baseFee.activate() = 1500;
                    xdrTxSet.v1TxSet()
                        .phases[i]
                        .v0Components()
                        .back()
                        .txsMaybeDiscountedFee()
                        .txs.emplace_back();

                    auto txSet = TxSetXDRFrame::makeFromWire(xdrTxSet);
                    REQUIRE(txSet->prepareForApply(*app) == nullptr);
                }
            }
        }
    }
    SECTION("empty component")
    {
        xdrTxSet.v1TxSet().phases.emplace_back();
        xdrTxSet.v1TxSet().phases.emplace_back();

        for (int i = 0; i < xdrTxSet.v1TxSet().phases.size(); ++i)
        {
            SECTION("phase " + std::to_string(i))
            {
                xdrTxSet.v1TxSet().phases[i].v0Components().emplace_back(
                    TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);

                auto txSet = TxSetXDRFrame::makeFromWire(xdrTxSet);
                REQUIRE(txSet->prepareForApply(*app) == nullptr);
            }
        }
    }
    SECTION("wrong tx type in phases")
    {
        xdrTxSet.v1TxSet().phases.emplace_back();
        xdrTxSet.v1TxSet().phases.emplace_back();
        SECTION("classic phase")
        {
            xdrTxSet.v1TxSet().phases[1].v0Components().emplace_back(
                TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
            xdrTxSet.v1TxSet()
                .phases[1]
                .v0Components()
                .back()
                .txsMaybeDiscountedFee()
                .txs.emplace_back();
        }
        SECTION("soroban phase")
        {
            xdrTxSet.v1TxSet().phases[0].v0Components().emplace_back(
                TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
            xdrTxSet.v1TxSet()
                .phases[0]
                .v0Components()
                .back()
                .txsMaybeDiscountedFee()
                .txs.emplace_back();

            auto& txEnv = xdrTxSet.v1TxSet()
                              .phases[0]
                              .v0Components()
                              .back()
                              .txsMaybeDiscountedFee()
                              .txs.back();
            txEnv.v0().tx.operations.emplace_back();
            txEnv.v0().tx.operations.back().body.type(INVOKE_HOST_FUNCTION);
        }
        auto txSet = TxSetXDRFrame::makeFromWire(xdrTxSet);
        REQUIRE(txSet->prepareForApply(*app) == nullptr);
    }
    SECTION("valid XDR")
    {

        for (int i = 0; i < xdrTxSet.v1TxSet().phases.size(); ++i)
        {
            auto maybeAddSorobanOp = [&](GeneralizedTransactionSet& txSet) {
                if (i == 1)
                {
                    auto& txEnv = xdrTxSet.v1TxSet()
                                      .phases[i]
                                      .v0Components()
                                      .back()
                                      .txsMaybeDiscountedFee()
                                      .txs.back();
                    txEnv.v0().tx.operations.emplace_back();
                    txEnv.v0().tx.operations.back().body.type(
                        INVOKE_HOST_FUNCTION); // tx->isSoroban() ==
                                               // true
                }
            };
            SECTION("phase " + std::to_string(i))
            {
                SECTION("no transactions")
                {
                    xdrTxSet.v1TxSet().phases.emplace_back();
                    xdrTxSet.v1TxSet().phases.emplace_back();
                    auto txSet = TxSetXDRFrame::makeFromWire(xdrTxSet);
                    REQUIRE(txSet->prepareForApply(*app) == nullptr);
                }
                SECTION("single component")
                {
                    xdrTxSet.v1TxSet().phases.emplace_back();
                    xdrTxSet.v1TxSet().phases.emplace_back();
                    xdrTxSet.v1TxSet().phases[i].v0Components().emplace_back(
                        TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
                    xdrTxSet.v1TxSet()
                        .phases[i]
                        .v0Components()
                        .back()
                        .txsMaybeDiscountedFee()
                        .txs.emplace_back();
                    maybeAddSorobanOp(xdrTxSet);
                    auto txSet = TxSetXDRFrame::makeFromWire(xdrTxSet);
                    REQUIRE(txSet->prepareForApply(*app) == nullptr);
                }
                SECTION("multiple components")
                {
                    xdrTxSet.v1TxSet().phases.emplace_back();
                    xdrTxSet.v1TxSet().phases.emplace_back();
                    xdrTxSet.v1TxSet().phases[i].v0Components().emplace_back(
                        TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
                    xdrTxSet.v1TxSet()
                        .phases[i]
                        .v0Components()
                        .back()
                        .txsMaybeDiscountedFee()
                        .txs.emplace_back();
                    maybeAddSorobanOp(xdrTxSet);

                    xdrTxSet.v1TxSet().phases[i].v0Components().emplace_back(
                        TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
                    xdrTxSet.v1TxSet()
                        .phases[i]
                        .v0Components()
                        .back()
                        .txsMaybeDiscountedFee()
                        .baseFee.activate() = 1400;
                    xdrTxSet.v1TxSet()
                        .phases[i]
                        .v0Components()
                        .back()
                        .txsMaybeDiscountedFee()
                        .txs.emplace_back();
                    maybeAddSorobanOp(xdrTxSet);

                    xdrTxSet.v1TxSet().phases[i].v0Components().emplace_back(
                        TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
                    xdrTxSet.v1TxSet()
                        .phases[i]
                        .v0Components()
                        .back()
                        .txsMaybeDiscountedFee()
                        .baseFee.activate() = 1500;
                    xdrTxSet.v1TxSet()
                        .phases[i]
                        .v0Components()
                        .back()
                        .txsMaybeDiscountedFee()
                        .txs.emplace_back();
                    maybeAddSorobanOp(xdrTxSet);

                    xdrTxSet.v1TxSet().phases[i].v0Components().emplace_back(
                        TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
                    xdrTxSet.v1TxSet()
                        .phases[i]
                        .v0Components()
                        .back()
                        .txsMaybeDiscountedFee()
                        .baseFee.activate() = 1600;
                    xdrTxSet.v1TxSet()
                        .phases[i]
                        .v0Components()
                        .back()
                        .txsMaybeDiscountedFee()
                        .txs.emplace_back();
                    maybeAddSorobanOp(xdrTxSet);

                    auto txSet = TxSetXDRFrame::makeFromWire(xdrTxSet);
                    REQUIRE(txSet->prepareForApply(*app) == nullptr);
                }
            }
        }
    }
}

TEST_CASE("generalized tx set XDR conversion", "[txset]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.LEDGER_PROTOCOL_VERSION =
        static_cast<uint32_t>(SOROBAN_PROTOCOL_VERSION);
    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
        static_cast<uint32_t>(SOROBAN_PROTOCOL_VERSION);
    Application::pointer app = createTestApplication(clock, cfg);
    overrideSorobanNetworkConfigForTest(*app);
    modifySorobanNetworkConfig(*app, [](SorobanNetworkConfig& sorobanCfg) {
        sorobanCfg.mLedgerMaxTxCount = 5;
    });
    auto root = TestAccount::createRoot(*app);
    int accountId = 0;
    auto createTxs = [&](int cnt, int fee, bool isSoroban = false) {
        std::vector<TransactionFrameBasePtr> txs;
        for (int i = 0; i < cnt; ++i)
        {
            auto source =
                root.create("unique " + std::to_string(accountId++),
                            app->getLedgerManager().getLastMinBalance(2));
            if (isSoroban)
            {
                SorobanResources resources;
                resources.instructions = 800'000;
                resources.readBytes = 1000;
                resources.writeBytes = 1000;
                txs.emplace_back(createUploadWasmTx(
                    *app, source, fee, DEFAULT_TEST_RESOURCE_FEE, resources));
            }
            else
            {
                txs.emplace_back(transactionFromOperations(
                    *app, source.getSecretKey(), source.nextSequenceNumber(),
                    {createAccount(getAccount(std::to_string(i)).getPublicKey(),
                                   1)},
                    fee));
            }
        }
        return txs;
    };

    auto checkXdrRoundtrip = [&](GeneralizedTransactionSet const& txSetXdr) {
        auto txSetFrame = TxSetXDRFrame::makeFromWire(txSetXdr);
        ApplicableTxSetFrameConstPtr applicableFrame;
        {
            LedgerTxn ltx(app->getLedgerTxnRoot(), false,
                          TransactionMode::READ_ONLY_WITHOUT_SQL_TXN);
            applicableFrame = txSetFrame->prepareForApply(*app);
        }
        REQUIRE(applicableFrame->checkValid(*app, 0, 0));
        GeneralizedTransactionSet newXdr;
        applicableFrame->toWireTxSetFrame()->toXDR(newXdr);
        REQUIRE(newXdr == txSetXdr);
    };

    SECTION("empty set")
    {
        auto [_, ApplicableTxSetFrame] =
            testtxset::makeNonValidatedGeneralizedTxSet(
                {{}, {}}, *app,
                app->getLedgerManager().getLastClosedLedgerHeader().hash);

        GeneralizedTransactionSet txSetXdr;
        ApplicableTxSetFrame->toWireTxSetFrame()->toXDR(txSetXdr);
        REQUIRE(txSetXdr.v1TxSet().phases[0].v0Components().empty());
        checkXdrRoundtrip(txSetXdr);
    }
    SECTION("one discounted component set")
    {
        auto [_, ApplicableTxSetFrame] =
            testtxset::makeNonValidatedGeneralizedTxSet(
                {{std::make_pair(1234LL, createTxs(5, 1234))}, {}}, *app,
                app->getLedgerManager().getLastClosedLedgerHeader().hash);

        GeneralizedTransactionSet txSetXdr;
        ApplicableTxSetFrame->toWireTxSetFrame()->toXDR(txSetXdr);
        REQUIRE(txSetXdr.v1TxSet().phases[0].v0Components().size() == 1);
        REQUIRE(*txSetXdr.v1TxSet()
                     .phases[0]
                     .v0Components()[0]
                     .txsMaybeDiscountedFee()
                     .baseFee == 1234);
        REQUIRE(txSetXdr.v1TxSet()
                    .phases[0]
                    .v0Components()[0]
                    .txsMaybeDiscountedFee()
                    .txs.size() == 5);
        checkXdrRoundtrip(txSetXdr);
    }
    SECTION("one non-discounted component set")
    {
        auto [_, ApplicableTxSetFrame] =
            testtxset::makeNonValidatedGeneralizedTxSet(
                {{std::make_pair(std::nullopt, createTxs(5, 4321))}, {}}, *app,
                app->getLedgerManager().getLastClosedLedgerHeader().hash);

        GeneralizedTransactionSet txSetXdr;
        ApplicableTxSetFrame->toWireTxSetFrame()->toXDR(txSetXdr);
        REQUIRE(txSetXdr.v1TxSet().phases[0].v0Components().size() == 1);
        REQUIRE(!txSetXdr.v1TxSet()
                     .phases[0]
                     .v0Components()[0]
                     .txsMaybeDiscountedFee()
                     .baseFee);
        REQUIRE(txSetXdr.v1TxSet()
                    .phases[0]
                    .v0Components()[0]
                    .txsMaybeDiscountedFee()
                    .txs.size() == 5);
        checkXdrRoundtrip(txSetXdr);
    }
    SECTION("multiple component sets")
    {
        auto [_, ApplicableTxSetFrame] =
            testtxset::makeNonValidatedGeneralizedTxSet(
                {{std::make_pair(12345LL, createTxs(3, 12345)),
                  std::make_pair(123LL, createTxs(1, 123)),
                  std::make_pair(1234LL, createTxs(2, 1234)),
                  std::make_pair(std::nullopt, createTxs(4, 4321))},
                 {}},
                *app, app->getLedgerManager().getLastClosedLedgerHeader().hash);

        GeneralizedTransactionSet txSetXdr;
        ApplicableTxSetFrame->toWireTxSetFrame()->toXDR(txSetXdr);
        auto const& comps = txSetXdr.v1TxSet().phases[0].v0Components();
        REQUIRE(comps.size() == 4);
        REQUIRE(!comps[0].txsMaybeDiscountedFee().baseFee);
        REQUIRE(comps[0].txsMaybeDiscountedFee().txs.size() == 4);
        REQUIRE(*comps[1].txsMaybeDiscountedFee().baseFee == 123);
        REQUIRE(comps[1].txsMaybeDiscountedFee().txs.size() == 1);
        REQUIRE(*comps[2].txsMaybeDiscountedFee().baseFee == 1234);
        REQUIRE(comps[2].txsMaybeDiscountedFee().txs.size() == 2);
        REQUIRE(*comps[3].txsMaybeDiscountedFee().baseFee == 12345);
        REQUIRE(comps[3].txsMaybeDiscountedFee().txs.size() == 3);
        checkXdrRoundtrip(txSetXdr);
    }
    SECTION("built from transactions")
    {
        auto const& lclHeader =
            app->getLedgerManager().getLastClosedLedgerHeader();
        std::vector<TransactionFrameBasePtr> txs =
            createTxs(5, lclHeader.header.baseFee, /* isSoroban */ false);
        std::vector<TransactionFrameBasePtr> sorobanTxs =
            createTxs(5, 10'000'000, /* isSoroban */ true);

        SECTION("classic only")
        {
            auto txSet = makeTxSetFromTransactions(txs, *app, 0, 0).first;
            GeneralizedTransactionSet txSetXdr;
            txSet->toXDR(txSetXdr);
            REQUIRE(txSetXdr.v1TxSet().phases.size() == 2);
            REQUIRE(txSetXdr.v1TxSet().phases[0].v0Components().size() == 1);
            REQUIRE(*txSetXdr.v1TxSet()
                         .phases[0]
                         .v0Components()[0]
                         .txsMaybeDiscountedFee()
                         .baseFee == lclHeader.header.baseFee);
            REQUIRE(txSetXdr.v1TxSet()
                        .phases[0]
                        .v0Components()[0]
                        .txsMaybeDiscountedFee()
                        .txs.size() == 5);
            checkXdrRoundtrip(txSetXdr);
        }
        SECTION("classic and soroban")
        {
            SECTION("valid")
            {
                SECTION("minimum base fee")
                {
                    auto txSet =
                        makeTxSetFromTransactions({txs, sorobanTxs}, *app, 0, 0)
                            .first;
                    GeneralizedTransactionSet txSetXdr;
                    txSet->toXDR(txSetXdr);
                    REQUIRE(txSetXdr.v1TxSet().phases.size() == 2);
                    for (auto const& phase : txSetXdr.v1TxSet().phases)
                    {
                        // Base inclusion fee is 100 for all phases since no
                        // surge pricing kicked in
                        REQUIRE(phase.v0Components().size() == 1);
                        REQUIRE(*phase.v0Components()[0]
                                     .txsMaybeDiscountedFee()
                                     .baseFee == lclHeader.header.baseFee);
                        REQUIRE(phase.v0Components()[0]
                                    .txsMaybeDiscountedFee()
                                    .txs.size() == 5);
                    }
                    checkXdrRoundtrip(txSetXdr);
                }
                SECTION("higher base fee")
                {
                    // generate more soroban txs with higher fee to trigger
                    // surge pricing
                    auto higherFeeSorobanTxs =
                        createTxs(5, 20'000'000, /* isSoroban */ true);
                    sorobanTxs.insert(sorobanTxs.begin(),
                                      higherFeeSorobanTxs.begin(),
                                      higherFeeSorobanTxs.end());
                    auto txSet = makeTxSetFromTransactions({txs, sorobanTxs},
                                                           *app, 0, 100)
                                     .first;
                    GeneralizedTransactionSet txSetXdr;
                    txSet->toXDR(txSetXdr);
                    REQUIRE(txSetXdr.v1TxSet().phases.size() == 2);
                    for (int i = 0; i < txSetXdr.v1TxSet().phases.size(); i++)
                    {
                        auto const& phase = txSetXdr.v1TxSet().phases[i];
                        auto expectedBaseFee =
                            i == 0 ? lclHeader.header.baseFee
                                   : higherFeeSorobanTxs[0]->getInclusionFee();
                        REQUIRE(phase.v0Components().size() == 1);
                        REQUIRE(*phase.v0Components()[0]
                                     .txsMaybeDiscountedFee()
                                     .baseFee == expectedBaseFee);
                        REQUIRE(phase.v0Components()[0]
                                    .txsMaybeDiscountedFee()
                                    .txs.size() == 5);
                    }
                    checkXdrRoundtrip(txSetXdr);
                }
            }
            SECTION("invalid, soroban tx in wrong phase")
            {
                sorobanTxs[4] = txs[0];
                REQUIRE_THROWS_WITH(
                    makeTxSetFromTransactions({txs, sorobanTxs}, *app, 0, 0),
                    "TxSetFrame::makeFromTransactions: phases "
                    "contain txs of wrong type");
            }
            SECTION("invalid, classic tx in wrong phase")
            {
                txs[4] = sorobanTxs[0];
                REQUIRE_THROWS_WITH(
                    makeTxSetFromTransactions({txs, sorobanTxs}, *app, 0, 0),
                    "TxSetFrame::makeFromTransactions: phases "
                    "contain txs of wrong type");
            }
        }
    }
}

TEST_CASE("generalized tx set with multiple txs per source account",
          "[txset][soroban]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.LEDGER_PROTOCOL_VERSION =
        static_cast<uint32_t>(SOROBAN_PROTOCOL_VERSION);
    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
        static_cast<uint32_t>(SOROBAN_PROTOCOL_VERSION);
    Application::pointer app = createTestApplication(clock, cfg);
    auto root = TestAccount::createRoot(*app);
    int accountId = 1;

    auto createTx = [&](int opCnt, int fee, bool unique) {
        std::vector<Operation> ops;
        for (int i = 0; i < opCnt; ++i)
        {
            ops.emplace_back(createAccount(
                getAccount(std::to_string(accountId++)).getPublicKey(), 1));
        }

        if (unique)
        {
            // Create a new unique accounts to ensure there are no collisions
            auto source =
                root.create("unique " + std::to_string(accountId),
                            app->getLedgerManager().getLastMinBalance(2));
            return transactionFromOperations(*app, source.getSecretKey(),
                                             source.nextSequenceNumber(), ops,
                                             fee);
        }
        else
        {
            return transactionFromOperations(
                *app, root.getSecretKey(), root.nextSequenceNumber(), ops, fee);
        }
    };

    SECTION("invalid")
    {
        auto txSet =
            testtxset::makeNonValidatedGeneralizedTxSet(
                {{std::make_pair(
                     500,
                     std::vector<TransactionFrameBasePtr>{
                         createTx(1, 1000, false), createTx(3, 1500, false)})},
                 {}},
                *app, app->getLedgerManager().getLastClosedLedgerHeader().hash)
                .second;

        REQUIRE(!txSet->checkValid(*app, 0, 0));
    }
    SECTION("valid")
    {
        auto txSet =
            testtxset::makeNonValidatedGeneralizedTxSet(
                {{std::make_pair(
                     500,
                     std::vector<TransactionFrameBasePtr>{
                         createTx(1, 1000, true), createTx(3, 1500, true)})},
                 {}},
                *app, app->getLedgerManager().getLastClosedLedgerHeader().hash)
                .second;

        REQUIRE(txSet->checkValid(*app, 0, 0));
    }
    SECTION("invalid, classic and soroban")
    {
        SorobanResources resources;
        resources.instructions = 800'000;
        resources.readBytes = 1000;
        resources.writeBytes = 1000;
        uint32_t inclusionFee = 500;
        int64_t resourceFee = sorobanResourceFee(*app, resources, 5000, 100);
        auto sorobanTx = createUploadWasmTx(*app, root, inclusionFee,
                                            resourceFee, resources);
        // Make sure fees got computed correctly
        REQUIRE(sorobanTx->getInclusionFee() == inclusionFee);

        auto txSet =
            testtxset::makeNonValidatedGeneralizedTxSet(
                {{std::make_pair(
                     500,
                     std::vector<TransactionFrameBasePtr>{
                         createTx(1, 1000, false), createTx(3, 1500, false)})},
                 {std::make_pair(
                     500, std::vector<TransactionFrameBasePtr>{sorobanTx})}},
                *app, app->getLedgerManager().getLastClosedLedgerHeader().hash)
                .second;

        REQUIRE(!txSet->checkValid(*app, 0, 0));
    }
}

TEST_CASE("generalized tx set fees", "[txset][soroban]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.LEDGER_PROTOCOL_VERSION =
        static_cast<uint32_t>(SOROBAN_PROTOCOL_VERSION);
    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
        static_cast<uint32_t>(SOROBAN_PROTOCOL_VERSION);

    Application::pointer app = createTestApplication(clock, cfg);
    overrideSorobanNetworkConfigForTest(*app);
    modifySorobanNetworkConfig(*app, [](SorobanNetworkConfig& sorobanCfg) {
        sorobanCfg.mLedgerMaxTxCount = 10;
    });
    auto root = TestAccount::createRoot(*app);
    int accountId = 1;

    auto createTx = [&](int opCnt, int inclusionFee, bool isSoroban = false,
                        bool validateTx = true) {
        auto source = root.create("unique " + std::to_string(accountId++),
                                  app->getLedgerManager().getLastMinBalance(2));
        if (isSoroban)
        {
            SorobanResources resources;
            resources.instructions = 800'000;
            resources.readBytes = 1000;
            resources.writeBytes = 1000;
            resources.footprint.readWrite.emplace_back();
            auto resourceFee = sorobanResourceFee(*app, resources, 5000, 40);
            resources.footprint.readWrite.pop_back();
            auto tx = createUploadWasmTx(*app, source, inclusionFee,
                                         resourceFee, resources);
            REQUIRE(tx->getInclusionFee() == inclusionFee);
            LedgerTxn ltx(app->getLedgerTxnRoot());
            if (validateTx)
            {
                REQUIRE(tx->checkValidForTesting(*app, ltx, 0, 0, 0));
            }
            return tx;
        }
        else
        {
            std::vector<Operation> ops;
            for (int i = 0; i < opCnt; ++i)
            {
                ops.emplace_back(createAccount(
                    getAccount(std::to_string(accountId++)).getPublicKey(), 1));
            }
            return transactionFromOperations(*app, source.getSecretKey(),
                                             source.nextSequenceNumber(), ops,
                                             inclusionFee);
        }
    };

    SECTION("valid txset")
    {
        auto txSet =
            testtxset::makeNonValidatedGeneralizedTxSet(
                {{std::make_pair(500,
                                 std::vector<TransactionFrameBasePtr>{
                                     createTx(1, 1000), createTx(3, 1500)}),
                  std::make_pair(1000,
                                 std::vector<TransactionFrameBasePtr>{
                                     createTx(4, 5000), createTx(1, 1000),
                                     createTx(5, 6000)}),
                  std::make_pair(std::nullopt,
                                 std::vector<TransactionFrameBasePtr>{
                                     createTx(2, 10000), createTx(5, 100000)})},
                 {std::make_pair(500,
                                 std::vector<TransactionFrameBasePtr>{
                                     createTx(1, 1000, /* isSoroban */ true),
                                     createTx(1, 500, /* isSoroban */ true)}),
                  std::make_pair(1000,
                                 std::vector<TransactionFrameBasePtr>{
                                     createTx(1, 1250, /* isSoroban */ true),
                                     createTx(1, 1000, /* isSoroban */ true),
                                     createTx(1, 1200, /* isSoroban */ true)}),
                  std::make_pair(
                      std::nullopt,
                      std::vector<TransactionFrameBasePtr>{
                          createTx(1, 5000, /* isSoroban */ true),
                          createTx(1, 20000, /* isSoroban */ true)})}},
                *app, app->getLedgerManager().getLastClosedLedgerHeader().hash)
                .second;

        REQUIRE(txSet->checkValid(*app, 0, 0));
        for (auto i = 0; i < static_cast<size_t>(TxSetPhase::PHASE_COUNT); ++i)
        {
            std::vector<std::optional<int64_t>> fees;
            for (auto const& tx :
                 txSet->getTxsForPhase(static_cast<TxSetPhase>(i)))
            {
                fees.push_back(
                    txSet->getTxBaseFee(tx, app->getLedgerManager()
                                                .getLastClosedLedgerHeader()
                                                .header));
            }
            std::sort(fees.begin(), fees.end());
            REQUIRE(fees == std::vector<std::optional<int64_t>>{
                                std::nullopt, std::nullopt, 500, 500, 1000,
                                1000, 1000});
        }
    }
    SECTION("tx with too low discounted fee")
    {
        SECTION("classic")
        {
            auto txSet =
                testtxset::makeNonValidatedGeneralizedTxSet(
                    {{std::make_pair(500,
                                     std::vector<TransactionFrameBasePtr>{
                                         createTx(2, 999)})},
                     {}},
                    *app,
                    app->getLedgerManager().getLastClosedLedgerHeader().hash)
                    .second;

            REQUIRE(!txSet->checkValid(*app, 0, 0));
        }
        SECTION("soroban")
        {
            auto txSet =
                testtxset::makeNonValidatedGeneralizedTxSet(
                    {{},
                     {std::make_pair(
                         500, std::vector<TransactionFrameBasePtr>{createTx(
                                  1, 499, /* isSoroban */ true)})}},
                    *app,
                    app->getLedgerManager().getLastClosedLedgerHeader().hash)
                    .second;

            REQUIRE(!txSet->checkValid(*app, 0, 0));
        }
    }

    SECTION("tx with too low non-discounted fee")
    {
        SECTION("classic")
        {
            auto txSet =
                testtxset::makeNonValidatedGeneralizedTxSet(
                    {{std::make_pair(std::nullopt,
                                     std::vector<TransactionFrameBasePtr>{
                                         createTx(2, 199)})},
                     {}},
                    *app,
                    app->getLedgerManager().getLastClosedLedgerHeader().hash)
                    .second;

            REQUIRE(!txSet->checkValid(*app, 0, 0));
        }
        SECTION("soroban")
        {
            auto txSet =
                testtxset::makeNonValidatedGeneralizedTxSet(
                    {{},
                     {std::make_pair(std::nullopt,
                                     std::vector<TransactionFrameBasePtr>{
                                         createTx(1, 99, true, false)})}},
                    *app,
                    app->getLedgerManager().getLastClosedLedgerHeader().hash)
                    .second;

            REQUIRE(!txSet->checkValid(*app, 0, 0));
        }
    }
}

} // namespace
} // namespace stellar
