// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SHA.h"
#include "herder/ParallelTxSetBuilder.h"
#include "herder/TxSetFrame.h"
#include "herder/test/TestTxSetUtils.h"
#include "ledger/LedgerManager.h"
#include "ledger/test/LedgerTestUtils.h"
#include "main/Application.h"
#include "main/Config.h"
#include "test/Catch2.h"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/MutableTransactionResult.h"
#include "transactions/TransactionUtils.h"
#include "transactions/test/SorobanTxTestUtils.h"
#include "util/Math.h"
#include "util/ProtocolVersion.h"
#include "util/XDRCereal.h"
#include <algorithm>
#include <map>
namespace stellar
{
namespace
{
using namespace txtest;

TEST_CASE("generalized tx set XDR validation", "[txset]")
{
    Config cfg(getTestConfig());
    cfg.LEDGER_PROTOCOL_VERSION = Config::CURRENT_LEDGER_PROTOCOL_VERSION;
    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
        Config::CURRENT_LEDGER_PROTOCOL_VERSION;
    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);

    GeneralizedTransactionSet xdrTxSet(1);
    xdrTxSet.v1TxSet().previousLedgerHash =
        app->getLedgerManager().getLastClosedLedgerHeader().hash;
    LedgerTxn ltx(app->getLedgerTxnRoot());
    SECTION("no phases")
    {
        auto txSet = TxSetXDRFrame::makeFromWire(xdrTxSet);
        REQUIRE(
            txSet->prepareForApply(
                *app,
                app->getLedgerManager().getLastClosedLedgerHeader().header) ==
            nullptr);
    }
    SECTION("one phase")
    {
        auto txSet = TxSetXDRFrame::makeFromWire(xdrTxSet);
        xdrTxSet.v1TxSet().phases.emplace_back();
        REQUIRE(
            txSet->prepareForApply(
                *app,
                app->getLedgerManager().getLastClosedLedgerHeader().header) ==
            nullptr);
    }
    SECTION("too many phases")
    {
        xdrTxSet.v1TxSet().phases.emplace_back();
        xdrTxSet.v1TxSet().phases.emplace_back();
        xdrTxSet.v1TxSet().phases.emplace_back();
        auto txSet = TxSetXDRFrame::makeFromWire(xdrTxSet);
        REQUIRE(
            txSet->prepareForApply(
                *app,
                app->getLedgerManager().getLastClosedLedgerHeader().header) ==
            nullptr);
    }

