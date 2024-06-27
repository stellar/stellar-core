// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SHA.h"
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
#include "transactions/TransactionUtils.h"
#include "transactions/test/SorobanTxTestUtils.h"
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
        REQUIRE(txSet->prepareForApply(*app) == nullptr);
    }
    SECTION("one phase")
    {
        auto txSet = TxSetXDRFrame::makeFromWire(xdrTxSet);
        xdrTxSet.v1TxSet().phases.emplace_back();
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
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
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
            parallelPhase({{2}, {{0}}, {3, 5}}, false), false,
            "parallel Soroban - one of the stages has empty cluster");
        scenarios[1].emplace_back(
            parallelPhase({{2}, {{0}}}, false), false,
            "parallel Soroban - one of the stages has empty cluster");
        scenarios[1].emplace_back(parallelPhase({{}, {}, {}}, false), false,
                                  "parallel Soroban - multiple empty stages");
        scenarios[1].emplace_back(
            parallelPhase({{{}, {0}}, {{0}}, {}}, false), false,
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
#endif
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
                    REQUIRE(txSet->prepareForApply(*app) != nullptr);
                }
                else
                {
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
        std::vector<TransactionFrameBaseConstPtr> txs;
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
        ApplicableTxSetFrameConstPtr applicableFrame =
            txSetFrame->prepareForApply(*app);
        REQUIRE(applicableFrame->checkValid(*app, 0, 0));
        GeneralizedTransactionSet newXdr;
        applicableFrame->toWireTxSetFrame()->toXDR(newXdr);
        REQUIRE(newXdr == txSetXdr);
    };

    SECTION("empty set")
    {
        auto [_, applicableTxSetFrame] =
            testtxset::makeNonValidatedGeneralizedTxSet(
                {{}, {}}, *app,
                app->getLedgerManager().getLastClosedLedgerHeader().hash);

        GeneralizedTransactionSet txSetXdr;
        applicableTxSetFrame->toWireTxSetFrame()->toXDR(txSetXdr);
        REQUIRE(txSetXdr.v1TxSet().phases[0].v0Components().empty());
        checkXdrRoundtrip(txSetXdr);
    }
    SECTION("one discounted component set")
    {
        auto [_, applicableTxSetFrame] =
            testtxset::makeNonValidatedGeneralizedTxSet(
                {{std::make_pair(1234LL, createTxs(5, 1234))}, {}}, *app,
                app->getLedgerManager().getLastClosedLedgerHeader().hash);

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
    SECTION("one non-discounted component set")
    {
        auto [_, applicableTxSetFrame] =
            testtxset::makeNonValidatedGeneralizedTxSet(
                {{std::make_pair(std::nullopt, createTxs(5, 4321))}, {}}, *app,
                app->getLedgerManager().getLastClosedLedgerHeader().hash);

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
    SECTION("multiple component sets")
    {
        auto [_, applicableTxSetFrame] =
            testtxset::makeNonValidatedGeneralizedTxSet(
                {{std::make_pair(12345LL, createTxs(3, 12345)),
                  std::make_pair(123LL, createTxs(1, 123)),
                  std::make_pair(1234LL, createTxs(2, 1234)),
                  std::make_pair(std::nullopt, createTxs(4, 4321))},
                 {}},
                *app, app->getLedgerManager().getLastClosedLedgerHeader().hash);

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
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    if (protocolVersionStartsFrom(static_cast<uint32_t>(protocolVersion),
                                  PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION))
    {
        SECTION("parallel Soroban phase")
        {
            modifySorobanNetworkConfig(
                *app, [](SorobanNetworkConfig& sorobanCfg) {
                    sorobanCfg.mLedgerMaxTxCount = 100;
                    sorobanCfg.mLedgerMaxDependentTxClusters = 5;
                });
            testtxset::PhaseComponents classicPhase = {
                {std::nullopt, createTxs(3, 1000, false)},
                {500, createTxs(3, 1200, false)},
                {1500, createTxs(3, 1500, false)}};
            SECTION("single stage, single cluster")
            {
                auto [_, applicableTxSetFrame] =
                    testtxset::makeNonValidatedGeneralizedTxSet(
                        classicPhase, 1234, {{createTxs(10, 1234, true)}}, *app,
                        app->getLedgerManager()
                            .getLastClosedLedgerHeader()
                            .hash);

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
                            .size() == 10);
                checkXdrRoundtrip(txSetXdr);
            }

            SECTION("single stage, multiple clusters")
            {
                auto [_, applicableTxSetFrame] =
                    testtxset::makeNonValidatedGeneralizedTxSet(
                        {}, 1234,
                        {{createTxs(10, 1234, true), createTxs(5, 2000, true),
                          createTxs(3, 1500, true)}},
                        *app,
                        app->getLedgerManager()
                            .getLastClosedLedgerHeader()
                            .hash);

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

            SECTION("multiple stages, multiple clusters")
            {
                auto [_, applicableTxSetFrame] =
                    testtxset::makeNonValidatedGeneralizedTxSet(
                        classicPhase, std::nullopt,
                        {{createTxs(1, 1234, true), createTxs(3, 2000, true),
                          createTxs(5, 1500, true), createTxs(2, 1000, true),
                          createTxs(7, 3000, true)},
                         {createTxs(3, 1234, true), createTxs(5, 1300, true)},
                         {createTxs(2, 4321, true)}},
                        *app,
                        app->getLedgerManager()
                            .getLastClosedLedgerHeader()
                            .hash);

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
        }
    }
#endif
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
        auto root = TestAccount::createRoot(*app);
        int accountId = 0;
        auto createTx = [&](bool isSoroban) {
            auto source =
                root.create("source" + std::to_string(accountId++),
                            app->getLedgerManager().getLastMinBalance(2));
            TransactionFrameBaseConstPtr tx = nullptr;
            if (isSoroban)
            {
                SorobanResources resources;
                resources.instructions = 800'000;
                resources.readBytes = 1000;
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
                auto txSet = testtxset::makeNonValidatedGeneralizedTxSet(
                                 phases, *app,
                                 app->getLedgerManager()
                                     .getLastClosedLedgerHeader()
                                     .hash)
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
        auto root = TestAccount::createRoot(*app);

        int accountId = 0;
        int footprintId = 0;
        auto ledgerKey = [&](int id) {
            LedgerKey key(LedgerEntryType::CONTRACT_DATA);
            key.contractData().key.type(SCValType::SCV_I32);
            key.contractData().key.i32() = id;
            return key;
        };

        auto createTx = [&](std::vector<int> addRoFootprint = {},
                            std::vector<int> addRwFootprint = {}) {
            auto source = root.create("source" + std::to_string(accountId++),
                                      1'000'000'000);
            Operation op;
            op.body.type(INVOKE_HOST_FUNCTION);
            op.body.invokeHostFunctionOp().hostFunction.type(
                HOST_FUNCTION_TYPE_UPLOAD_CONTRACT_WASM);
            SorobanResources resources;
            resources.instructions = 1'000'000;
            resources.readBytes = 5'000;
            resources.writeBytes = 2'000;
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
                    sorobanCfg.mLedgerMaxInstructions = 20 * 1'000'000;
                    sorobanCfg.mLedgerMaxReadBytes = 20 * 5000;
                    sorobanCfg.mLedgerMaxWriteBytes = 20 * 2000;
                    sorobanCfg.mLedgerMaxReadLedgerEntries = 20 * 10;
                    sorobanCfg.mLedgerMaxWriteLedgerEntries = 20 * 2;
                    sorobanCfg.mLedgerMaxTxCount = 20;
                    sorobanCfg.mLedgerMaxTransactionsSizeBytes = 20 * txSize;

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

            auto buildAndValidate = [&]() {
                std::vector<TransactionFrameBaseConstPtr> txs;
                for (int i = 0; i < 20; ++i)
                {
                    txs.push_back(createTx());
                }
                ApplicableTxSetFrameConstPtr txSet;
                if (protocolVersionIsBefore(
                        protocolVersion,
                        PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION))
                {
                    txSet = testtxset::makeNonValidatedGeneralizedTxSet(
                                {{}, {{1000, txs}}}, *app,
                                app->getLedgerManager()
                                    .getLastClosedLedgerHeader()
                                    .hash)
                                .second;
                }
                else
                {
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
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
                                {}, 1234, txsPerStage, *app,
                                app->getLedgerManager()
                                    .getLastClosedLedgerHeader()
                                    .hash)
                                .second;
#endif
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
                        sorobanCfg.mLedgerMaxReadBytes -= 1;
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
                        sorobanCfg.mLedgerMaxReadLedgerEntries -= 1;
                    });
                REQUIRE(!buildAndValidate());
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
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
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
#endif
        }
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
        if (protocolVersionStartsFrom(protocolVersion,
                                      PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION))
        {
            SECTION("data dependency validation")
            {

                auto buildAndValidate = [&](TxStageFrameList txsPerStage) {
                    auto txSet = testtxset::makeNonValidatedGeneralizedTxSet(
                                     {}, 1234, txsPerStage, *app,
                                     app->getLedgerManager()
                                         .getLastClosedLedgerHeader()
                                         .hash)
                                     .second;
                    return txSet->checkValid(*app, 0, 0);
                };

                // Relax the per-ledger limits to ensure we're not running
                // into any.
                modifySorobanNetworkConfig(
                    *app, [&](SorobanNetworkConfig& sorobanCfg) {
                        sorobanCfg.mLedgerMaxInstructions =
                            std::numeric_limits<int64_t>::max();
                        sorobanCfg.mLedgerMaxReadBytes =
                            std::numeric_limits<uint32_t>::max();
                        sorobanCfg.mLedgerMaxWriteBytes =
                            std::numeric_limits<uint32_t>::max();
                        sorobanCfg.mLedgerMaxReadLedgerEntries =
                            std::numeric_limits<uint32_t>::max();
                        sorobanCfg.mLedgerMaxWriteLedgerEntries =
                            std::numeric_limits<uint32_t>::max();
                        sorobanCfg.mLedgerMaxTxCount =
                            std::numeric_limits<uint32_t>::max();
                        sorobanCfg.mLedgerMaxTransactionsSizeBytes =
                            std::numeric_limits<uint32_t>::max();
                        sorobanCfg.mLedgerMaxDependentTxClusters =
                            std::numeric_limits<uint32_t>::max();
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
#endif
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
