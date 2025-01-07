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
#include "transactions/MutableTransactionResult.h"
#include "util/ProtocolVersion.h"
#include "util/XDRCereal.h"

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

void
testGeneralizedTxSetXDRConversion(ProtocolVersion protocolVersion)
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.LEDGER_PROTOCOL_VERSION = static_cast<uint32_t>(protocolVersion);
    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
        static_cast<uint32_t>(protocolVersion);
    bool isParallelSoroban = protocolVersionStartsFrom(
        cfg.LEDGER_PROTOCOL_VERSION, PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION);

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
            LedgerTxn ltx(app->getLedgerTxnRoot());
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
                    for (auto i = 0; i < txSetXdr.v1TxSet().phases.size(); ++i)
                    {
                        auto const& phase = txSetXdr.v1TxSet().phases[i];

                        // Base inclusion fee is 100 for all phases since no
                        // surge pricing kicked in
                        if (i == static_cast<size_t>(TxSetPhase::SOROBAN) &&
                            isParallelSoroban)
                        {
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
                            REQUIRE(phase.v() == 1);
                            REQUIRE(*phase.parallelTxsComponent().baseFee ==
                                    lclHeader.header.baseFee);
                            REQUIRE(phase.parallelTxsComponent()
                                        .executionStages.size() == 1);
                            REQUIRE(phase.parallelTxsComponent()
                                        .executionStages[0]
                                        .size() == 1);
                            REQUIRE(phase.parallelTxsComponent()
                                        .executionStages[0][0]
                                        .size() == 5);
#else
                            releaseAssert(false);
#endif
                        }
                        else
                        {
                            REQUIRE(phase.v() == 0);
                            REQUIRE(phase.v0Components().size() == 1);
                            REQUIRE(*phase.v0Components()[0]
                                         .txsMaybeDiscountedFee()
                                         .baseFee == lclHeader.header.baseFee);
                            REQUIRE(phase.v0Components()[0]
                                        .txsMaybeDiscountedFee()
                                        .txs.size() == 5);
                        }
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
                    for (auto i = 0; i < txSetXdr.v1TxSet().phases.size(); ++i)
                    {
                        auto const& phase = txSetXdr.v1TxSet().phases[i];
                        auto expectedBaseFee =
                            i == 0 ? lclHeader.header.baseFee
                                   : higherFeeSorobanTxs[0]->getInclusionFee();
                        if (i == static_cast<size_t>(TxSetPhase::SOROBAN) &&
                            isParallelSoroban)
                        {
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
                            REQUIRE(phase.v() == 1);
                            REQUIRE(*phase.parallelTxsComponent().baseFee ==
                                    expectedBaseFee);
                            REQUIRE(phase.parallelTxsComponent()
                                        .executionStages.size() == 1);
                            REQUIRE(phase.parallelTxsComponent()
                                        .executionStages[0]
                                        .size() == 1);
                            REQUIRE(phase.parallelTxsComponent()
                                        .executionStages[0][0]
                                        .size() == 5);
#else
                            releaseAssert(false);
#endif
                        }
                        else
                        {
                            REQUIRE(phase.v() == 0);
                            REQUIRE(phase.v0Components().size() == 1);
                            REQUIRE(*phase.v0Components()[0]
                                         .txsMaybeDiscountedFee()
                                         .baseFee == expectedBaseFee);
                            REQUIRE(phase.v0Components()[0]
                                        .txsMaybeDiscountedFee()
                                        .txs.size() == 5);
                        }
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

TEST_CASE("generalized tx set XDR conversion",
          "[txset]"){SECTION("soroban protocol version"){
    testGeneralizedTxSetXDRConversion(SOROBAN_PROTOCOL_VERSION);
}
SECTION("current protocol version")
{
    testGeneralizedTxSetXDRConversion(
        static_cast<ProtocolVersion>(Config::CURRENT_LEDGER_PROTOCOL_VERSION));
}
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
SECTION("parallel soroban protocol version")
{
    testGeneralizedTxSetXDRConversion(PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION);
}
#endif
}

TEST_CASE("soroban phase version validation", "[txset][soroban]")
{
    auto runTest = [](uint32_t protocolVersion,
                      bool useParallelSorobanPhase) -> bool {
        VirtualClock clock;
        auto cfg = getTestConfig();
        cfg.LEDGER_PROTOCOL_VERSION = protocolVersion;
        cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION = protocolVersion;
        auto app = createTestApplication(clock, cfg);
        auto txSet =
            testtxset::makeNonValidatedGeneralizedTxSet(
                {{}, {}}, *app,
                app->getLedgerManager().getLastClosedLedgerHeader().hash,
                useParallelSorobanPhase)
                .second;
        REQUIRE(txSet);
        return txSet->checkValid(*app, 0, 0);
    };
    SECTION("sequential phase")
    {
        SECTION("valid before parallel tx set protocol version")
        {
            REQUIRE(runTest(
                static_cast<uint32_t>(PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION) -
                    1,
                false));
        }
    }
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    SECTION("sequential phase invalid at parallel tx set protocol version")
    {
        REQUIRE(!runTest(
            static_cast<uint32_t>(PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION),
            false));
    }
    SECTION("parallel phase")
    {
        SECTION("valid before parallel tx set protocol version")
        {
            REQUIRE(!runTest(
                static_cast<uint32_t>(PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION) -
                    1,
                true));
        }
        SECTION("invalid at parallel tx set protocol version")
        {
            REQUIRE(runTest(
                static_cast<uint32_t>(PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION),
                true));
        }
    }
#endif
}

TEST_CASE("generalized tx set with multiple txs per source account",
          "[txset][soroban]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.LEDGER_PROTOCOL_VERSION = Config::CURRENT_LEDGER_PROTOCOL_VERSION;
    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
        Config::CURRENT_LEDGER_PROTOCOL_VERSION;
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
    cfg.LEDGER_PROTOCOL_VERSION = Config::CURRENT_LEDGER_PROTOCOL_VERSION;
    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
        Config::CURRENT_LEDGER_PROTOCOL_VERSION;

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
                REQUIRE(tx->checkValidForTesting(app->getAppConnector(), ltx, 0,
                                                 0, 0));
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
        testtxset::ComponentPhases sorobanTxs;
        bool isParallelSoroban =
            protocolVersionStartsFrom(Config::CURRENT_LEDGER_PROTOCOL_VERSION,
                                      PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION);
        if (isParallelSoroban)
        {
            sorobanTxs = {std::make_pair(
                1000, std::vector<TransactionFrameBasePtr>{
                          createTx(1, 1250, /* isSoroban */ true),
                          createTx(1, 1000, /* isSoroban */ true),
                          createTx(1, 1200, /* isSoroban */ true)})};
        }
        else
        {
            sorobanTxs = {
                std::make_pair(500,
                               std::vector<TransactionFrameBasePtr>{
                                   createTx(1, 1000, /* isSoroban */ true),
                                   createTx(1, 500, /* isSoroban */ true)}),
                std::make_pair(1000,
                               std::vector<TransactionFrameBasePtr>{
                                   createTx(1, 1250, /* isSoroban */ true),
                                   createTx(1, 1000, /* isSoroban */ true),
                                   createTx(1, 1200, /* isSoroban */ true)}),
                std::make_pair(std::nullopt,
                               std::vector<TransactionFrameBasePtr>{
                                   createTx(1, 5000, /* isSoroban */ true),
                                   createTx(1, 20000, /* isSoroban */ true)})};
        }
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
                 sorobanTxs},
                *app, app->getLedgerManager().getLastClosedLedgerHeader().hash)
                .second;

        REQUIRE(txSet->checkValid(*app, 0, 0));
        for (auto i = 0; i < static_cast<size_t>(TxSetPhase::PHASE_COUNT); ++i)
        {
            std::vector<std::optional<int64_t>> fees;
            for (auto const& tx : txSet->getPhase(static_cast<TxSetPhase>(i)))
            {
                fees.push_back(txSet->getTxBaseFee(tx));
            }
            std::sort(fees.begin(), fees.end());
            if (isParallelSoroban &&
                i == static_cast<size_t>(TxSetPhase::SOROBAN))
            {
                REQUIRE(fees ==
                        std::vector<std::optional<int64_t>>{1000, 1000, 1000});
            }
            else
            {
                REQUIRE(fees == std::vector<std::optional<int64_t>>{
                                    std::nullopt, std::nullopt, 500, 500, 1000,
                                    1000, 1000});
            }
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

TEST_CASE("txset nomination", "[txset]")
{
    // This test covers the transaction set nomination logic using randomly
    // generated transactions and ledger limits.
    // While transaction set building logic is not a part of the protocol, it's
    // still useful to check that we don't unexpectedly break it, especially
    // during supposedly no-op changes.
    // This doesn't cover the small edge cases, which should be covered by
    // dedicated test scenarios instead.
    auto runTest = [](uint32_t protocolVersion, std::string testName) {
        std::mt19937 rng(54321);
        std::vector<std::string> testResults;
        testResults.push_back(
            "xdr_hash,total_fees,total_inclusion_fees,classic_"
            "ops,classic_non_dex_txs,classic_non_dex_txs_base_fee,classic_dex_"
            "txs,classic_dex_txs_base_fee,soroban_ops,soroban_base_fee,"
            "insns,read_bytes,write_bytes,read_entries,write_"
            "entries,tx_size_bytes");
        Config cfg(getTestConfig());
        cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION = protocolVersion;

        cfg.NODE_SEED = SecretKey::pseudoRandomForTestingFromSeed(54321);
        VirtualClock clock;
        Application::pointer app = createTestApplication(clock, cfg);
        auto root = TestAccount::createRoot(*app);
        std::vector<std::pair<TestAccount, int64_t>> accounts;
        for (int i = 0; i < 1000; ++i)
        {
            auto account = root.create(std::to_string(i), 1'000'000'000);
            accounts.emplace_back(account, account.getLastSequenceNumber() + 1);
        }

        stellar::uniform_int_distribution<> txCountDistr(100, 500);

        stellar::uniform_int_distribution<> ledgerEntriesDistr(100, 500);
        stellar::uniform_int_distribution<> ledgerBytesDistr(50'000, 300'000);
        stellar::uniform_int_distribution<> ledgerInsnsDistr(100'000'000,
                                                             1'000'000'000);
        stellar::uniform_int_distribution<> ledgerTxSizeBytesDistr(300'000,
                                                                   1'000'000);

        stellar::uniform_int_distribution<> txToLedgerRatioPercentDistr(
            2 * 100, 10 * 100);
        stellar::uniform_int_distribution<> maxInclusionFeeDistr(100, 2000);

        stellar::uniform_int_distribution<> maxOpsPerTxDistr(1, 100);
        stellar::uniform_int_distribution<> feeBumpFractionDistr(0, 3);
        stellar::uniform_int_distribution<> isFeeBumpDistr(0, 2);
        Asset asset1(ASSET_TYPE_CREDIT_ALPHANUM4);
        strToAssetCode(asset1.alphaNum4().assetCode, "USD");
        Asset asset2(ASSET_TYPE_NATIVE);

        auto runIteration = [&]() {
            int classicOpsCount = txCountDistr(rng);
            int feeBumpFraction = feeBumpFractionDistr(rng);

            LedgerUpgrade upgrade(LEDGER_UPGRADE_MAX_TX_SET_SIZE);
            upgrade.newMaxTxSetSize() = classicOpsCount;
            xdr::xvector<UpgradeType, 6> upgrades;
            LedgerUpgrade ledgerUpgrade(LEDGER_UPGRADE_MAX_TX_SET_SIZE);
            ledgerUpgrade.newMaxTxSetSize() = txCountDistr(rng);

            app->getMutableConfig().MAX_DEX_TX_OPERATIONS_IN_TX_SET =
                stellar::uniform_int_distribution<>(0, classicOpsCount)(rng);

            auto v = xdr::xdr_to_opaque(ledgerUpgrade);
            upgrades.push_back(UpgradeType(v.begin(), v.end()));
            closeLedger(*app, {}, false, upgrades);

            modifySorobanNetworkConfig(*app, [&](SorobanNetworkConfig& cfg) {
                cfg.mLedgerMaxTxCount = txCountDistr(rng);

                cfg.mLedgerMaxWriteLedgerEntries = ledgerEntriesDistr(rng);
                cfg.mTxMaxWriteLedgerEntries =
                    static_cast<int64_t>(cfg.mLedgerMaxWriteLedgerEntries) *
                    100 / txToLedgerRatioPercentDistr(rng);

                cfg.mLedgerMaxReadLedgerEntries =
                    cfg.mLedgerMaxWriteLedgerEntries + ledgerEntriesDistr(rng);
                cfg.mTxMaxReadLedgerEntries =
                    static_cast<int64_t>(cfg.mLedgerMaxReadLedgerEntries) *
                    100 / txToLedgerRatioPercentDistr(rng);

                cfg.mLedgerMaxReadBytes = ledgerBytesDistr(rng);
                cfg.mTxMaxReadBytes =
                    static_cast<int64_t>(cfg.mLedgerMaxReadBytes) * 100 /
                    txToLedgerRatioPercentDistr(rng);

                cfg.mLedgerMaxWriteBytes = ledgerBytesDistr(rng);
                cfg.mTxMaxWriteBytes =
                    static_cast<int64_t>(cfg.mLedgerMaxWriteBytes) * 100 /
                    txToLedgerRatioPercentDistr(rng);

                cfg.mLedgerMaxTransactionsSizeBytes =
                    ledgerTxSizeBytesDistr(rng);
                cfg.mTxMaxSizeBytes =
                    static_cast<int64_t>(cfg.mLedgerMaxTransactionsSizeBytes) *
                    100 / txToLedgerRatioPercentDistr(rng);
                cfg.mMaxContractSizeBytes = cfg.mTxMaxSizeBytes;

                cfg.mLedgerMaxInstructions = ledgerInsnsDistr(rng);
                cfg.mTxMaxInstructions =
                    static_cast<int64_t>(cfg.mLedgerMaxInstructions) * 100 /
                    txToLedgerRatioPercentDistr(rng);
            });

            auto const& sorobanConfig =
                app->getLedgerManager().getSorobanNetworkConfigReadOnly();
            stellar::uniform_int_distribution<> txReadEntriesDistr(
                1, sorobanConfig.txMaxReadLedgerEntries());

            stellar::uniform_int_distribution<> txReadBytesDistr(
                50, sorobanConfig.txMaxReadBytes());
            stellar::uniform_int_distribution<> txWriteBytesDistr(
                50, sorobanConfig.txMaxWriteBytes());

            stellar::uniform_int_distribution<> txInsnsDistr(
                100'000, sorobanConfig.txMaxInstructions());
            stellar::uniform_int_distribution<> txSizeDistr(
                1, sorobanConfig.txMaxSizeBytes());

            int maxOpsPerTx = maxOpsPerTxDistr(rng);
            stellar::uniform_int_distribution<> opsPerTxDistr(1, maxOpsPerTx);
            stellar::uniform_int_distribution<> inclusionFeeDistr(
                100, maxInclusionFeeDistr(rng));

            uint32_t dexOpsCount = stellar::uniform_int_distribution<>(
                0, classicOpsCount * 2)(rng);
            std::vector<TransactionFrameBaseConstPtr> classicTxs;
            size_t accountId = 0;
            for (uint32_t i = 0; i < classicOpsCount * 2;)
            {
                auto numOps = opsPerTxDistr(rng);
                std::vector<Operation> ops;
                if (i < dexOpsCount)
                {

                    for (uint32_t j = 1; j <= numOps; ++j)
                    {
                        ops.emplace_back(manageBuyOffer(
                            i * 1000 + j, asset1, asset2, Price{2, 5}, 10));
                    }
                }
                else
                {
                    for (uint32_t j = 1; j <= numOps; ++j)
                    {
                        ops.emplace_back(payment(root, j));
                    }
                }
                auto& [account, seqNum] = accounts[accountId++];
                auto tx = transactionFromOperations(
                    *app, account.getSecretKey(), seqNum, ops,
                    inclusionFeeDistr(rng) * numOps);
                if (isFeeBumpDistr(rng) < feeBumpFraction)
                {
                    classicTxs.push_back(
                        feeBump(*app, account, tx, tx->getFullFee() * 2));
                }
                else
                {
                    classicTxs.push_back(tx);
                }
                i += numOps;
            }

            LedgerKey key(LedgerEntryType::CONTRACT_DATA);
            key.contractData().key.type(SCValType::SCV_U32);
            std::vector<TransactionFrameBaseConstPtr> sorobanTxs;
            for (uint32_t i = 0; i < sorobanConfig.ledgerMaxTxCount(); ++i)
            {
                SorobanResources resources;
                resources.instructions = txInsnsDistr(rng);
                resources.readBytes = txReadBytesDistr(rng);
                resources.writeBytes = txWriteBytesDistr(rng);

                auto readEntries = txReadEntriesDistr(rng);
                stellar::uniform_int_distribution<> txWriteEntriesDistr(
                    1, std::min(readEntries,
                                static_cast<int>(
                                    sorobanConfig.txMaxWriteLedgerEntries())));
                auto writeEntries = txWriteEntriesDistr(rng);
                readEntries -= writeEntries;

                for (uint32_t j = 0; j < readEntries; ++j)
                {
                    key.contractData().key.u32() = j;
                    resources.footprint.readOnly.push_back(key);
                }

                for (uint32_t j = 0; j < writeEntries; ++j)
                {
                    key.contractData().key.u32() = j + 10000;
                    resources.footprint.readWrite.push_back(key);
                }
                int paddingBytes = std::max(
                    0, txSizeDistr(rng) -
                           static_cast<int>(xdr::xdr_size(resources)) - 200);
                Operation uploadOp;
                uploadOp.body.type(INVOKE_HOST_FUNCTION);
                auto& uploadHF =
                    uploadOp.body.invokeHostFunctionOp().hostFunction;
                uploadHF.type(HOST_FUNCTION_TYPE_UPLOAD_CONTRACT_WASM);
                uploadHF.wasm().resize(paddingBytes);
                auto& [account, seqNum] = accounts[accountId++];
                auto tx = sorobanTransactionFrameFromOps(
                    app->getNetworkID(), account, {uploadOp}, {}, resources,
                    inclusionFeeDistr(rng), 100'000'000, std::nullopt, seqNum);
                if (isFeeBumpDistr(rng) < feeBumpFraction)
                {
                    sorobanTxs.push_back(
                        feeBump(*app, account, tx, tx->getInclusionFee() * 2));
                }
                else
                {
                    sorobanTxs.push_back(tx);
                }
            }
            PerPhaseTransactionList txPhases = {classicTxs, sorobanTxs};
            PerPhaseTransactionList invalidTxs;
            invalidTxs.resize(txPhases.size());
            auto [xdrTxSetFrame, applicableTxSet] =
                makeTxSetFromTransactions(txPhases, *app, 0, 0, invalidTxs);
            REQUIRE(xdrTxSetFrame);
            REQUIRE(applicableTxSet);
            REQUIRE(invalidTxs[0].empty());
            REQUIRE(invalidTxs[1].empty());

            GeneralizedTransactionSet xdrTxSet;
            xdrTxSetFrame->toXDR(xdrTxSet);
            // Clear previous ledger hash in order to avoid tx set changes in
            // case of protocol changes that don't affect tx sets in any
            // meaningful way. Hash also has nothing to do with the tx set
            // building logic.
            xdrTxSet.v1TxSet().previousLedgerHash = Hash{};
            std::ostringstream oss;
            int64_t totalInsns = 0;
            int64_t totalReadBytes = 0;
            int64_t totalWriteBytes = 0;
            int64_t totalReadEntries = 0;
            int64_t totalWriteEntries = 0;
            int64_t totalTxSizeBytes = 0;
            for (auto const& tx :
                 applicableTxSet->getPhase(TxSetPhase::SOROBAN))
            {
                auto const& resources = tx->sorobanResources();
                totalInsns += resources.instructions;
                totalReadBytes += resources.readBytes;
                totalWriteBytes += resources.writeBytes;
                totalReadEntries += resources.footprint.readOnly.size() +
                                    resources.footprint.readWrite.size();
                totalWriteEntries += resources.footprint.readWrite.size();
                totalTxSizeBytes += xdr::xdr_size(tx->getEnvelope());
            }
            auto const& classicComponents =
                xdrTxSet.v1TxSet().phases[0].v0Components();
            REQUIRE(!classicComponents.empty());
            REQUIRE(classicComponents.size() <= 2);
            int64_t nonDexBaseFee =
                classicComponents[0].txsMaybeDiscountedFee().baseFee
                    ? *classicComponents[0].txsMaybeDiscountedFee().baseFee
                    : 100;
            size_t classicDexTxCount = 0;
            int64_t dexTxBaseFee = 0;
            if (classicComponents.size() > 1)
            {
                classicDexTxCount =
                    classicComponents[1].txsMaybeDiscountedFee().txs.size();
                dexTxBaseFee =
                    classicComponents[1].txsMaybeDiscountedFee().baseFee
                        ? *classicComponents[1].txsMaybeDiscountedFee().baseFee
                        : 100;
            }

            int64_t sorobanBaseFee = 100;
            if (protocolVersionIsBefore(
                    protocolVersion, PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION))
            {
                auto const& sorobanComponents =
                    xdrTxSet.v1TxSet().phases[1].v0Components();
                REQUIRE(sorobanComponents.size() == 1);
                REQUIRE(sorobanComponents[0].type() == 0);
                if (sorobanComponents[0].txsMaybeDiscountedFee().baseFee)
                {
                    sorobanBaseFee =
                        *sorobanComponents[0].txsMaybeDiscountedFee().baseFee;
                }
            }
            else
            {
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
                auto const& sorobanComponent =
                    xdrTxSet.v1TxSet().phases[1].parallelTxsComponent();
                if (sorobanComponent.baseFee)
                {
                    sorobanBaseFee = *sorobanComponent.baseFee;
                }
#endif
            }

            oss << binToHex(xdrSha256(xdrTxSet)) << ","
                << applicableTxSet->getTotalFees(
                       app->getLedgerManager()
                           .getLastClosedLedgerHeader()
                           .header)
                << "," << applicableTxSet->getTotalInclusionFees() << ","
                << applicableTxSet->sizeOp(TxSetPhase::CLASSIC) << ","
                << classicComponents[0].txsMaybeDiscountedFee().txs.size()
                << "," << nonDexBaseFee << "," << classicDexTxCount << ","
                << dexTxBaseFee << ","
                << applicableTxSet->sizeOp(TxSetPhase::SOROBAN) << ","
                << sorobanBaseFee << "," << totalInsns << "," << totalReadBytes
                << "," << totalWriteBytes << "," << totalReadEntries << ","
                << totalWriteEntries << "," << totalTxSizeBytes;
            testResults.push_back(oss.str());
        };

        for (int i = 1; i < 50; ++i)
        {
            runIteration();
        }
        namespace fs = std::filesystem;
        fs::path resultsPath = "testdata/txset/" + testName;
        fs::create_directories(resultsPath.parent_path());
        if (getenv("GENERATE_TEST_TXSETS"))
        {
            std::ofstream resultsStream(resultsPath);
            for (auto const& result : testResults)
            {
                resultsStream << result << std::endl;
            }
        }
        else
        {
            // If the tx sets have changed, this will fail. If the changes are
            // intentional, update the test data by setting
            // `GENERATE_TEST_TXSETS=1` env variable.
            std::ifstream resultsStream(resultsPath);
            std::string line;
            for (size_t i = 0; i < testResults.size(); ++i)
            {
                std::getline(resultsStream, line);
                REQUIRE(line == testResults[i]);
            }
        }
    };
    // Any Core build should normally only nominate transaction sets for at
    // most 2 protocol versions: the 'latest' one and the one before it (in
    // case of builds on protocol version boundaries). However, since we might
    // be in the 'vnext' build, we actually need to cover 3 versions:
    // 'previous' (the one before the latest released protocol), the 'current'
    // one (the latest released protocol) and the 'next' one (unstable next
    // protocol version). The 'current' version has to be covered in both
    // 'vcurr' and 'vnext' builds.
#ifndef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    SECTION("prev protocol")
    {
        runTest(Config::CURRENT_LEDGER_PROTOCOL_VERSION - 1, "v_prev.csv");
    }
#endif
    SECTION("curr protocol")
    {
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
        runTest(Config::CURRENT_LEDGER_PROTOCOL_VERSION - 1, "v_curr.csv");
#else
        runTest(Config::CURRENT_LEDGER_PROTOCOL_VERSION, "v_curr.csv");
#endif
    }
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    SECTION("next protocol")
    {
        runTest(Config::CURRENT_LEDGER_PROTOCOL_VERSION, "v_next.csv");
    }
#endif
}

} // namespace
} // namespace stellar