    SECTION("two phase scenarios")
    {
        xdrTxSet.v1TxSet().phases.emplace_back();
        xdrTxSet.v1TxSet().phases.emplace_back();
        int txId = 0;
        auto buildTx = [&](TransactionEnvelope& txEnv, bool isSoroban) {
            txEnv.v1().tx.operations.emplace_back();
            // The fee is actually not relevant for XDR validation, we just use
            // it to have different tx envelopes.
            txEnv.v1().tx.fee = 100 + txId;
            ++txId;
            txEnv.v1().tx.operations.back().body.type(
                isSoroban ? OperationType::INVOKE_HOST_FUNCTION
                          : OperationType::PAYMENT);
            if (isSoroban)
            {
                txEnv.v1().tx.ext.v(1);
                txEnv.v1().tx.ext.sorobanData().resourceFee = 1000;
            }
        };
        auto compareTxHash = [](TransactionEnvelope const& tx1,
                                TransactionEnvelope const& tx2) -> bool {
            return xdrSha256(tx1) < xdrSha256(tx2);
        };
        auto v0Phase =
            [&](std::vector<std::optional<int64_t>> componentBaseFees,
                bool isSoroban) -> TransactionPhase {
            TransactionPhase phase(0);
            for (auto const& baseFee : componentBaseFees)
            {
                auto& component = phase.v0Components().emplace_back(
                    TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
                if (baseFee)
                {
                    component.txsMaybeDiscountedFee().baseFee.activate() =
                        *baseFee;
                }
                for (int i = 0; i < 10; ++i)
                {
                    auto& txEnv =
                        component.txsMaybeDiscountedFee().txs.emplace_back(
                            ENVELOPE_TYPE_TX);
                    buildTx(txEnv, isSoroban);
                }
                std::sort(component.txsMaybeDiscountedFee().txs.begin(),
                          component.txsMaybeDiscountedFee().txs.end(),
                          compareTxHash);
            }
            return phase;
        };
        // Collection of per-phase scenarios: each scenario consists of a phase
        // XDR, a flag indicating whether the XDR is valid or not, and a string
        // name of the scenario.
        std::vector<
            std::vector<std::tuple<TransactionPhase, bool, std::string>>>
            scenarios(static_cast<size_t>(TxSetPhase::PHASE_COUNT));
        // Most of the scenarios are the same for Soroban and Classic phases, so
        // generate the same scenarios for both.
        for (int phaseId = 0;
             phaseId < static_cast<int>(TxSetPhase::PHASE_COUNT); ++phaseId)
        {
            bool isSoroban = phaseId == static_cast<int>(TxSetPhase::SOROBAN);

            // Valid scenarios
            scenarios[phaseId].emplace_back(v0Phase({}, isSoroban), true,
                                            "no txs");
            scenarios[phaseId].emplace_back(v0Phase({std::nullopt}, isSoroban),
                                            true,
                                            "single no discount component");
            scenarios[phaseId].emplace_back(v0Phase({1000}, isSoroban), true,
                                            "single discount component");
            scenarios[phaseId].emplace_back(
                v0Phase({1000, 1001, 1002}, isSoroban), true,
                "multiple discount components");
            auto validMultiComponentPhase =
                v0Phase({std::nullopt, 1000, 2000, 3000}, isSoroban);
            scenarios[phaseId].emplace_back(
                validMultiComponentPhase, true,
                "multiple discount components and no discount component");

            {
                auto phase = validMultiComponentPhase;
                phase.v0Components().emplace_back(
                    TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
                phase.v0Components().back() = phase.v0Components()[1];
                phase.v0Components()
                    .back()
                    .txsMaybeDiscountedFee()
                    .baseFee.activate() = 10000;
                // Note, that during XDR validation we don't try to check that
                // transactions are unique across components or phases.
                scenarios[phaseId].emplace_back(phase, true,
                                                "duplicate txs across phases");
            }

            // Invalid scenarios
            scenarios[phaseId].emplace_back(
                v0Phase({1000, 3000, 2000}, isSoroban), false,
                "incorrect discounted component order");
            scenarios[phaseId].emplace_back(
                v0Phase({1000, std::nullopt, 2000}, isSoroban), false,
                "incorrect non-discounted component order");
            scenarios[phaseId].emplace_back(
                v0Phase({std::nullopt, std::nullopt, 1000}, isSoroban), false,
                "duplicate non-discounted component");
            scenarios[phaseId].emplace_back(
                v0Phase({std::nullopt, 1000, 1000, 2000}, isSoroban), false,
                "duplicate discounted component");
            {
                auto phase = v0Phase({1000}, isSoroban);
                phase.v0Components()[0].txsMaybeDiscountedFee().txs.clear();

                scenarios[phaseId].emplace_back(phase, false,
                                                "single empty component");
            }
            {
                auto phase = validMultiComponentPhase;
                phase.v0Components()[3].txsMaybeDiscountedFee().txs.clear();

                scenarios[phaseId].emplace_back(
                    phase, false,
                    "one empty component among non-empty components");
            }
            {
                auto phase = validMultiComponentPhase;
                phase.v0Components()[1]
                    .txsMaybeDiscountedFee()
                    .txs.emplace_back(
                        phase.v0Components()[1].txsMaybeDiscountedFee().txs[5]);
                std::sort(
                    phase.v0Components()[0].txsMaybeDiscountedFee().txs.begin(),
                    phase.v0Components()[0].txsMaybeDiscountedFee().txs.end(),
                    compareTxHash);
                scenarios[phaseId].emplace_back(
                    phase, false, "duplicate txs within a component");
            }
            {
                auto phase = validMultiComponentPhase;
                std::swap(
                    phase.v0Components()[2].txsMaybeDiscountedFee().txs[4],
                    phase.v0Components()[2].txsMaybeDiscountedFee().txs[5]);

                scenarios[phaseId].emplace_back(
                    phase, false, "non-canonical tx order within component");
            }

            // Invalid scenarios specific to Soroban.
            if (isSoroban)
            {
                {
                    auto phase = validMultiComponentPhase;
                    TransactionEnvelope tx(EnvelopeType::ENVELOPE_TYPE_TX_V0);
                    tx.v0().tx.operations = phase.v0Components()[1]
                                                .txsMaybeDiscountedFee()
                                                .txs[0]
                                                .v1()
                                                .tx.operations;
                    phase.v0Components()[1].txsMaybeDiscountedFee().txs[0] = tx;
                    std::sort(phase.v0Components()[1]
                                  .txsMaybeDiscountedFee()
                                  .txs.begin(),
                              phase.v0Components()[1]
                                  .txsMaybeDiscountedFee()
                                  .txs.end(),
                              compareTxHash);
                    scenarios[phaseId].emplace_back(
                        phase, false, "v0 envelope for Soroban tx");
                }
                {
                    auto phase = validMultiComponentPhase;
                    phase.v0Components()[0]
                        .txsMaybeDiscountedFee()
                        .txs[7]
                        .v1()
                        .tx.ext.v(0);
                    std::sort(phase.v0Components()[0]
                                  .txsMaybeDiscountedFee()
                                  .txs.begin(),
                              phase.v0Components()[0]
                                  .txsMaybeDiscountedFee()
                                  .txs.end(),
                              compareTxHash);
                    scenarios[phaseId].emplace_back(
                        phase, false, "Soroban tx without extension");
                }
                {
                    auto phase = validMultiComponentPhase;
                    phase.v0Components()[3]
                        .txsMaybeDiscountedFee()
                        .txs[4]
                        .v1()
                        .tx.ext.sorobanData()
                        .resourceFee = -1;
                    std::sort(phase.v0Components()[3]
                                  .txsMaybeDiscountedFee()
                                  .txs.begin(),
                              phase.v0Components()[3]
                                  .txsMaybeDiscountedFee()
                                  .txs.end(),
                              compareTxHash);

                    scenarios[phaseId].emplace_back(
                        phase, false, "Soroban tx with negative resource fee");
                }
            }
        }
        // When doing XDR conversion we don't verify that transactions belong
        // to the correct phase, so we can also swap the phase for every
        // scenario without changing the XDR validity conditions.
        auto classicScenariosSize = scenarios[0].size();
        scenarios[0].insert(scenarios[0].end(), scenarios[1].begin(),
                            scenarios[1].end());
        scenarios[1].insert(scenarios[1].end(), scenarios[0].begin(),
                            scenarios[0].begin() + classicScenariosSize);
        // Scenarios for the Soroban parallel phase.
        auto parallelPhase = [&](std::vector<std::vector<int>> shape,
                                 bool normalize = true,
                                 std::optional<uint32_t> baseFee =
                                     std::nullopt) -> TransactionPhase {
            TransactionPhase phase(1);
            if (baseFee)
            {
                phase.parallelTxsComponent().baseFee.activate() = *baseFee;
            }
            for (auto const& stageClusters : shape)
            {
                auto& stage =
                    phase.parallelTxsComponent().executionStages.emplace_back();
                for (int txCount : stageClusters)
                {
                    auto& cluster = stage.emplace_back();

                    for (int i = 0; i < txCount; ++i)
                    {
                        auto& txEnv = cluster.emplace_back(
                            EnvelopeType::ENVELOPE_TYPE_TX);
                        buildTx(txEnv, true);
                    }

                    std::sort(cluster.begin(), cluster.end(), compareTxHash);
                }
            }
            if (normalize)
            {
                testtxset::normalizeParallelPhaseXDR(phase);
            }

            return phase;
        };

        auto prevScenariosSize = scenarios[1].size();

        // Valid scenarios
        scenarios[1].emplace_back(parallelPhase({}), true,
                                  "parallel Soroban - no txs");
        scenarios[1].emplace_back(parallelPhase({}, true, 1000), true,
                                  "parallel Soroban - no txs, fee discount");
        scenarios[1].emplace_back(parallelPhase({{10}}), true,
                                  "parallel Soroban - 1 stage, 1 cluster");
        scenarios[1].emplace_back(
            parallelPhase({{10, 2, 14, 1, 3}}), true,
            "parallel Soroban - 1 stage, multiple clusters");
        scenarios[1].emplace_back(
            parallelPhase({{1}, {3}, {2}}), true,
            "parallel Soroban - multiple single-cluster stages");

        auto validMultiStagePhase =
            parallelPhase({{2, 3, 4, 5}, {3, 2}, {5, 4, 5}, {2, 4}});
        scenarios[1].emplace_back(
            validMultiStagePhase, true,
            "parallel Soroban - multiple multi-cluster stages");
        scenarios[1].emplace_back(
            parallelPhase({{1, 2, 3, 4, 5}, {2}, {5, 4, 1}, {1, 1}}, true,
                          1000),
            true,
            "parallel Soroban - multiple multi-cluster stages with fee "
            "discount");

        // Invalid scenarios
        scenarios[1].emplace_back(
            parallelPhase({{0}}, false), false,
            "parallel Soroban - single stage with empty cluster");
        scenarios[1].emplace_back(
            parallelPhase({{2}, {}}, false), false,
            "parallel Soroban - one of the stages has no clusters");
        scenarios[1].emplace_back(
            parallelPhase({{2}, {0}, {3, 5}}, false), false,
            "parallel Soroban - one of the stages has empty cluster");
        scenarios[1].emplace_back(
            parallelPhase({{2}, {0}}, false), false,
            "parallel Soroban - one of the stages has empty cluster");
        scenarios[1].emplace_back(parallelPhase({{}, {}, {}}, false), false,
                                  "parallel Soroban - multiple empty stages");
        scenarios[1].emplace_back(
            parallelPhase({{{}, 0}, {0}, {}}, false), false,
            "parallel Soroban - multiple empty and empty cluster stages");
        scenarios[1].emplace_back(
            parallelPhase({{10, 2, 0, 1, 3}}, false), false,
            "parallel Soroban - empty cluster among non-empty ones");
        scenarios[1].emplace_back(parallelPhase({{0, 1, 0, 0, 3}}, false),
                                  false,
                                  "parallel Soroban - multiple empty clusters");
        {
            auto phase = validMultiStagePhase;
            std::swap(phase.parallelTxsComponent().executionStages[1],
                      phase.parallelTxsComponent().executionStages[2]);
            scenarios[1].emplace_back(
                phase, false, "parallel Soroban - stages incorrectly ordered");
        }
        {
            auto phase = validMultiStagePhase;
            std::swap(phase.parallelTxsComponent().executionStages[1][0],
                      phase.parallelTxsComponent().executionStages[2][1]);
            scenarios[1].emplace_back(
                phase, false,
                "parallel Soroban - clusters incorrectly ordered");
        }
        {
            auto phase = validMultiStagePhase;
            std::swap(phase.parallelTxsComponent().executionStages[1][1][0],
                      phase.parallelTxsComponent().executionStages[1][1][1]);
            scenarios[1].emplace_back(phase, false,
                                      "parallel Soroban - transactions "
                                      "incorrectly ordered within cluster");
        }
        {
            auto phase = validMultiStagePhase;
            TransactionEnvelope tx(EnvelopeType::ENVELOPE_TYPE_TX_V0);
            tx.v0().tx.operations = phase.parallelTxsComponent()
                                        .executionStages[2][1][1]
                                        .v1()
                                        .tx.operations;
            phase.parallelTxsComponent().executionStages[2][1][1] = tx;
            testtxset::normalizeParallelPhaseXDR(phase);
            scenarios[1].emplace_back(
                phase, false, "parallel Soroban - v0 envelope for Soroban tx");
        }
        {
            auto phase = validMultiStagePhase;
            phase.parallelTxsComponent().executionStages[3][0][0].v1().tx.ext.v(
                0);
            testtxset::normalizeParallelPhaseXDR(phase);
            scenarios[1].emplace_back(
                phase, false,
                "parallel Soroban - Soroban tx without extension");
        }
        {
            auto phase = validMultiStagePhase;
            phase.parallelTxsComponent()
                .executionStages[0][1][1]
                .v1()
                .tx.ext.sorobanData()
                .resourceFee = -1;
            testtxset::normalizeParallelPhaseXDR(phase);

            scenarios[1].emplace_back(
                phase, false,
                "parallel Soroban - Soroban tx with negative resource fee");
        }
        // Also add all the parallel Soroban scenarios to the classic phase -
        // this is never valid as we don't allow parallel component in classic
        // phase, but some additional coverage wouldn't hurt.
        for (size_t i = prevScenariosSize; i < scenarios[1].size(); ++i)
        {
            scenarios[0].emplace_back(scenarios[1][i]);
            std::get<1>(scenarios[0].back()) = false;
        }
        for (auto const& [classicPhase, classicIsValid, classicScenario] :
             scenarios[0])

        {
            xdrTxSet.v1TxSet().phases[0] = classicPhase;
            for (auto const& [sorobanPhase, sorobanIsValid, sorobanScenario] :
                 scenarios[1])
            {
                xdrTxSet.v1TxSet().phases[1] = sorobanPhase;
                auto txSet = TxSetXDRFrame::makeFromWire(xdrTxSet);
                INFO("Classic phase: " + classicScenario +
                     ", Soroban phase: " + sorobanScenario);
                bool valid = classicIsValid && sorobanIsValid;
                if (valid)
                {
                    REQUIRE(txSet->prepareForApply(
                                *app, app->getLedgerManager()
                                          .getLastClosedLedgerHeader()
                                          .header) != nullptr);
                }
                else
                {
                    REQUIRE(txSet->prepareForApply(
                                *app, app->getLedgerManager()
                                          .getLastClosedLedgerHeader()
                                          .header) == nullptr);
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
    cfg.GENESIS_TEST_ACCOUNT_COUNT = 10000;
    bool isParallelSoroban = protocolVersionStartsFrom(
        cfg.LEDGER_PROTOCOL_VERSION, PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION);

    Application::pointer app = createTestApplication(clock, cfg);
    overrideSorobanNetworkConfigForTest(*app);
    modifySorobanNetworkConfig(*app, [](SorobanNetworkConfig& sorobanCfg) {
        sorobanCfg.mLedgerMaxTxCount = 5;
    });

    int accountId = 0;
    auto createTxs = [&](int cnt, int fee, bool isSoroban = false) {
        std::vector<TransactionFrameBaseConstPtr> txs;
        for (int i = 0; i < cnt; ++i)
        {
            auto source = getGenesisAccount(*app, accountId++);
            if (isSoroban)
            {
                SorobanResources resources;
                resources.instructions = 800'000;
                resources.diskReadBytes = 1000;
                resources.writeBytes = 1000;
                txs.emplace_back(createUploadWasmTx(
                    *app, source, fee, DEFAULT_TEST_RESOURCE_FEE, resources,
                    /*memo=*/std::nullopt, /*addInvalidOps=*/0,
                    /*wasmSize=*/std::nullopt,
                    /*seq=*/std::nullopt,
                    /*wasmSeed=*/accountId));
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
        ApplicableTxSetFrameConstPtr applicableFrame =
            txSetFrame->prepareForApply(
                *app,
                app->getLedgerManager().getLastClosedLedgerHeader().header);
        REQUIRE(applicableFrame->checkValid(*app, 0, 0));
        GeneralizedTransactionSet newXdr;
        applicableFrame->toWireTxSetFrame()->toXDR(newXdr);
        REQUIRE(newXdr == txSetXdr);
    };

    {
        INFO("empty set");
        auto [_, applicableTxSetFrame] =
            testtxset::makeNonValidatedGeneralizedTxSet(
                {{}, {}}, *app,
                app->getLedgerManager().getLastClosedLedgerHeader().hash);

        GeneralizedTransactionSet txSetXdr;
        applicableTxSetFrame->toWireTxSetFrame()->toXDR(txSetXdr);
        REQUIRE(txSetXdr.v1TxSet().phases[0].v0Components().empty());
        checkXdrRoundtrip(txSetXdr);
    }
    {
        INFO("one discounted component set");
        auto txs = createTxs(5, 1234);
        auto ledgerHash =
            app->getLedgerManager().getLastClosedLedgerHeader().hash;
        auto [_, applicableTxSetFrame] =
            testtxset::makeNonValidatedGeneralizedTxSet(
                {{std::make_pair(1234LL, txs)}, {}}, *app, ledgerHash);

        GeneralizedTransactionSet txSetXdr;
        applicableTxSetFrame->toWireTxSetFrame()->toXDR(txSetXdr);
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
    {
        INFO("one non-discounted component set");
        auto txs = createTxs(5, 4321);
        auto ledgerHash =
            app->getLedgerManager().getLastClosedLedgerHeader().hash;
        auto [_, applicableTxSetFrame] =
            testtxset::makeNonValidatedGeneralizedTxSet(
                {{std::make_pair(std::nullopt, txs)}, {}}, *app, ledgerHash);

        GeneralizedTransactionSet txSetXdr;
        applicableTxSetFrame->toWireTxSetFrame()->toXDR(txSetXdr);
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
    {
        INFO("multiple component sets");
        auto txs1 = createTxs(3, 12345);
        auto txs2 = createTxs(1, 123);
        auto txs3 = createTxs(2, 1234);
        auto txs4 = createTxs(4, 4321);
        auto ledgerHash =
            app->getLedgerManager().getLastClosedLedgerHeader().hash;
        auto [_, applicableTxSetFrame] =
            testtxset::makeNonValidatedGeneralizedTxSet(
                {{std::make_pair(12345LL, txs1), std::make_pair(123LL, txs2),
                  std::make_pair(1234LL, txs3),
                  std::make_pair(std::nullopt, txs4)},
                 {}},
                *app, ledgerHash);

        GeneralizedTransactionSet txSetXdr;
        applicableTxSetFrame->toWireTxSetFrame()->toXDR(txSetXdr);
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
    if (protocolVersionStartsFrom(static_cast<uint32_t>(protocolVersion),
                                  PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION))
    {
        {
            INFO("parallel Soroban phase");
            auto configBeforeUpgrade =
                app->getLedgerManager().getLastClosedSorobanNetworkConfig();
            modifySorobanNetworkConfig(
                *app, [](SorobanNetworkConfig& sorobanCfg) {
                    sorobanCfg.mLedgerMaxTxCount = 2000;
                    sorobanCfg.mLedgerMaxDependentTxClusters = 50;
                });
            auto classicTxs1 = createTxs(3, 1000, false);
            auto classicTxs2 = createTxs(3, 1200, false);
            auto classicTxs3 = createTxs(3, 1500, false);
            testtxset::PhaseComponents classicPhase = {
                {std::nullopt, classicTxs1},
                {500, classicTxs2},
                {1500, classicTxs3}};
            SECTION("single stage, single cluster")
            {
                auto sorobanTxs = createTxs(20, 1234, true);
                auto ledgerHash =
                    app->getLedgerManager().getLastClosedLedgerHeader().hash;
                auto [_, applicableTxSetFrame] =
                    testtxset::makeNonValidatedGeneralizedTxSet(
                        classicPhase, 1234, {{sorobanTxs}}, *app, ledgerHash);

                GeneralizedTransactionSet txSetXdr;
                applicableTxSetFrame->toWireTxSetFrame()->toXDR(txSetXdr);
                // Smoke test classic phase
                REQUIRE(txSetXdr.v1TxSet().phases[0].v() == 0);
                REQUIRE(txSetXdr.v1TxSet().phases[0].v0Components().size() ==
                        3);
                REQUIRE(*txSetXdr.v1TxSet()
                             .phases[0]
                             .v0Components()[1]
                             .txsMaybeDiscountedFee()
                             .baseFee == 500);

                REQUIRE(txSetXdr.v1TxSet().phases[1].v() == 1);
                REQUIRE(*txSetXdr.v1TxSet()
                             .phases[1]
                             .parallelTxsComponent()
                             .baseFee == 1234);
                REQUIRE(txSetXdr.v1TxSet()
                            .phases[1]
                            .parallelTxsComponent()
                            .executionStages.size() == 1);
                REQUIRE(txSetXdr.v1TxSet()
                            .phases[1]
                            .parallelTxsComponent()
                            .executionStages[0]
                            .size() == 1);
                REQUIRE(txSetXdr.v1TxSet()
                            .phases[1]
                            .parallelTxsComponent()
                            .executionStages[0][0]
                            .size() == 20);
                checkXdrRoundtrip(txSetXdr);
            }

            {
                INFO("single stage, multiple clusters");
                auto sorobanTxs1 = createTxs(20, 1234, true);
                auto sorobanTxs2 = createTxs(15, 2000, true);
                auto sorobanTxs3 = createTxs(10, 1500, true);
                auto ledgerHash =
                    app->getLedgerManager().getLastClosedLedgerHeader().hash;
                auto [_, applicableTxSetFrame] =
                    testtxset::makeNonValidatedGeneralizedTxSet(
                        {}, 1234, {{sorobanTxs1, sorobanTxs2, sorobanTxs3}},
                        *app, ledgerHash);

                GeneralizedTransactionSet txSetXdr;
                applicableTxSetFrame->toWireTxSetFrame()->toXDR(txSetXdr);
                REQUIRE(txSetXdr.v1TxSet().phases[1].v() == 1);
                REQUIRE(*txSetXdr.v1TxSet()
                             .phases[1]
                             .parallelTxsComponent()
                             .baseFee == 1234);
                REQUIRE(txSetXdr.v1TxSet()
                            .phases[1]
                            .parallelTxsComponent()
                            .executionStages.size() == 1);
                REQUIRE(txSetXdr.v1TxSet()
                            .phases[1]
                            .parallelTxsComponent()
                            .executionStages[0]
                            .size() == 3);
                checkXdrRoundtrip(txSetXdr);
            }

            {
                INFO("multiple stages, multiple clusters");
                auto stage1Txs1 = createTxs(10, 1234, true);
                auto stage1Txs2 = createTxs(15, 2000, true);
                auto stage1Txs3 = createTxs(20, 1500, true);
                auto stage1Txs4 = createTxs(12, 1000, true);
                auto stage1Txs5 = createTxs(25, 3000, true);
                auto stage2Txs1 = createTxs(18, 1234, true);
                auto stage2Txs2 = createTxs(22, 1300, true);
                auto stage3Txs1 = createTxs(14, 4321, true);
                auto ledgerHash =
                    app->getLedgerManager().getLastClosedLedgerHeader().hash;
                auto [_, applicableTxSetFrame] =
                    testtxset::makeNonValidatedGeneralizedTxSet(
                        classicPhase, std::nullopt,
                        {{stage1Txs1, stage1Txs2, stage1Txs3, stage1Txs4,
                          stage1Txs5},
                         {stage2Txs1, stage2Txs2},
                         {stage3Txs1}},
                        *app, ledgerHash);

                GeneralizedTransactionSet txSetXdr;
                applicableTxSetFrame->toWireTxSetFrame()->toXDR(txSetXdr);
                REQUIRE(txSetXdr.v1TxSet().phases[1].v() == 1);
                REQUIRE(!txSetXdr.v1TxSet()
                             .phases[1]
                             .parallelTxsComponent()
                             .baseFee);
                REQUIRE(txSetXdr.v1TxSet()
                            .phases[1]
                            .parallelTxsComponent()
                            .executionStages.size() == 3);
                checkXdrRoundtrip(txSetXdr);
            }
            {
                INFO("apply order is shuffled");
                // each stage has a unique size for id purposes
                std::vector<std::vector<TxFrameList>> stages(10);
                for (int stageId = 0; stageId < 10; ++stageId)
                {
                    // Create stages with unique cluster counts: 3, 5, 7, 9, 11,
                    // 13, 15, 17, 19, 21
                    int clusterCount = 3 + stageId * 2;
                    for (int clusterId = 0; clusterId < clusterCount;
                         ++clusterId)
                    {
                        // Each cluster has a unique number of transactions,
                        // 50-80
                        stages[stageId].push_back(createTxs(
                            50 + (stageId + clusterId) % 31,
                            stageId * 1000 + clusterId * 10 + 1000, true));
                    }
                }
                auto [xdrTxSetFrame, applicableTxSet] =
                    testtxset::makeNonValidatedGeneralizedTxSet(
                        classicPhase, std::nullopt, stages, *app,
                        app->getLedgerManager()
                            .getLastClosedLedgerHeader()
                            .hash);
                GeneralizedTransactionSet xdrTxSet;
                xdrTxSetFrame->toXDR(xdrTxSet);
                auto const& applyOrderPhases =
                    applicableTxSet->getPhasesInApplyOrder();
                auto const& classicPhaseTxs =
                    applyOrderPhases[static_cast<size_t>(TxSetPhase::CLASSIC)]
                        .getSequentialTxs();
                REQUIRE(classicPhaseTxs.size() == 9);

                // The order is shuffled, but is deterministically defined
                // by the tx set hash, so this may only break on protocol
                // upgrade if the ledger hash changes (use a different indices
                // in that case - the chance of order being exactly the same
                // is very low).
                // We use source account here for comparison as all the source
                // accounts are unique.
                // Check that at least one classic transaction is in a different
                // position
                bool foundDifferentOrder = false;
                size_t txIndex = 0;
                auto const& components =
                    xdrTxSet.v1TxSet().phases[0].v0Components();
                for (auto const& txSetComponent : components)
                {
                    auto const& componentTxs =
                        txSetComponent.txsMaybeDiscountedFee().txs;
                    for (size_t i = 0; i < componentTxs.size(); ++i)
                    {
                        if (txIndex < classicPhaseTxs.size() &&
                            classicPhaseTxs[txIndex]->getSourceID().ed25519() !=
                                componentTxs[i].v1().tx.sourceAccount.ed25519())
                        {
                            foundDifferentOrder = true;
                            break;
                        }
                        ++txIndex;
                    }

                    if (foundDifferentOrder)
                    {
                        break;
                    }
                }
                REQUIRE(foundDifferentOrder);

                auto const& shuffledStages = xdrTxSet.v1TxSet()
                                                 .phases[1]
                                                 .parallelTxsComponent()
                                                 .executionStages;

                // Return a vector of the total number of transactions in each
                // stage. This is basically just a vector of IDs for each stage
                // in order.
                auto calcStageSizes = [](auto const& stageList) {
                    std::vector<size_t> sizes;
                    for (auto const& stage : stageList)
                    {
                        size_t total = 0;
                        for (auto const& cluster : stage)
                            total += cluster.size();
                        sizes.push_back(total);
                    }
                    return sizes;
                };

                // Check that stages are shuffled
                auto originalStageSizes = calcStageSizes(stages);
                auto shuffledStageSizes = calcStageSizes(shuffledStages);
                REQUIRE(originalStageSizes.size() == shuffledStageSizes.size());
                REQUIRE(originalStageSizes != shuffledStageSizes);

                // Check transaction-level shuffling within a randomly
                // selected cluster
                auto& randomStage = rand_element(stages);
                auto& selectedOriginalCluster = rand_element(randomStage);

                // Find corresponding shuffled stage by its size
                size_t stageIdx = std::distance(
                    stages.begin(),
                    std::find(stages.begin(), stages.end(), randomStage));
                auto stageIt = std::find(shuffledStageSizes.begin(),
                                         shuffledStageSizes.end(),
                                         originalStageSizes[stageIdx]);
                REQUIRE(stageIt != shuffledStageSizes.end());
                auto const& shuffledStage =
                    shuffledStages[stageIt - shuffledStageSizes.begin()];

                // Find matching cluster by size (each cluster has unique size)
                auto clusterIt = std::find_if(
                    shuffledStage.begin(), shuffledStage.end(),
                    [&](auto const& cluster) {
                        return cluster.size() == selectedOriginalCluster.size();
                    });
                REQUIRE(clusterIt != shuffledStage.end());
                auto const& selectedShuffledCluster = *clusterIt;

                // Verify at least one transaction is shuffled
                auto mismatchPair = std::mismatch(
                    selectedOriginalCluster.begin(),
                    selectedOriginalCluster.end(),
                    selectedShuffledCluster.begin(),
                    [](auto const& original, auto const& shuffled) {
                        return original->getSourceID().ed25519() ==
                               shuffled.v1().tx.sourceAccount.ed25519();
                    });
                REQUIRE(mismatchPair.first != selectedOriginalCluster.end());
            }
            modifySorobanNetworkConfig(*app,
                                       [&](SorobanNetworkConfig& sorobanCfg) {
                                           sorobanCfg = configBeforeUpgrade;
                                       });
        }
    }
    {
        INFO("built from transactions");
        auto getLclHeader = [&]() -> auto const&
        {
            return app->getLedgerManager().getLastClosedLedgerHeader();
        };
        std::vector<TransactionFrameBasePtr> const txs =
            createTxs(5, getLclHeader().header.baseFee, /* isSoroban */ false);
        std::vector<TransactionFrameBasePtr> const baseSorobanTxs =
            createTxs(5, 10'000'000, /* isSoroban */ true);

        {
            INFO("classic only");
            auto txSet = makeTxSetFromTransactions(txs, *app, 0, 0).first;
            GeneralizedTransactionSet txSetXdr;
            txSet->toXDR(txSetXdr);
            REQUIRE(txSetXdr.v1TxSet().phases.size() == 2);
            REQUIRE(txSetXdr.v1TxSet().phases[0].v0Components().size() == 1);
            REQUIRE(*txSetXdr.v1TxSet()
                         .phases[0]
                         .v0Components()[0]
                         .txsMaybeDiscountedFee()
                         .baseFee == getLclHeader().header.baseFee);
            REQUIRE(txSetXdr.v1TxSet()
                        .phases[0]
                        .v0Components()[0]
                        .txsMaybeDiscountedFee()
                        .txs.size() == 5);
            checkXdrRoundtrip(txSetXdr);
        }

        {
            INFO("classic and soroban");

            {
                INFO("valid");
                {
                    INFO("minimum base fee");
                    auto txSet = makeTxSetFromTransactions(
                                     {txs, baseSorobanTxs}, *app, 0, 0)
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
                            REQUIRE(phase.v() == 1);
                            REQUIRE(*phase.parallelTxsComponent().baseFee ==
                                    getLclHeader().header.baseFee);
                            REQUIRE(phase.parallelTxsComponent()
                                        .executionStages.size() == 1);
                            REQUIRE(phase.parallelTxsComponent()
                                        .executionStages[0]
                                        .size() == 1);
                            REQUIRE(phase.parallelTxsComponent()
                                        .executionStages[0][0]
                                        .size() == 5);
                        }
                        else
                        {
                            REQUIRE(phase.v() == 0);
                            REQUIRE(phase.v0Components().size() == 1);
                            REQUIRE(*phase.v0Components()[0]
                                         .txsMaybeDiscountedFee()
                                         .baseFee ==
                                    getLclHeader().header.baseFee);
                            REQUIRE(phase.v0Components()[0]
                                        .txsMaybeDiscountedFee()
                                        .txs.size() == 5);
                        }
                    }
                    checkXdrRoundtrip(txSetXdr);
                }
                {
                    INFO("higher base fee");
                    // generate more soroban txs with higher fee to trigger
                    // surge pricing
                    auto higherFeeSorobanTxs =
                        createTxs(5, 20'000'000, /* isSoroban */ true);
                    auto sorobanTxs = baseSorobanTxs;
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
                            i == static_cast<size_t>(TxSetPhase::CLASSIC)
                                ? getLclHeader().header.baseFee
                                : higherFeeSorobanTxs[0]->getInclusionFee();
                        if (i == static_cast<size_t>(TxSetPhase::SOROBAN) &&
                            isParallelSoroban)
                        {
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
            {
                INFO("invalid, soroban tx in wrong phase");
                auto sorobanTxs = baseSorobanTxs;
                sorobanTxs[4] = txs[0];
                REQUIRE_THROWS_WITH(
                    makeTxSetFromTransactions({txs, sorobanTxs}, *app, 0, 0),
                    "TxSetFrame::makeFromTransactions: phases "
                    "contain txs of wrong type");
            }
            {
                INFO("invalid, classic tx in wrong phase");
                auto classicTxs = txs;
                classicTxs[4] = baseSorobanTxs[0];
                REQUIRE_THROWS_WITH(
                    makeTxSetFromTransactions({classicTxs, baseSorobanTxs},
                                              *app, 0, 0),
                    "TxSetFrame::makeFromTransactions: phases "
                    "contain txs of wrong type");
            }
        }
    }
}

TEST_CASE("generalized tx set XDR conversion", "[txset]")
{
    SECTION("soroban protocol version")
    {
        testGeneralizedTxSetXDRConversion(SOROBAN_PROTOCOL_VERSION);
    }
    SECTION("current protocol version")
    {
        testGeneralizedTxSetXDRConversion(static_cast<ProtocolVersion>(
            Config::CURRENT_LEDGER_PROTOCOL_VERSION));
    }
    SECTION("parallel soroban protocol version")
    {
        testGeneralizedTxSetXDRConversion(
            PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION);
    }
}

TEST_CASE("applicable txset validation - Soroban phase version is correct",
          "[txset][soroban]")
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
}

TEST_CASE("applicable txset validation - transactions belong to correct phase",
          "[txset][soroban]")
{
    auto runTest = [](uint32_t protocolVersion) {
        VirtualClock clock;
        auto cfg = getTestConfig();
        cfg.LEDGER_PROTOCOL_VERSION = protocolVersion;
        cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION = protocolVersion;
        auto app = createTestApplication(clock, cfg);
        overrideSorobanNetworkConfigForTest(*app);
        auto root = app->getRoot();
        int accountId = 0;
        auto createTx = [&](bool isSoroban) {
            auto source =
                root->create("source" + std::to_string(accountId++),
                             app->getLedgerManager().getLastMinBalance(2));
            TransactionTestFramePtr tx = nullptr;
            if (isSoroban)
            {
                SorobanResources resources;
                resources.instructions = 800'000;
                resources.diskReadBytes = 1000;
                resources.writeBytes = 1000;
                tx = createUploadWasmTx(*app, source, 1000, 100'000'000,
                                        resources);
            }
            else
            {
                tx = transactionFromOperations(
                    *app, source.getSecretKey(), source.nextSequenceNumber(),
                    {createAccount(
                        getAccount(std::to_string(accountId++)).getPublicKey(),
                        1)},
                    2000);
            }
            LedgerSnapshot ls(*app);
            REQUIRE(tx->checkValid(app->getAppConnector(), ls, 0, 0, 0)
                        ->isSuccess());
            return tx;
        };

        auto validateTxSet =
            [&](std::vector<std::vector<TransactionFrameBaseConstPtr>>
                    phaseTxs) {
                std::vector<testtxset::PhaseComponents> phases(2);
                if (!phaseTxs[0].empty())
                {
                    phases[0].emplace_back(100, phaseTxs[0]);
                }
                if (!phaseTxs[1].empty())
                {
                    phases[1].emplace_back(100, phaseTxs[1]);
                }
                auto ledgerHash =
                    app->getLedgerManager().getLastClosedLedgerHeader().hash;
                auto txSet = testtxset::makeNonValidatedGeneralizedTxSet(
                                 phases, *app, ledgerHash)
                                 .second;
                REQUIRE(txSet);
                return txSet->checkValid(*app, 0, 0);
            };
        SECTION("empty phases")
        {
            REQUIRE(validateTxSet({{}, {}}));
        }
        SECTION("non-empty correct phases")
        {
            REQUIRE(validateTxSet(
                {{createTx(false), createTx(false), createTx(false)},
                 {createTx(true), createTx(true)}}));
        }
        SECTION("classic tx in Soroban phase")
        {
            REQUIRE(!validateTxSet({{}, {createTx(false)}}));
            REQUIRE(!validateTxSet(
                {{createTx(false), createTx(false), createTx(false)},
                 {createTx(true), createTx(false), createTx(true)}}));
        }
        SECTION("Soroban tx in classic phase")
        {
            REQUIRE(!validateTxSet({{createTx(true)}, {}}));
            REQUIRE(!validateTxSet(
                {{createTx(false), createTx(true), createTx(false)},
                 {createTx(true), createTx(true), createTx(true)}}));
        }
        SECTION("both phases mixed")
        {
            REQUIRE(!validateTxSet({{createTx(true)}, {createTx(false)}}));
            REQUIRE(!validateTxSet(
                {{createTx(false), createTx(true), createTx(false)},
                 {createTx(true), createTx(true), createTx(false)}}));
        }
    };
    SECTION("previous protocol")
    {
        runTest(Config::CURRENT_LEDGER_PROTOCOL_VERSION - 1);
    }
    SECTION("current protocol")
    {
        runTest(Config::CURRENT_LEDGER_PROTOCOL_VERSION);
    }
}

TEST_CASE("applicable txset validation - Soroban resources", "[txset][soroban]")
{
    auto runTest = [](uint32_t protocolVersion) {
        VirtualClock clock;
        auto cfg = getTestConfig();
        cfg.LEDGER_PROTOCOL_VERSION = protocolVersion;
        cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION = protocolVersion;

        auto app = createTestApplication(clock, cfg);
        overrideSorobanNetworkConfigForTest(*app);
        auto root = app->getRoot();

        int accountId = 0;
        int footprintId = 0;
        auto ledgerKey = [&](int id) {
            LedgerKey key(LedgerEntryType::CONTRACT_DATA);
            key.contractData().key.type(SCValType::SCV_I32);
            key.contractData().key.i32() = id;
            return key;
        };

        auto createTx = [&](std::vector<int> addRoFootprint = {},
                            std::vector<int> addRwFootprint = {},
                            bool useClassic = false) {
            auto source = root->create("source" + std::to_string(accountId++),
                                       1'000'000'000);
            Operation op;
            op.body.type(INVOKE_HOST_FUNCTION);
            op.body.invokeHostFunctionOp().hostFunction.type(
                HOST_FUNCTION_TYPE_UPLOAD_CONTRACT_WASM);

            // Make sure that our transactions are large enough so our upgrade
            // maintains at least the minimum required max tx size
            auto randomWasm = rust_bridge::get_random_wasm(
                MinimumSorobanNetworkConfig::TX_MAX_SIZE_BYTES, 0);
            op.body.invokeHostFunctionOp().hostFunction.wasm().insert(
                op.body.invokeHostFunctionOp().hostFunction.wasm().begin(),
                randomWasm.data.data(),
                randomWasm.data.data() + randomWasm.data.size());

            SorobanResources resources;
            resources.instructions =
                MinimumSorobanNetworkConfig::TX_MAX_INSTRUCTIONS;
            resources.diskReadBytes = 5'000;
            resources.writeBytes =
                MinimumSorobanNetworkConfig::TX_MAX_WRITE_BYTES;

            if (useClassic)
            {
                UnorderedSet<LedgerKey> seenKeys;
                auto keys =
                    LedgerTestUtils::generateValidUniqueLedgerKeysWithTypes(
                        {ACCOUNT}, 10, seenKeys);
                for (auto const& key : keys)
                {
                    resources.footprint.readOnly.push_back(key);
                }
            }
            else
            {

                for (int i = 0; i < 8; ++i)
                {
                    resources.footprint.readOnly.push_back(
                        ledgerKey(footprintId++));
                }
                for (int i = 0; i < 2; ++i)
                {
                    resources.footprint.readWrite.push_back(
                        ledgerKey(footprintId++));
                }
                for (auto id : addRoFootprint)
                {
                    resources.footprint.readOnly.push_back(
                        ledgerKey(1'000'000'000 + id));
                }
                for (auto id : addRwFootprint)
                {
                    resources.footprint.readWrite.push_back(
                        ledgerKey(1'000'000'000 + id));
                }
            }

            auto tx = sorobanTransactionFrameFromOps(
                app->getNetworkID(), source, {op}, {}, resources, 2000,
                100'000'000);
            LedgerSnapshot ls(*app);
            REQUIRE(tx->checkValid(app->getAppConnector(), ls, 0, 0, 0)
                        ->isSuccess());
            return tx;
        };

        SECTION("individual ledger resource limits")
        {
            auto txSize = xdr::xdr_size(createTx()->getEnvelope());
            // Update the ledger limits to the minimum values that
            // accommodate 20 txs created by `createTx()`.
            modifySorobanNetworkConfig(
                *app, [&](SorobanNetworkConfig& sorobanCfg) {
                    auto const txCount = 20;
                    sorobanCfg.mLedgerMaxTxCount = txCount;

                    sorobanCfg.mTxMaxInstructions =
                        MinimumSorobanNetworkConfig::TX_MAX_INSTRUCTIONS;
                    sorobanCfg.mLedgerMaxInstructions =
                        txCount * sorobanCfg.mTxMaxInstructions;

                    sorobanCfg.mTxMaxDiskReadBytes = 5000;
                    sorobanCfg.mLedgerMaxDiskReadBytes =
                        txCount * sorobanCfg.mTxMaxDiskReadBytes;

                    sorobanCfg.mTxMaxWriteBytes =
                        MinimumSorobanNetworkConfig::TX_MAX_WRITE_BYTES;
                    sorobanCfg.mLedgerMaxWriteBytes =
                        txCount * sorobanCfg.mTxMaxWriteBytes;

                    sorobanCfg.mTxMaxDiskReadEntries = 10;
                    sorobanCfg.mLedgerMaxDiskReadEntries =
                        txCount * sorobanCfg.mTxMaxDiskReadEntries;

                    sorobanCfg.mTxMaxWriteLedgerEntries = 2;
                    sorobanCfg.mLedgerMaxWriteLedgerEntries =
                        txCount * sorobanCfg.mTxMaxWriteLedgerEntries;

                    sorobanCfg.mTxMaxSizeBytes = txSize;
                    sorobanCfg.mLedgerMaxTransactionsSizeBytes =
                        txCount * sorobanCfg.mTxMaxSizeBytes;

                    if (protocolVersionStartsFrom(
                            protocolVersion,
                            PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION))
                    {
                        sorobanCfg.mLedgerMaxDependentTxClusters = 4;
                        // Technically we could use /= 4 here, but that
                        // would make for a less interesting scenario as
                        // every stage will need to have exactly the same
                        // clusters.
                        sorobanCfg.mLedgerMaxInstructions /= 2;
                    }
                });

            auto buildAndValidate = [&](bool diskReadTest = false) {
                std::vector<TransactionFrameBaseConstPtr> txs;
                for (int i = 0; i < 20; ++i)
                {
                    txs.push_back(createTx({}, {}, diskReadTest));
                }
                ApplicableTxSetFrameConstPtr txSet;
                auto ledgerHash =
                    app->getLedgerManager().getLastClosedLedgerHeader().hash;
                if (protocolVersionIsBefore(
                        protocolVersion,
                        PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION))
                {
                    txSet = testtxset::makeNonValidatedGeneralizedTxSet(
                                {{}, {{1000, txs}}}, *app, ledgerHash)
                                .second;
                }
                else
                {
                    auto takeTxs = [&](int from, int to) {
                        return std::vector<TransactionFrameBaseConstPtr>(
                            txs.begin() + from, txs.begin() + to);
                    };
                    TxStageFrameList txsPerStage = {
                        // 3 sequential transactions
                        {
                            takeTxs(0, 1),
                            takeTxs(1, 3),
                            takeTxs(3, 6),
                            takeTxs(6, 8),
                        },
                        // 4 sequential transactions
                        {
                            takeTxs(8, 12),
                            takeTxs(12, 13),
                            takeTxs(13, 15),
                        },
                        // 3 sequential transactions
                        {
                            takeTxs(15, 17),
                            takeTxs(17, 20),
                        },
                        // 10 sequential transactions total, accounting for
                        // a
                        // half of ledger max instructions.
                    };
                    txSet = testtxset::makeNonValidatedGeneralizedTxSet(
                                {}, 1234, txsPerStage, *app, ledgerHash)
                                .second;
                }
                return txSet->checkValid(*app, 0, 0);
            };
            SECTION("sufficient resources")
            {
                REQUIRE(buildAndValidate());
            }
            SECTION("instruction limit exceeded")
            {
                modifySorobanNetworkConfig(
                    *app, [&](SorobanNetworkConfig& sorobanCfg) {
                        sorobanCfg.mLedgerMaxInstructions -= 1;
                    });
                REQUIRE(!buildAndValidate());
            }
            SECTION("read bytes limit exceeded")
            {
                modifySorobanNetworkConfig(
                    *app, [&](SorobanNetworkConfig& sorobanCfg) {
                        sorobanCfg.mLedgerMaxDiskReadBytes -= 1;
                    });
                REQUIRE(!buildAndValidate());
            }
            SECTION("write bytes limit exceeded")
            {
                modifySorobanNetworkConfig(
                    *app, [&](SorobanNetworkConfig& sorobanCfg) {
                        sorobanCfg.mLedgerMaxWriteBytes -= 1;
                    });
                REQUIRE(!buildAndValidate());
            }
            SECTION("read entries limit exceeded")
            {
                modifySorobanNetworkConfig(
                    *app, [&](SorobanNetworkConfig& sorobanCfg) {
                        sorobanCfg.mLedgerMaxDiskReadEntries -= 1;
                    });
                bool useClassic = protocolVersionStartsFrom(
                    protocolVersion, ProtocolVersion::V_23);
                REQUIRE(!buildAndValidate(useClassic));
            }
            SECTION("write entries limit exceeded")
            {
                modifySorobanNetworkConfig(
                    *app, [&](SorobanNetworkConfig& sorobanCfg) {
                        sorobanCfg.mLedgerMaxWriteLedgerEntries -= 1;
                    });
                REQUIRE(!buildAndValidate());
            }
            SECTION("tx size limit exceeded")
            {
                modifySorobanNetworkConfig(
                    *app, [&](SorobanNetworkConfig& sorobanCfg) {
                        sorobanCfg.mLedgerMaxTransactionsSizeBytes -= 1;
                    });
                REQUIRE(!buildAndValidate());
            }
            SECTION("tx count limit exceeded")
            {
                modifySorobanNetworkConfig(
                    *app, [&](SorobanNetworkConfig& sorobanCfg) {
                        sorobanCfg.mLedgerMaxTxCount -= 1;
                    });
                REQUIRE(!buildAndValidate());
            }
            if (protocolVersionStartsFrom(
                    protocolVersion, PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION))
            {
                SECTION("dependent clusters limit exceeded")
                {
                    modifySorobanNetworkConfig(
                        *app, [&](SorobanNetworkConfig& sorobanCfg) {
                            sorobanCfg.mLedgerMaxDependentTxClusters -= 1;
                        });
                    REQUIRE(!buildAndValidate());
                }
            }
        }
        if (protocolVersionStartsFrom(protocolVersion,
                                      PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION))
        {
            SECTION("data dependency validation")
            {

                auto buildAndValidate = [&](TxStageFrameList txsPerStage) {
                    auto ledgerHash = app->getLedgerManager()
                                          .getLastClosedLedgerHeader()
                                          .hash;
                    auto txSet = testtxset::makeNonValidatedGeneralizedTxSet(
                                     {}, 1234, txsPerStage, *app, ledgerHash)
                                     .second;
                    return txSet->checkValid(*app, 0, 0);
                };

                // Relax the per-ledger limits to ensure we're not running
                // into any.
                modifySorobanNetworkConfig(
                    *app, [&](SorobanNetworkConfig& sorobanCfg) {
                        sorobanCfg.mLedgerMaxInstructions =
                            std::numeric_limits<int64_t>::max();
                        sorobanCfg.mLedgerMaxDiskReadBytes =
                            std::numeric_limits<uint32_t>::max();
                        sorobanCfg.mLedgerMaxWriteBytes =
                            std::numeric_limits<uint32_t>::max();
                        sorobanCfg.mLedgerMaxDiskReadEntries =
                            std::numeric_limits<uint32_t>::max();
                        sorobanCfg.mLedgerMaxWriteLedgerEntries =
                            std::numeric_limits<uint32_t>::max();
                        sorobanCfg.mLedgerMaxTxCount =
                            std::numeric_limits<uint32_t>::max();
                        sorobanCfg.mLedgerMaxTransactionsSizeBytes =
                            std::numeric_limits<uint32_t>::max();
                        sorobanCfg.mLedgerMaxDependentTxClusters = 100;
                    });
                TxStageFrameList nonConflictingTxsPerStage = {
                    {
                        // Cluster with RO-RW conflict
                        {createTx({1}, {2}), createTx({2}, {3})},
                        // Cluster with RW-RW conflict
                        {createTx({1, 4}, {5}), createTx({1, 6}, {5})},
                        // Cluster without conflicts
                        {createTx({1, 4, 7}, {8}), createTx({6, 7}, {9})},
                    },
                    {
                        // Cluster that would conflict with every
                        // cluster in previous stage
                        {createTx({}, {1, 2, 3, 4, 5, 6, 7, 8}),
                         createTx({1, 2, 3}, {4, 5, 6}),
                         createTx({1, 2}, {3, 4})},
                        // Cluster without conflicts
                        {createTx({9}, {10}), createTx({9}, {11}),
                         createTx({9}, {12, 13})},
                    }};
                SECTION("no dependencies between clusters")
                {
                    REQUIRE(buildAndValidate(nonConflictingTxsPerStage));
                }
                SECTION("RO-RW conflict")
                {
                    TxStageFrameList txsPerStage = {{
                        {createTx({1}, {})},
                        {createTx({}, {1})},
                    }};
                    REQUIRE(!buildAndValidate(txsPerStage));
                }
                SECTION("RW-RW conflict")
                {
                    TxStageFrameList txsPerStage = {{
                        {createTx({}, {1})},
                        {createTx({}, {1})},
                    }};
                    REQUIRE(!buildAndValidate(txsPerStage));
                }
                SECTION("one stage with a conflict")
                {
                    auto txsPerStage = nonConflictingTxsPerStage;
                    txsPerStage.push_back({
                        {createTx({1}, {})},
                        {createTx({}, {1})},
                    });
                    REQUIRE(!buildAndValidate(txsPerStage));
                }
                SECTION("one cluster conflict among multiple clusters")
                {
                    auto txsPerStage = nonConflictingTxsPerStage;
                    txsPerStage[0][2].push_back(createTx({}, {5}));
                    REQUIRE(!buildAndValidate(txsPerStage));
                }
                SECTION("multiple conflicting clusters")
                {
                    auto txsPerStage = nonConflictingTxsPerStage;
                    txsPerStage[0][2].push_back(createTx({9, 10}, {12, 13, 4}));
                    txsPerStage[1].emplace_back().push_back(
                        createTx({9, 10}, {12, 13, 8}));
                    REQUIRE(!buildAndValidate(txsPerStage));
                }
            }
        }
    };

    SECTION("previous protocol")
    {
        runTest(Config::CURRENT_LEDGER_PROTOCOL_VERSION - 1);
    }
    SECTION("current protocol")
    {
        runTest(Config::CURRENT_LEDGER_PROTOCOL_VERSION);
    }
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
    auto root = app->getRoot();
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
                root->create("unique " + std::to_string(accountId),
                             app->getLedgerManager().getLastMinBalance(2));
            return transactionFromOperations(*app, source.getSecretKey(),
                                             source.nextSequenceNumber(), ops,
                                             fee);
        }
        else
        {
            return transactionFromOperations(*app, root->getSecretKey(),
                                             root->nextSequenceNumber(), ops,
                                             fee);
        }
    };

    SECTION("invalid")
    {
        auto tx1 = createTx(1, 1000, false);
        auto tx2 = createTx(3, 1500, false);
        auto ledgerHash =
            app->getLedgerManager().getLastClosedLedgerHeader().hash;
        auto txSet =
            testtxset::makeNonValidatedGeneralizedTxSet(
                {{std::make_pair(
                     500, std::vector<TransactionFrameBasePtr>{tx1, tx2})},
                 {}},
                *app, ledgerHash)
                .second;

        REQUIRE(!txSet->checkValid(*app, 0, 0));
    }
    SECTION("valid")
    {
        auto tx1 = createTx(1, 1000, true);
        auto tx2 = createTx(3, 1500, true);
        auto ledgerHash =
            app->getLedgerManager().getLastClosedLedgerHeader().hash;
        auto txSet =
            testtxset::makeNonValidatedGeneralizedTxSet(
                {{std::make_pair(
                     500, std::vector<TransactionFrameBasePtr>{tx1, tx2})},
                 {}},
                *app, ledgerHash)
                .second;

        REQUIRE(txSet->checkValid(*app, 0, 0));
    }
    SECTION("invalid, classic and soroban")
    {
        SorobanResources resources;
        resources.instructions = 800'000;
        resources.diskReadBytes = 1000;
        resources.writeBytes = 1000;
        uint32_t inclusionFee = 500;
        int64_t resourceFee = sorobanResourceFee(*app, resources, 5000, 100);
        auto sorobanTx = createUploadWasmTx(*app, *root, inclusionFee,
                                            resourceFee, resources);
        // Make sure fees got computed correctly
        REQUIRE(sorobanTx->getInclusionFee() == inclusionFee);

        auto tx1 = createTx(1, 1000, false);
        auto tx2 = createTx(3, 1500, false);
        auto ledgerHash =
            app->getLedgerManager().getLastClosedLedgerHeader().hash;
        auto txSet =
            testtxset::makeNonValidatedGeneralizedTxSet(
                {{std::make_pair(
                     500, std::vector<TransactionFrameBasePtr>{tx1, tx2})},
                 {std::make_pair(
                     500, std::vector<TransactionFrameBasePtr>{sorobanTx})}},
                *app, ledgerHash)
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
    auto root = app->getRoot();
    int accountId = 1;

    auto createTx = [&](int opCnt, int inclusionFee, bool isSoroban = false,
                        bool validateTx = true) {
        auto source =
            root->create("unique " + std::to_string(accountId++),
                         app->getLedgerManager().getLastMinBalance(2));
        if (isSoroban)
        {
            SorobanResources resources;
            resources.instructions = 800'000;
            resources.diskReadBytes = 1000;
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
        testtxset::PhaseComponents sorobanTxs;
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

        // Create all transactions first to avoid order of evaluation issues
        auto tx1_1000 = createTx(1, 1000);
        auto tx3_1500 = createTx(3, 1500);
        auto tx4_5000 = createTx(4, 5000);
        auto tx1_1000_dup = createTx(1, 1000);
        auto tx5_6000 = createTx(5, 6000);
        auto tx2_10000 = createTx(2, 10000);
        auto tx5_100000 = createTx(5, 100000);

        // Get the ledger header hash after all transactions are created
        auto ledgerHash =
            app->getLedgerManager().getLastClosedLedgerHeader().hash;

        auto txSet =
            testtxset::makeNonValidatedGeneralizedTxSet(
                {{std::make_pair(
                      500,
                      std::vector<TransactionFrameBasePtr>{tx1_1000, tx3_1500}),
                  std::make_pair(1000,
                                 std::vector<TransactionFrameBasePtr>{
                                     tx4_5000, tx1_1000_dup, tx5_6000}),
                  std::make_pair(std::nullopt,
                                 std::vector<TransactionFrameBasePtr>{
                                     tx2_10000, tx5_100000})},
                 sorobanTxs},
                *app, ledgerHash)
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
            auto tx = createTx(2, 999);
            auto ledgerHash =
                app->getLedgerManager().getLastClosedLedgerHeader().hash;
            auto txSet =
                testtxset::makeNonValidatedGeneralizedTxSet(
                    {{std::make_pair(500,
                                     std::vector<TransactionFrameBasePtr>{tx})},
                     {}},
                    *app, ledgerHash)
                    .second;

            REQUIRE(!txSet->checkValid(*app, 0, 0));
        }
        SECTION("soroban")
        {
            auto tx = createTx(1, 499, /* isSoroban */ true);
            auto ledgerHash =
                app->getLedgerManager().getLastClosedLedgerHeader().hash;
            auto txSet =
                testtxset::makeNonValidatedGeneralizedTxSet(
                    {{},
                     {std::make_pair(
                         500, std::vector<TransactionFrameBasePtr>{tx})}},
                    *app, ledgerHash)
                    .second;

            REQUIRE(!txSet->checkValid(*app, 0, 0));
        }
    }

    SECTION("tx with too low non-discounted fee")
    {
        SECTION("classic")
        {
            auto tx = createTx(2, 199);
            auto ledgerHash =
                app->getLedgerManager().getLastClosedLedgerHeader().hash;
            auto txSet =
                testtxset::makeNonValidatedGeneralizedTxSet(
                    {{std::make_pair(std::nullopt,
                                     std::vector<TransactionFrameBasePtr>{tx})},
                     {}},
                    *app, ledgerHash)
                    .second;

            REQUIRE(!txSet->checkValid(*app, 0, 0));
        }
        SECTION("soroban")
        {
            auto tx = createTx(1, 99, true, false);
            auto ledgerHash =
                app->getLedgerManager().getLastClosedLedgerHeader().hash;
            auto txSet = testtxset::makeNonValidatedGeneralizedTxSet(
                             {{},
                              {std::make_pair(
                                  std::nullopt,
                                  std::vector<TransactionFrameBasePtr>{tx})}},
                             *app, ledgerHash)
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
            "insns,disk_read_bytes,write_bytes,disk_read_entries,write_"
            "entries,tx_size_bytes");
        Config cfg(getTestConfig(0, Config::TESTDB_BUCKET_DB_PERSISTENT));
        cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION = protocolVersion;
        cfg.SOROBAN_PHASE_MIN_STAGE_COUNT = 1;
        cfg.SOROBAN_PHASE_MAX_STAGE_COUNT = 1;
        cfg.GENESIS_TEST_ACCOUNT_COUNT = 1000;

        cfg.NODE_SEED = SecretKey::pseudoRandomForTestingFromSeed(54321);

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

        // We store the `SecretKey` instead of the `TestAccount` in this vector
        // so that we can create a vector without dangling references to `app`
        // inside `runIteration`
        std::vector<std::pair<SecretKey, int64_t>> accountKeys;
        {
            VirtualClock clock;
            Application::pointer app = createTestApplication(clock, cfg, true);
            auto root = app->getRoot();
            for (int i = 0; i < 1000; ++i)
            {
                auto acc = getGenesisAccount(*app, i).getSecretKey();
                accountKeys.emplace_back(acc, 0);
            }
        }

        auto runIteration = [&]() {
            int classicOpsCount = txCountDistr(rng);
            int feeBumpFraction = feeBumpFractionDistr(rng);

            LedgerUpgrade upgrade(LEDGER_UPGRADE_MAX_TX_SET_SIZE);
            upgrade.newMaxTxSetSize() = classicOpsCount;
            xdr::xvector<UpgradeType, 6> upgrades;
            LedgerUpgrade ledgerUpgrade(LEDGER_UPGRADE_MAX_TX_SET_SIZE);
            ledgerUpgrade.newMaxTxSetSize() = txCountDistr(rng);

            cfg.MAX_DEX_TX_OPERATIONS_IN_TX_SET =
                stellar::uniform_int_distribution<>(0, classicOpsCount)(rng);

            // Note: we restart app every iteration so that the config updates
            // apply: in particular, MAX_DEX_TX_OPERATIONS_IN_TX_SET is read at
            // construction time by some downstream users
            VirtualClock clock;
            Application::pointer app = createTestApplication(clock, cfg, false);
            std::vector<std::pair<TestAccount, int64_t>> accounts;
            for (const auto& [key, seqNo] : accountKeys)
            {
                accounts.emplace_back(TestAccount{*app, key, seqNo}, seqNo + 1);
            }
            auto root = app->getRoot();

            auto v = xdr::xdr_to_opaque(ledgerUpgrade);
            upgrades.push_back(UpgradeType(v.begin(), v.end()));
            closeLedger(*app, {}, false, upgrades);

            modifySorobanNetworkConfig(*app, [&](SorobanNetworkConfig& cfg) {
                cfg.mLedgerMaxTxCount = txCountDistr(rng);

                cfg.mLedgerMaxWriteLedgerEntries = ledgerEntriesDistr(rng);
                cfg.mTxMaxWriteLedgerEntries =
                    static_cast<int64_t>(cfg.mLedgerMaxWriteLedgerEntries) *
                    100 / txToLedgerRatioPercentDistr(rng);

                cfg.mLedgerMaxDiskReadEntries =
                    cfg.mLedgerMaxWriteLedgerEntries + ledgerEntriesDistr(rng);
                cfg.mTxMaxDiskReadEntries =
                    static_cast<int64_t>(cfg.mLedgerMaxDiskReadEntries) * 100 /
                    txToLedgerRatioPercentDistr(rng);

                cfg.mLedgerMaxDiskReadBytes = ledgerBytesDistr(rng);
                cfg.mTxMaxDiskReadBytes =
                    static_cast<int64_t>(cfg.mLedgerMaxDiskReadBytes) * 100 /
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
                if (protocolVersionStartsFrom(
                        protocolVersion,
                        PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION))
                {
                    cfg.mTxMaxFootprintEntries = cfg.mTxMaxDiskReadEntries +
                                                 cfg.mTxMaxWriteLedgerEntries;
                }
            });

            auto const& sorobanConfig =
                app->getLedgerManager().getLastClosedSorobanNetworkConfig();
            stellar::uniform_int_distribution<> txReadEntriesDistr(
                1, sorobanConfig.txMaxDiskReadEntries());

            stellar::uniform_int_distribution<> txReadBytesDistr(
                50, sorobanConfig.txMaxDiskReadBytes());
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
                        ops.emplace_back(payment(*root, j));
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
                resources.diskReadBytes = txReadBytesDistr(rng);
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
                totalReadBytes += resources.diskReadBytes;
                totalWriteBytes += resources.writeBytes;

                // In protocol 23, all generated entries are in-memory.
                if (protocolVersionIsBefore(
                        protocolVersion,
                        PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION))
                {
                    totalReadEntries += resources.footprint.readOnly.size() +
                                        resources.footprint.readWrite.size();
                }

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
                auto const& sorobanComponent =
                    xdrTxSet.v1TxSet().phases[1].parallelTxsComponent();
                if (sorobanComponent.baseFee)
                {
                    sorobanBaseFee = *sorobanComponent.baseFee;
                }
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

void
runParallelTxSetBuildingTest(bool variableStageCount)
{
    int const STAGE_COUNT = 4;
    int const CLUSTER_COUNT = 8;

    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.LEDGER_PROTOCOL_VERSION =
        static_cast<uint32_t>(PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION);
    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
        static_cast<uint32_t>(PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION);
    cfg.SOROBAN_PHASE_MIN_STAGE_COUNT = variableStageCount ? 1 : STAGE_COUNT;
    cfg.SOROBAN_PHASE_MAX_STAGE_COUNT = STAGE_COUNT;
    // Temporary set the limits override very high in order for the upgrades
    // to pass (with 4 stages we have not enough insns for an upgrade tx to go
    // through).
    cfg.TESTING_SOROBAN_HIGH_LIMIT_OVERRIDE = true;
    cfg.GENESIS_TEST_ACCOUNT_COUNT = 10000;
    Application::pointer app = createTestApplication(clock, cfg);

    modifySorobanNetworkConfig(*app, [&](SorobanNetworkConfig& sorobanCfg) {
        sorobanCfg.mTxMaxInstructions = 100'000'000;
        sorobanCfg.mLedgerMaxInstructions = 400'000'000;
        sorobanCfg.mTxMaxDiskReadEntries = 3000;
        sorobanCfg.mTxMaxFootprintEntries = sorobanCfg.mTxMaxDiskReadEntries;
        sorobanCfg.mLedgerMaxDiskReadEntries = 3000;
        sorobanCfg.mTxMaxWriteLedgerEntries = 2000;
        sorobanCfg.mTxMaxWriteLedgerEntries = 2000;
        sorobanCfg.mLedgerMaxWriteLedgerEntries = 2000;
        sorobanCfg.mTxMaxDiskReadBytes = 1'000'000;
        sorobanCfg.mLedgerMaxDiskReadBytes = 1'000'000;
        sorobanCfg.mTxMaxWriteBytes = 100'000;
        sorobanCfg.mLedgerMaxWriteBytes = 100'000;
        sorobanCfg.mLedgerMaxTxCount = 1000;
        sorobanCfg.mLedgerMaxDependentTxClusters = CLUSTER_COUNT;
    });
    int accountId = 0;
    SCAddress contract(SC_ADDRESS_TYPE_CONTRACT);

    auto generateKey = [&contract](int i) {
        return stellar::contractDataKey(
            contract, txtest::makeU32(i),
            i % 2 == 0 ? ContractDataDurability::PERSISTENT
                       : ContractDataDurability::TEMPORARY);
    };

    auto createTx = [&](int instructions, std::vector<int> const& roKeys,
                        std::vector<int> rwKeys, int64_t inclusionFee = 1000,
                        int readBytes = 1000, int writeBytes = 100) {
        auto source = getGenesisAccount(*app, accountId++);
        SorobanResources resources;
        resources.instructions = instructions;
        resources.diskReadBytes = readBytes;
        resources.writeBytes = writeBytes;
        for (auto roKeyId : roKeys)
        {
            resources.footprint.readOnly.push_back(generateKey(roKeyId));
        }
        for (auto rwKeyId : rwKeys)
        {
            resources.footprint.readWrite.push_back(generateKey(rwKeyId));
        }
        auto resourceFee = sorobanResourceFee(*app, resources, 10'000, 40);
        // It doesn't really matter what tx does as we're only interested in
        // its resources.
        auto tx = createUploadWasmTx(*app, source, inclusionFee, resourceFee,
                                     resources);
        LedgerSnapshot ls(*app);
        REQUIRE(
            tx->checkValid(app->getAppConnector(), ls, 0, 0, 0)->isSuccess());

        return tx;
    };

    auto validateShape = [&](ApplicableTxSetFrame const& txSet,
                             size_t stageCount, size_t clustersPerStage,
                             size_t txsPerCluster) {
        auto const& phase =
            txSet.getPhase(TxSetPhase::SOROBAN).getParallelStages();

        REQUIRE(phase.size() == stageCount);
        for (auto const& stage : phase)
        {
            REQUIRE(stage.size() == clustersPerStage);
            for (auto const& cluster : stage)
            {
                REQUIRE(cluster.size() == txsPerCluster);
            }
        }
    };

    auto validateBaseFee = [&](ApplicableTxSetFrame const& txSet,
                               int64_t baseFee) {
        for (auto const& tx : txSet.getPhase(TxSetPhase::SOROBAN))
        {
            REQUIRE(*txSet.getTxBaseFee(tx) == baseFee);
        }
    };

    auto runWithConfigUpgrade = [&](auto&& configUpgradeFn, auto&& fn) {
        SorobanNetworkConfig preUpgradeConfig =
            app->getLedgerManager().getLastClosedSorobanNetworkConfig();
        modifySorobanNetworkConfig(*app, configUpgradeFn);
        fn();
        modifySorobanNetworkConfig(*app, [&](SorobanNetworkConfig& config) {
            config = preUpgradeConfig;
        });
    };
    {
        INFO("no conflicts");
        {
            INFO("single stage");
            std::vector<TransactionFrameBaseConstPtr> sorobanTxs;
            for (int i = 0; i < CLUSTER_COUNT; ++i)
            {
                sorobanTxs.push_back(createTx(100'000'000, {4 * i, 4 * i + 1},
                                              {4 * i + 2, 4 * i + 3}));
            }
            PerPhaseTransactionList phases = {{}, sorobanTxs};
            auto [_, txSet] = makeTxSetFromTransactions(phases, *app, 0, 0);
            // We have a single stage with both variable and fixed stage count,
            // but in the former case this case will be large (full
            // instructions limit), and in the latter case it will be
            // STAGE_COUNT times smaller and thus have more clusters.
            if (variableStageCount)
            {
                validateShape(*txSet, 1, CLUSTER_COUNT / STAGE_COUNT,
                              STAGE_COUNT);
            }
            else
            {
                validateShape(*txSet, 1, CLUSTER_COUNT, 1);
            }

            validateBaseFee(*txSet, 100);
        }
        {
            INFO("all stages");
            std::vector<TransactionFrameBaseConstPtr> sorobanTxs;
            for (int i = 0; i < STAGE_COUNT * CLUSTER_COUNT; ++i)
            {
                sorobanTxs.push_back(createTx(100'000'000, {4 * i, 4 * i + 1},
                                              {4 * i + 2, 4 * i + 3}));
            }
            PerPhaseTransactionList phases = {{}, sorobanTxs};
            auto [_, txSet] = makeTxSetFromTransactions(phases, *app, 0, 0);
            if (variableStageCount)
            {
                validateShape(*txSet, 1, CLUSTER_COUNT, STAGE_COUNT);
            }
            else
            {
                validateShape(*txSet, STAGE_COUNT, CLUSTER_COUNT, 1);
            }

            validateBaseFee(*txSet, 100);
        }
        {
            INFO("all stages, smaller txs");
            std::vector<TransactionFrameBaseConstPtr> sorobanTxs;
            for (int i = 0; i < STAGE_COUNT * CLUSTER_COUNT * 5; ++i)
            {
                sorobanTxs.push_back(createTx(20'000'000, {4 * i, 4 * i + 1},
                                              {4 * i + 2, 4 * i + 3}));
            }
            PerPhaseTransactionList phases = {{}, sorobanTxs};
            auto [_, txSet] = makeTxSetFromTransactions(phases, *app, 0, 0);
            if (variableStageCount)
            {
                validateShape(*txSet, 1, CLUSTER_COUNT, STAGE_COUNT * 5);
            }
            else
            {
                validateShape(*txSet, STAGE_COUNT, CLUSTER_COUNT, 5);
            }
            validateBaseFee(*txSet, 100);
        }
        {
            INFO("all stages, smaller txs with prioritization");
            std::vector<TransactionFrameBaseConstPtr> sorobanTxs;
            for (int i = 0; i < STAGE_COUNT * CLUSTER_COUNT * 10; ++i)
            {
                sorobanTxs.push_back(createTx(
                    20'000'000, {4 * i, 4 * i + 1}, {4 * i + 2, 4 * i + 3},
                    /* inclusionFee*/ (i + 1) * 1000LL));
            }
            PerPhaseTransactionList phases = {{}, sorobanTxs};
            auto [_, txSet] = makeTxSetFromTransactions(phases, *app, 0, 0);
            if (variableStageCount)
            {
                validateShape(*txSet, 1, CLUSTER_COUNT, STAGE_COUNT * 5);
            }
            else
            {
                validateShape(*txSet, STAGE_COUNT, CLUSTER_COUNT, 5);
            }
            validateBaseFee(
                *txSet, 10LL * STAGE_COUNT * CLUSTER_COUNT * 1000 / 2 + 1000);
        }

        {
            INFO("instruction limit reached");
            runWithConfigUpgrade(
                [](SorobanNetworkConfig& sorobanCfg) {
                    sorobanCfg.mTxMaxInstructions = 2'500'000;
                    sorobanCfg.mLedgerMaxInstructions = 10'000'000;
                },
                [&]() {
                    std::vector<TransactionFrameBaseConstPtr> sorobanTxs;
                    for (int i = 0; i < STAGE_COUNT * CLUSTER_COUNT * 4; ++i)
                    {
                        sorobanTxs.push_back(
                            createTx(2'500'000, {4 * i, 4 * i + 1},
                                     {4 * i + 2, 4 * i + 3},
                                     /* inclusionFee */ 100 + i));
                    }
                    PerPhaseTransactionList phases = {{}, sorobanTxs};
                    auto [_, txSet] =
                        makeTxSetFromTransactions(phases, *app, 0, 0);

                    if (variableStageCount)
                    {
                        validateShape(*txSet, 1, CLUSTER_COUNT, STAGE_COUNT);
                    }
                    else
                    {
                        validateShape(*txSet, STAGE_COUNT, CLUSTER_COUNT, 1);
                    }

                    validateBaseFee(*txSet,
                                    100 + STAGE_COUNT * CLUSTER_COUNT * 4 -
                                        STAGE_COUNT * CLUSTER_COUNT);
                });
        }
        {
            INFO("read bytes limit reached");
            std::vector<TransactionFrameBaseConstPtr> sorobanTxs;
            for (int i = 0; i < STAGE_COUNT * CLUSTER_COUNT; ++i)
            {
                sorobanTxs.push_back(createTx(1'000'000, {4 * i, 4 * i + 1},
                                              {4 * i + 2, 4 * i + 3},
                                              /* inclusionFee */ 100 + i,
                                              /* readBytes */ 100'000));
            }
            PerPhaseTransactionList phases = {{}, sorobanTxs};
            auto [_, txSet] = makeTxSetFromTransactions(phases, *app, 0, 0);
            validateShape(*txSet, 1, 1, 10);
            validateBaseFee(*txSet, 100 + STAGE_COUNT * CLUSTER_COUNT - 10);
        }
        {
            INFO("read entries limit reached");

            runWithConfigUpgrade(
                [](SorobanNetworkConfig& sorobanCfg) {
                    sorobanCfg.mTxMaxDiskReadEntries = 4 * 10 + 3;
                    sorobanCfg.mLedgerMaxDiskReadEntries = 4 * 10 + 3;
                },
                [&]() {
                    std::vector<TransactionFrameBaseConstPtr> sorobanTxs;
                    for (int i = 0; i < STAGE_COUNT * CLUSTER_COUNT; ++i)
                    {
                        sorobanTxs.push_back(
                            createTx(1'000'000, {4 * i, 4 * i + 1},
                                     {4 * i + 2, 4 * i + 3},
                                     /* inclusionFee */ 100 + i,
                                     /* readBytes */ 100'000));
                    }
                    PerPhaseTransactionList phases = {{}, sorobanTxs};
                    auto [_, txSet] =
                        makeTxSetFromTransactions(phases, *app, 0, 0);

                    validateShape(*txSet, 1, 1, 10);
                    validateBaseFee(*txSet,
                                    100 + STAGE_COUNT * CLUSTER_COUNT - 10);
                });
        }
        {
            INFO("write bytes limit reached");
            std::vector<TransactionFrameBaseConstPtr> sorobanTxs;
            for (int i = 0; i < STAGE_COUNT * CLUSTER_COUNT; ++i)
            {
                sorobanTxs.push_back(createTx(1'000'000, {4 * i, 4 * i + 1},
                                              {4 * i + 2, 4 * i + 3},
                                              /* inclusionFee */ 100 + i,
                                              /* readBytes */ 100,
                                              /* writeBytes */ 10'000));
            }
            PerPhaseTransactionList phases = {{}, sorobanTxs};
            auto [_, txSet] = makeTxSetFromTransactions(phases, *app, 0, 0);

            validateShape(*txSet, 1, 1, 10);
            validateBaseFee(*txSet, 100 + STAGE_COUNT * CLUSTER_COUNT - 10);
        }
        {
            INFO("write entries limit reached");
            runWithConfigUpgrade(
                [](SorobanNetworkConfig& sorobanCfg) {
                    sorobanCfg.mTxMaxWriteLedgerEntries = 2 * 10 + 1;
                    sorobanCfg.mLedgerMaxWriteLedgerEntries = 2 * 10 + 1;
                },
                [&]() {
                    std::vector<TransactionFrameBaseConstPtr> sorobanTxs;
                    for (int i = 0; i < STAGE_COUNT * CLUSTER_COUNT; ++i)
                    {
                        sorobanTxs.push_back(
                            createTx(1'000'000, {4 * i, 4 * i + 1},
                                     {4 * i + 2, 4 * i + 3},
                                     /* inclusionFee */ 100 + i));
                    }
                    PerPhaseTransactionList phases = {{}, sorobanTxs};
                    auto [_, txSet] =
                        makeTxSetFromTransactions(phases, *app, 0, 0);

                    validateShape(*txSet, 1, 1, 10);
                    validateBaseFee(*txSet,
                                    100 + STAGE_COUNT * CLUSTER_COUNT - 10);
                });
        }
        {
            INFO("tx size limit reached");
            std::vector<TransactionFrameBaseConstPtr> sorobanTxs;
            for (int i = 0; i < STAGE_COUNT * CLUSTER_COUNT; ++i)
            {
                sorobanTxs.push_back(createTx(1'000'000, {4 * i, 4 * i + 1},
                                              {4 * i + 2, 4 * i + 3},
                                              /* inclusionFee */ 100 + i));
            }
            runWithConfigUpgrade(
                [&](SorobanNetworkConfig& sorobanCfg) {
                    sorobanCfg.mTxMaxSizeBytes =
                        MinimumSorobanNetworkConfig::TX_MAX_SIZE_BYTES;
                    sorobanCfg.mLedgerMaxTransactionsSizeBytes =
                        xdr::xdr_size(sorobanTxs[0]->getEnvelope()) * 11 - 1;
                    REQUIRE(sorobanCfg.mTxMaxSizeBytes <=
                            sorobanCfg.mLedgerMaxTransactionsSizeBytes);
                },
                [&]() {
                    PerPhaseTransactionList phases = {{}, sorobanTxs};
                    auto [_, txSet] =
                        makeTxSetFromTransactions(phases, *app, 0, 0);

                    validateShape(*txSet, 1, 1, 10);
                    validateBaseFee(*txSet,
                                    100 + STAGE_COUNT * CLUSTER_COUNT - 10);
                });
        }
        {
            INFO("tx count limit reached");
            runWithConfigUpgrade(
                [](SorobanNetworkConfig& sorobanCfg) {
                    sorobanCfg.mLedgerMaxTxCount = 5;
                },
                [&]() {
                    std::vector<TransactionFrameBaseConstPtr> sorobanTxs;
                    for (int i = 0; i < STAGE_COUNT * CLUSTER_COUNT; ++i)
                    {
                        sorobanTxs.push_back(
                            createTx(1'000'000, {4 * i, 4 * i + 1},
                                     {4 * i + 2, 4 * i + 3},
                                     /* inclusionFee */ 100 + i));
                    }

                    PerPhaseTransactionList phases = {{}, sorobanTxs};
                    auto [_, txSet] =
                        makeTxSetFromTransactions(phases, *app, 0, 0);

                    validateShape(*txSet, 1, 1, 5);
                    validateBaseFee(*txSet,
                                    100 + STAGE_COUNT * CLUSTER_COUNT - 5);
                });
        }
    }

    {
        INFO("with conflicts");
        {
            INFO("all RW conflicting");
            std::vector<TransactionFrameBaseConstPtr> sorobanTxs;
            for (int i = 0; i < CLUSTER_COUNT * STAGE_COUNT; ++i)
            {
                sorobanTxs.push_back(createTx(100'000'000,
                                              {4 * i + 1, 4 * i + 2},
                                              {4 * i + 3, 0, 4 * i + 4},
                                              /* inclusionFee */ 100 + i));
            }
            PerPhaseTransactionList phases = {{}, sorobanTxs};
            auto [_, txSet] = makeTxSetFromTransactions(phases, *app, 0, 0);
            if (variableStageCount)
            {
                validateShape(*txSet, 1, 1, STAGE_COUNT);
            }
            else
            {
                validateShape(*txSet, STAGE_COUNT, 1, 1);
            }

            validateBaseFee(*txSet,
                            100 + CLUSTER_COUNT * STAGE_COUNT - STAGE_COUNT);
        }
        {
            INFO("chain of conflicts");
            std::vector<TransactionFrameBaseConstPtr> sorobanTxs;
            for (int i = 0; i < CLUSTER_COUNT * STAGE_COUNT; ++i)
            {
                sorobanTxs.push_back(createTx(100'000'000, {i}, {i + 1},
                                              /* inclusionFee */ 100 + i));
            }
            PerPhaseTransactionList phases = {{}, sorobanTxs};
            auto [_, txSet] = makeTxSetFromTransactions(phases, *app, 0, 0);
            // It's easy to 'break' the chain by allocating transactions to
            // different stages (technically, 2 stages would be sufficient,
            // but the nomination algorithm isn't clever enough to figure that
            // out).
            validateShape(*txSet, STAGE_COUNT, CLUSTER_COUNT, 1);
            validateBaseFee(*txSet, 100);
        }
        {
            INFO("conflict clusters not exceeding max ledger insns");
            std::vector<TransactionFrameBaseConstPtr> sorobanTxs;
            for (int i = 0; i < CLUSTER_COUNT; ++i)
            {
                for (int j = 0; j < STAGE_COUNT; ++j)
                {
                    sorobanTxs.push_back(
                        createTx(100'000'000, {i * STAGE_COUNT + j + 1000},
                                 {i, i * STAGE_COUNT + j + 10000},
                                 /* inclusionFee */ 100 + i));
                }
            }
            PerPhaseTransactionList phases = {{}, sorobanTxs};
            auto [_, txSet] = makeTxSetFromTransactions(phases, *app, 0, 0);
            if (variableStageCount)
            {
                // With variable stage count, we can fit all transactions into
                // a single stage.
                validateShape(*txSet, 1, CLUSTER_COUNT, STAGE_COUNT);
            }
            else
            {
                // With fixed stage count, we can fit all transactions into
                // separate clusters in each stage.
                validateShape(*txSet, STAGE_COUNT, CLUSTER_COUNT, 1);
            }
            validateBaseFee(*txSet, 100);
        }
        {
            INFO("small conflict clusters with excluded txs");
            std::vector<TransactionFrameBaseConstPtr> sorobanTxs;
            for (int i = 0; i < CLUSTER_COUNT; ++i)
            {
                for (int j = 0; j < STAGE_COUNT + 1; ++j)
                {
                    sorobanTxs.push_back(createTx(
                        100'000'000, {}, {i},
                        /* inclusionFee */ 100 + i * (STAGE_COUNT + 1) + j));
                }
            }
            PerPhaseTransactionList phases = {{}, sorobanTxs};
            auto [_, txSet] = makeTxSetFromTransactions(phases, *app, 0, 0);
            if (variableStageCount)
            {
                // With variable stage count, we can fit all transactions into
                // a single stage and lower fee txs in every cluster will be
                // excluded.
                validateShape(*txSet, 1, CLUSTER_COUNT, STAGE_COUNT);
            }
            else
            {
                // Conflicting transactions can be distributed into separate
                // stages and lower fee txs in every cluster will be excluded.
                validateShape(*txSet, STAGE_COUNT, CLUSTER_COUNT, 1);
            }

            // 1 cluster worth of txs will be excluded, however, the lowest
            // fee transaction in the set has a fee of 101 (generated in
            // cluster 0, stage 1).
            validateBaseFee(*txSet, 101);
        }
        {
            INFO("one sparse conflict cluster");
            std::vector<TransactionFrameBaseConstPtr> sorobanTxs;
            // A small dense cluster of RW conflicts on entry 1000 with high
            // fee to ensure these are included.
            for (int i = 0; i < STAGE_COUNT; ++i)
            {
                sorobanTxs.push_back(
                    createTx(100'000'000, {}, {i, 1000},
                             /* inclusionFee */ 1'000'000 - i));
            }
            // Create a (CLUSTER_COUNT - 1) txs with RO-RW conflict with one
            // of the transactions in the small dense cluster, so that it's
            // possible to fit them all into a stage with one of the dense
            // cluster transactions.
            for (int i = 0; i < STAGE_COUNT; ++i)
            {
                for (int j = 0; j < CLUSTER_COUNT - 1; ++j)
                {
                    sorobanTxs.push_back(createTx(
                        100'000'000, {i}, {i * CLUSTER_COUNT + j + 10'000},
                        /* inclusionFee */ 1000 + i * CLUSTER_COUNT + j));
                }
            }
            // This is the assumption under which this test operates.
            releaseAssert(CLUSTER_COUNT > STAGE_COUNT);
            // Add some cheap transactions that conflict with the dense
            // cluster that shouldn't be included.
            for (int i = 0; i < CLUSTER_COUNT - STAGE_COUNT; ++i)
            {
                sorobanTxs.push_back(createTx(100'000'000, {i % STAGE_COUNT},
                                              {i + 100'000},
                                              /* inclusionFee */ 100 + i));
            }
            PerPhaseTransactionList phases = {{}, sorobanTxs};
            auto [_, txSet] = makeTxSetFromTransactions(phases, *app, 0, 0);

            if (variableStageCount)
            {
                // We can actually fit all transactions into two stages.
                validateShape(*txSet, 2, CLUSTER_COUNT, STAGE_COUNT / 2);
            }
            else
            {
                validateShape(*txSet, STAGE_COUNT, CLUSTER_COUNT, 1);
            }

            // All transactions can be distributed across stages, but 4
            // transactions simply don't fit into instruction limits (hence
            // 1000 base fee).
            validateBaseFee(*txSet, 1000);
        }
        {
            INFO("many clusters with small transactions");
            std::vector<TransactionFrameBaseConstPtr> sorobanTxs;
            for (int i = 0; i < CLUSTER_COUNT; ++i)
            {
                for (int j = 0; j < 10 * STAGE_COUNT; ++j)
                {
                    sorobanTxs.push_back(createTx(
                        10'000'000, {1000 + i * 10 + j},
                        {i, 10'000 + i * 10 + j},
                        /* inclusionFee */ 100 + i * (STAGE_COUNT + 1) + j));
                }
            }
            PerPhaseTransactionList phases = {{}, sorobanTxs};
            auto [_, txSet] = makeTxSetFromTransactions(phases, *app, 0, 0);

            validateShape(*txSet, STAGE_COUNT, CLUSTER_COUNT, 10);
            validateBaseFee(*txSet, 100);
        }
        {
            INFO("all RO conflict with one RW");
            std::vector<TransactionFrameBaseConstPtr> sorobanTxs;
            sorobanTxs.push_back(createTx(100'000'000, {1, 2}, {0, 3, 4},
                                          /* inclusionFee */ 1'000'000));
            for (int i = 1; i < CLUSTER_COUNT * STAGE_COUNT * 5; ++i)
            {
                sorobanTxs.push_back(createTx(20'000'000,
                                              {0, 4 * i + 1, 4 * i + 2},
                                              {4 * i + 3, 4 * i + 4},
                                              /* inclusionFee */ 100 + i));
            }

            PerPhaseTransactionList phases = {{}, sorobanTxs};
            auto [_, txSet] = makeTxSetFromTransactions(phases, *app, 0, 0);
            auto const& phase =
                txSet->getPhase(TxSetPhase::SOROBAN).getParallelStages();

            bool wasSingleThreadStage = false;

            for (auto const& stage : phase)
            {
                if (stage.size() == 1)
                {
                    REQUIRE(!wasSingleThreadStage);
                    wasSingleThreadStage = true;
                    REQUIRE(stage[0].size() == 1);
                    REQUIRE(stage[0][0]->getEnvelope() ==
                            sorobanTxs[0]->getEnvelope());
                    continue;
                }
                REQUIRE(stage.size() == CLUSTER_COUNT);
                for (auto const& thread : stage)
                {
                    REQUIRE(thread.size() == 5);
                }
            }
            // We can't include any of the small txs into stage 0, as it's
            // occupied by high fee tx that writes entry 0.
            validateBaseFee(*txSet, 100 + CLUSTER_COUNT * 5);
        }
    }

    {
        INFO("smoke test");
        auto runTest = [&]() {
            stellar::uniform_int_distribution<> maxInsnsDistr(20'000'000,
                                                              100'000'000);
            stellar::uniform_int_distribution<> keyRangeDistr(50, 1000);
            stellar::uniform_int_distribution<> insnsDistr(
                1'000'000, maxInsnsDistr(Catch::rng()));
            stellar::uniform_int_distribution<> keyCountDistr(1, 10);
            stellar::uniform_int_distribution<> keyDistr(
                1, keyRangeDistr(Catch::rng()));
            stellar::uniform_int_distribution<> feeDistr(100, 100'000);
            stellar::uniform_int_distribution<> readBytesDistr(100, 10'000);
            stellar::uniform_int_distribution<> writeBytesDistr(10, 1000);
            std::vector<TransactionFrameBaseConstPtr> sorobanTxs;
            accountId = 1;
            for (int iter = 0; iter < 500; ++iter)
            {
                int roKeyCount = keyCountDistr(Catch::rng());
                int rwKeyCount = keyCountDistr(Catch::rng());
                std::unordered_set<int> usedKeys;
                std::vector<int> roKeys;
                std::vector<int> rwKeys;
                for (int i = 0; i < roKeyCount + rwKeyCount; ++i)
                {
                    int key = keyDistr(Catch::rng());
                    while (usedKeys.find(key) != usedKeys.end())
                    {
                        key = keyDistr(Catch::rng());
                    }
                    if (i < roKeyCount)
                    {
                        roKeys.push_back(key);
                    }
                    else
                    {
                        rwKeys.push_back(key);
                    }
                    usedKeys.insert(key);
                }
                sorobanTxs.push_back(createTx(insnsDistr(Catch::rng()), roKeys,
                                              rwKeys, feeDistr(Catch::rng()),
                                              readBytesDistr(Catch::rng()),
                                              writeBytesDistr(Catch::rng())));
            }
            PerPhaseTransactionList phases = {{}, sorobanTxs};
            // NB: `makeTxSetFromTransactions` does an XDR roundtrip and
            // validation, so just calling it does a good amount of smoke
            // testing.
            auto [_, txSet] = makeTxSetFromTransactions(phases, *app, 0, 0);
            auto const& phase =
                txSet->getPhase(TxSetPhase::SOROBAN).getParallelStages();
            if (variableStageCount)
            {
                // The setup involves a large number of conflicts, so we
                // expect at least more than 1 stage.
                REQUIRE(phase.size() > 1);
            }
            else
            {
                // With the fixed number of stages all the stages should be
                // populated.
                REQUIRE(phase.size() == STAGE_COUNT);
            }
        };
        for (int iter = 0; iter < 10; ++iter)
        {
            runTest();
        }
    }
}

TEST_CASE("parallel tx set building", "[txset][soroban]")
{
    SECTION("variable stage count")
    {
        runParallelTxSetBuildingTest(true);
    }
    SECTION("fixed stage count")
    {
        runParallelTxSetBuildingTest(false);
    }
}

TEST_CASE("parallel tx set building benchmark",
          "[txset][soroban][bench][!hide]")
{
    int const MIN_STAGE_COUNT = 1;
    int const MAX_STAGE_COUNT = 4;
    int const CLUSTER_COUNT = MAX_LEDGER_DEPENDENT_TX_CLUSTERS;
    int const MEAN_INCLUDED_TX_COUNT = 5000;
    int const TX_COUNT_MEMPOOL_MULTIPLIER = 2;

    int const MEAN_INSTRUCTIONS_PER_TX = 10'000'000;
    int const MAX_INSTRUCTIONS_PER_TX = 100'000'000;
    int const MEAN_READS_PER_TX = 30;
    int const MAX_READS_PER_TX = 60;
    int const MEAN_WRITES_PER_TX = 10;
    int const MAX_WRITES_PER_TX = 30;
    int const MEAN_TX_SIZE = 5000;
    int const MAX_TX_SIZE = 5000;
    // The exact values for r/w bytes aren't meaningful for the performance,
    // just give them high enough value to create some spread.
    int const MEAN_READ_BYTES_PER_TX = 1000;
    int const MAX_READ_BYTES_PER_TX = 5000;
    int const MEAN_WRITE_BYTES_PER_TX = 500;
    int const MAX_WRITE_BYTES_PER_TX = 2000;

    auto cfg = getTestConfig();
    cfg.SOROBAN_PHASE_MIN_STAGE_COUNT = MIN_STAGE_COUNT;
    cfg.SOROBAN_PHASE_MAX_STAGE_COUNT = MAX_STAGE_COUNT;

    // Only per-ledger limits matter for tx set building, as we don't perform
    // any validation.
    SorobanNetworkConfig sorobanCfg;
    sorobanCfg.mLedgerMaxTransactionsSizeBytes =
        MEAN_INCLUDED_TX_COUNT * MEAN_TX_SIZE;
    sorobanCfg.mLedgerMaxInstructions =
        static_cast<int64_t>(MEAN_INSTRUCTIONS_PER_TX) *
        MEAN_INCLUDED_TX_COUNT / CLUSTER_COUNT;
    sorobanCfg.mLedgerMaxDiskReadEntries =
        MEAN_INCLUDED_TX_COUNT * (MEAN_READS_PER_TX + MEAN_WRITES_PER_TX);
    sorobanCfg.mLedgerMaxDiskReadBytes =
        MEAN_INCLUDED_TX_COUNT * MEAN_READ_BYTES_PER_TX;
    sorobanCfg.mLedgerMaxWriteLedgerEntries =
        MEAN_INCLUDED_TX_COUNT * MEAN_WRITES_PER_TX;
    sorobanCfg.mLedgerMaxWriteBytes =
        MEAN_INCLUDED_TX_COUNT * MEAN_WRITE_BYTES_PER_TX;
    // This doesn't need to be a real limit for this test.
    sorobanCfg.mLedgerMaxTxCount = MEAN_INCLUDED_TX_COUNT * 10;
    sorobanCfg.mLedgerMaxDependentTxClusters = CLUSTER_COUNT;

    auto limits = sorobanCfg.maxLedgerResources();
    limits.setVal(Resource::Type::INSTRUCTIONS,
                  std::numeric_limits<int64_t>::max());

    auto surgePricingLaneConfig =
        std::make_shared<SorobanGenericLaneConfig>(limits);

    SCAddress contract(SC_ADDRESS_TYPE_CONTRACT);
    auto generateKey = [&contract](int i) {
        return stellar::contractDataKey(
            contract, txtest::makeU32(i),
            i % 2 == 0 ? ContractDataDurability::PERSISTENT
                       : ContractDataDurability::TEMPORARY);
    };

    auto createTx = [&](int instructions, std::vector<int> const& roKeys,
                        std::vector<int> const& rwKeys, int64_t inclusionFee,
                        int readBytes, int writeBytes, int txSize) {
        TransactionEnvelope txEnvelope(EnvelopeType::ENVELOPE_TYPE_TX);
        txEnvelope.v1().tx.ext.v(1);
        txEnvelope.v1().tx.fee = inclusionFee;
        auto& resources = txEnvelope.v1().tx.ext.sorobanData().resources;

        resources.instructions = instructions;
        resources.diskReadBytes = readBytes;
        resources.writeBytes = writeBytes;
        for (auto roKeyId : roKeys)
        {
            resources.footprint.readOnly.push_back(generateKey(roKeyId));
        }
        for (auto rwKeyId : rwKeys)
        {
            resources.footprint.readWrite.push_back(generateKey(rwKeyId));
        }
        auto& op = txEnvelope.v1().tx.operations.emplace_back();
        op.body.type(OperationType::INVOKE_HOST_FUNCTION);

        op.body.invokeHostFunctionOp().hostFunction.type(
            HostFunctionType::HOST_FUNCTION_TYPE_UPLOAD_CONTRACT_WASM);
        auto currSize = xdr::xdr_size(txEnvelope);
        if (currSize < txSize)
        {
            op.body.invokeHostFunctionOp().hostFunction.wasm().resize(txSize -
                                                                      currSize);
        }

        return TransactionFrameBase::makeTransactionFromWire(Hash{},
                                                             txEnvelope);
    };
    std::normal_distribution<> insnsDistr(MEAN_INSTRUCTIONS_PER_TX,
                                          0.5 * MEAN_INSTRUCTIONS_PER_TX);
    std::normal_distribution<> txSizeDistr(MEAN_TX_SIZE, 0.1 * MEAN_TX_SIZE);
    std::normal_distribution<> readBytesDistr(MEAN_READ_BYTES_PER_TX,
                                              0.2 * MEAN_READ_BYTES_PER_TX);
    std::normal_distribution<> writeBytesDistr(MEAN_WRITE_BYTES_PER_TX,
                                               0.1 * MEAN_WRITE_BYTES_PER_TX);
    std::normal_distribution<> readCountDistr(MEAN_READS_PER_TX,
                                              0.15 * MEAN_READS_PER_TX);
    std::normal_distribution<> writeCountDistr(MEAN_WRITES_PER_TX,
                                               0.2 * MEAN_WRITES_PER_TX);
    stellar::uniform_int_distribution<> feeDistr(100, 100'000);

    auto genValue = [](auto& distribution, int maxValue) {
        return std::min(
            maxValue, std::max(1, static_cast<int>(
                                      std::round(distribution(Catch::rng())))));
    };

    auto generateConflictingTxs = [&](double mean_conflicts_per_tx,
                                      double mean_ro_txs_per_conflict,
                                      double mean_rw_txs_per_conflict) {
        int maxKey = 0;
        int txCount = MEAN_INCLUDED_TX_COUNT * TX_COUNT_MEMPOOL_MULTIPLIER;
        std::vector<std::pair<std::vector<int>, std::vector<int>>> txKeys(
            txCount);
        // First, generate non-conflicting keys that follow the distributions
        // for the respective resources.
        for (int i = 0; i < txCount; ++i)
        {
            int readCount = genValue(readCountDistr, MAX_READS_PER_TX);
            for (int j = 0; j < readCount; ++j)
            {
                txKeys[i].first.push_back(maxKey++);
            }
            int writeCount = genValue(writeCountDistr, MAX_WRITES_PER_TX);
            for (int j = 0; j < writeCount; ++j)
            {
                txKeys[i].second.push_back(maxKey++);
            }
        }

        // Then, generate the conflict 'clusters', such that the same key is
        // present in the RO footprint of `mean_ro_txs_per_conflict` on average
        // and in the RW footprint of `mean_rw_txs_per_conflict` on average.
        // The total number of the conflicts generated is defined such that
        // on average a transaction participates in `mean_conflicts_per_tx`
        // clusters.
        int currConflictKey = 0;
        std::poisson_distribution<> roTxCountDistr(mean_ro_txs_per_conflict);
        std::poisson_distribution<> rwTxCountDistr(mean_rw_txs_per_conflict);
        int conflictsLeft = txCount * mean_conflicts_per_tx;
        // Try to exhaust all the conflicts, but make sure we don't get stuck
        // in an infinite loop.
        for (int iter = 0; iter < 1000 && conflictsLeft > 0; ++iter)
        {
            int roTxCount = roTxCountDistr(Catch::rng());
            int rwTxCount = std::max(1, rwTxCountDistr(Catch::rng()));
            if (roTxCount + rwTxCount < 2)
            {
                continue;
            }
            stellar::shuffle(txKeys.begin(), txKeys.end(), Catch::rng());
            int conflictsLeftToAdd = roTxCount + rwTxCount;
            for (int i = 0; i < txCount && conflictsLeftToAdd > 0; ++i)
            {
                auto& keys = conflictsLeftToAdd > roTxCount ? txKeys[i].second
                                                            : txKeys[i].first;

                for (int j = 0; j < keys.size(); ++j)
                {
                    if (keys[j] < maxKey)
                    {
                        keys[j] = maxKey + currConflictKey;
                        --conflictsLeftToAdd;
                        --conflictsLeft;
                        break;
                    }
                }
            }
            ++currConflictKey;
        }

        TxFrameList txs;
        for (int i = 0; i < txCount; ++i)
        {
            int insns = genValue(insnsDistr, MAX_INSTRUCTIONS_PER_TX);
            int txSize = genValue(txSizeDistr, MAX_TX_SIZE);
            int readBytes = genValue(readBytesDistr, MAX_READ_BYTES_PER_TX);
            int writeBytes = genValue(writeBytesDistr, MAX_WRITE_BYTES_PER_TX);
            txs.push_back(createTx(insns, txKeys[i].first, txKeys[i].second,
                                   feeDistr(Catch::rng()), readBytes,
                                   writeBytes, txSize));
        }
        return txs;
    };

    uint32_t ledgerVersion = cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION;
    auto runBenchmark = [&](double mean_conflicts_per_tx,
                            double mean_ro_txs_per_conflict,
                            double mean_rw_txs_per_conflict) {
        const int iterCount = 5;
        std::vector<TxFrameList> allTxs;
        for (int i = 0; i < iterCount; ++i)
        {
            allTxs.push_back(generateConflictingTxs(mean_conflicts_per_tx,
                                                    mean_ro_txs_per_conflict,
                                                    mean_rw_txs_per_conflict));
        }

        int64_t totalDuration = 0;
        int txsIncluded = 0;
        int64_t insnsIncluded = 0;
        for (int iter = 0; iter < 5; ++iter)
        {
            std::vector<bool> hadTxNotFittingLane;
            auto start = std::chrono::steady_clock::now();
            auto stages = buildSurgePricedParallelSorobanPhase(
                allTxs[iter], cfg, sorobanCfg, surgePricingLaneConfig,
                hadTxNotFittingLane, ledgerVersion);
            auto end = std::chrono::steady_clock::now();
            totalDuration +=
                std::chrono::duration_cast<std::chrono::nanoseconds>(end -
                                                                     start)
                    .count();
            int currTxsIncluded = 0;
            for (auto const& stage : stages)
            {
                for (auto const& cluster : stage)
                {
                    txsIncluded += cluster.size();
                    currTxsIncluded += cluster.size();
                    for (auto const& tx : cluster)
                    {
                        insnsIncluded += tx->sorobanResources().instructions;
                    }
                }
            }
            std::cout << "input tx count: " << allTxs[iter].size()
                      << ", included tx count: " << currTxsIncluded
                      << ", stages created: " << stages.size() << std::endl;
        }
        std::cout << "Mean conflicts per tx: " << mean_conflicts_per_tx
                  << ", mean RO txs per conflict: " << mean_ro_txs_per_conflict
                  << ", mean RW txs per conflict: " << mean_rw_txs_per_conflict
                  << ", mean txs included: " << txsIncluded / iterCount
                  << ", insns included %: "
                  << static_cast<double>(insnsIncluded) / iterCount * 100.0 /
                         (sorobanCfg.ledgerMaxInstructions() *
                          sorobanCfg.ledgerMaxDependentTxClusters())
                  << ", mean duration: " << 1e-6 * totalDuration / iterCount
                  << " ms" << std::endl;
    };
    runBenchmark(0, 0, 0);
    runBenchmark(1, 1000, 1);
    runBenchmark(10, 40, 1);
    runBenchmark(20, 40, 1);
    runBenchmark(10, 10, 10);
    runBenchmark(50, 50, 5);
}

TEST_CASE("simple parallel tx stage building", "[txset]")
{
    using Indices = std::vector<std::vector<std::vector<size_t>>>;

    SECTION("no conflicts - all parallel")
    {
        // 6 transactions, no conflicts
        std::vector<BitSet> conflicts(6);

        auto stages = testBuildSimpleParallelStagesFromIndices(conflicts, 3, 2);

        // Should create 2 stages with 3 txs each
        REQUIRE(stages.size() == 2);
        REQUIRE(stages[0].size() == 3);
        REQUIRE(stages[1].size() == 3);

        // Each cluster should have 1 tx
        for (auto& stage : stages)
        {
            for (auto& cluster : stage)
            {
                REQUIRE(cluster.size() == 1);
            }
        }
    }

    SECTION("simple chain conflict")
    {
        // 4 transactions: 0->1, 1->2, 2->3 (chain)
        std::vector<BitSet> conflicts(4);
        conflicts[0].set(1);
        conflicts[1].set(0);
        conflicts[1].set(2);
        conflicts[2].set(1);
        conflicts[2].set(3);
        conflicts[3].set(2);

        auto stages = testBuildSimpleParallelStagesFromIndices(conflicts, 2, 1);

        // Should create 1 stage with 1 cluster containing all 4
        REQUIRE(stages.size() == 1);
        REQUIRE(stages[0].size() == 1);
        REQUIRE(stages[0][0] == std::vector<size_t>{0, 1, 2, 3});
    }

    SECTION("two independent chains")
    {
        // 6 transactions: two chains 0->1->2 and 3->4->5
        std::vector<BitSet> conflicts(6);
        // First chain
        conflicts[0].set(1);
        conflicts[1].set(0);
        conflicts[1].set(2);
        conflicts[2].set(1);
        // Second chain
        conflicts[3].set(4);
        conflicts[4].set(3);
        conflicts[4].set(5);
        conflicts[5].set(4);

        auto stages = testBuildSimpleParallelStagesFromIndices(conflicts, 2, 1);

        // Should create 1 stage with 2 clusters
        REQUIRE(stages.size() == 1);
        REQUIRE(stages[0].size() == 2);

        // Check clusters contain the right transactions
        std::set<std::vector<size_t>> clusters;
        for (auto& cluster : stages[0])
        {
            clusters.insert(cluster);
        }
        REQUIRE(clusters.count({0, 1, 2}) == 1);
        REQUIRE(clusters.count({3, 4, 5}) == 1);
    }

    SECTION("conflict forces merge")
    {
        // 5 transactions: 0 independent, 1->2 chain, 3->4 chain, then 5
        // conflicts with both
        std::vector<BitSet> conflicts(6);
        // First chain
        conflicts[1].set(2);
        conflicts[2].set(1);
        // Second chain
        conflicts[3].set(4);
        conflicts[4].set(3);
        // Transaction 5 conflicts with both chains
        conflicts[5].set(2);
        conflicts[5].set(4);
        conflicts[2].set(5);
        conflicts[4].set(5);

        auto stages = testBuildSimpleParallelStagesFromIndices(conflicts, 3, 1);

        // All 6 txs should fit in stage 0
        REQUIRE(stages.size() >= 1);

        // Stage 0 should have tx 0 in its own cluster, and txs 1-5 merged
        bool found0 = false;
        bool found1to5 = false;

        for (auto& cluster : stages[0])
        {
            std::sort(cluster.begin(), cluster.end());
            if (cluster == std::vector<size_t>{0})
            {
                found0 = true;
            }
            else if (cluster == std::vector<size_t>{1, 2, 3, 4, 5})
            {
                found1to5 = true;
            }
        }

        REQUIRE(found0);
        REQUIRE(found1to5);
    }

    SECTION("buffer spillover")
    {
        // 8 transactions with conflicts that force spillover
        std::vector<BitSet> conflicts(8);

        // Create 4 conflict pairs that can't all fit in 2 clusters
        for (int i = 0; i < 8; i += 2)
        {
            conflicts[i].set(i + 1);
            conflicts[i + 1].set(i);
        }

        auto stages = testBuildSimpleParallelStagesFromIndices(conflicts, 2, 1);

        // Should create multiple stages since we can only fit 2 clusters per
        // stage
        REQUIRE(stages.size() >= 2);

        // Count total transactions placed
        size_t totalTxs = 0;
        for (auto& stage : stages)
        {
            for (auto& cluster : stage)
            {
                totalTxs += cluster.size();
            }
        }
        REQUIRE(totalTxs == 8);
    }

    SECTION("complex merge scenario")
    {
        // Start with 3 independent clusters, then add txs that merge them
        std::vector<BitSet> conflicts(7);

        // Cluster 1: 0->1
        conflicts[0].set(1);
        conflicts[1].set(0);

        // Cluster 2: 2->3
        conflicts[2].set(3);
        conflicts[3].set(2);

        // Cluster 3: just 4

        // Tx 5 merges clusters 1 and 2
        conflicts[5].set(1);
        conflicts[5].set(3);
        conflicts[1].set(5);
        conflicts[3].set(5);

        // Tx 6 merges all three
        conflicts[6].set(5);
        conflicts[6].set(4);
        conflicts[5].set(6);
        conflicts[4].set(6);

        auto stages = testBuildSimpleParallelStagesFromIndices(conflicts, 3, 1);

        REQUIRE(stages.size() == 1);
        REQUIRE(stages[0].size() == 1);
        // All transactions should be in one cluster
        REQUIRE(stages[0][0].size() == 7);

        // Check that all transactions are present
        std::vector<size_t> sorted = stages[0][0];
        std::sort(sorted.begin(), sorted.end());
        REQUIRE(sorted == std::vector<size_t>{0, 1, 2, 3, 4, 5, 6});

        // Check partial order is maintained
        auto& cluster = stages[0][0];
        auto indexOf = [&cluster](size_t tx) -> size_t {
            auto it = std::find(cluster.begin(), cluster.end(), tx);
            REQUIRE(it != cluster.end());
            return std::distance(cluster.begin(), it);
        };

        // Tx 0 conflicts with 1, so 0 must come before 1
        REQUIRE(indexOf(0) < indexOf(1));

        // Tx 2 conflicts with 3, so 2 must come before 3
        REQUIRE(indexOf(2) < indexOf(3));

        // Tx 5 conflicts with 1 and 3, so both must come before 5
        REQUIRE(indexOf(1) < indexOf(5));
        REQUIRE(indexOf(3) < indexOf(5));

        // Tx 6 conflicts with 4 and 5, so both must come before 6
        REQUIRE(indexOf(4) < indexOf(6));
        REQUIRE(indexOf(5) < indexOf(6));
    }
}
} // namespace
} // namespace stellar
