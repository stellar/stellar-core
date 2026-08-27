// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketManager.h"
#include "crypto/SHA.h"
#include "crypto/SecretKey.h"
#include "ledger/ImmutableLedgerView.h"
#include "ledger/LedgerManager.h"
#include "main/Config.h"
#include "simulation/ApplyLoad.h"
#include "simulation/LoadGenerator.h"
#include "simulation/Topologies.h"
#include "test/Catch2.h"
#include "test/test.h"
#include "transactions/test/SorobanTxTestUtils.h"
#include "util/Math.h"
#include "util/MetricsRegistry.h"
#include "util/finally.h"
#include <cmath>
#include <fmt/format.h>
#include <random>

using namespace stellar;

TEST_CASE("loadgen in overlay-only mode", "[loadgen]")
{
    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    Simulation::pointer simulation = Topologies::pair(networkID, [&](int i) {
        auto cfg = getTestConfig(i);
        cfg.LOADGEN_INSTRUCTIONS_FOR_TESTING = {10'000'000, 50'000'000};
        cfg.LOADGEN_INSTRUCTIONS_DISTRIBUTION_FOR_TESTING = {5, 1};
        cfg.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = true;
        cfg.ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING = true;
        cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
            Config::CURRENT_LEDGER_PROTOCOL_VERSION;
        cfg.GENESIS_TEST_ACCOUNT_COUNT = 1000;
        return cfg;
    });

    simulation->startAllNodes();
    simulation->crankUntil(
        [&]() { return simulation->haveAllExternalized(3, 1); },
        10 * simulation->getExpectedLedgerCloseTime(), false);
    auto nodes = simulation->getNodes();
    auto& app = *nodes[0]; // pick a node to generate load

    uint32_t nAccounts = 1000;
    uint32_t nTxs = 100;

    // Upgrade the network config. Lift both ledger- and tx-level limits so
    // SOROBAN_INVOKE_APPLY_LOAD's oversized invoke txs pass validation in
    // overlay-only mode (where checkValid now runs end-to-end, including
    // checkSorobanResources).
    upgradeSorobanNetworkConfig(
        [&](SorobanNetworkConfig& cfg) {
            auto mx = std::numeric_limits<uint32_t>::max();
            cfg.mLedgerMaxTxCount = mx;
            cfg.mLedgerMaxInstructions = mx;
            cfg.mLedgerMaxTransactionsSizeBytes = mx;
            cfg.mLedgerMaxDiskReadEntries = mx;
            cfg.mLedgerMaxDiskReadBytes = mx;
            cfg.mLedgerMaxWriteLedgerEntries = mx;
            cfg.mLedgerMaxWriteBytes = mx;
            cfg.mTxMaxInstructions = mx;
            cfg.mTxMaxDiskReadEntries = mx;
            cfg.mTxMaxDiskReadBytes = mx;
            cfg.mTxMaxWriteLedgerEntries = mx;
            cfg.mTxMaxWriteBytes = mx;
            cfg.mTxMaxFootprintEntries = mx;
            cfg.mTxMaxSizeBytes = mx;
            cfg.mTxMaxContractEventsSizeBytes = mx;
        },
        simulation);

    for (auto& node : nodes)
    {
        node->setRunInOverlayOnlyMode(true);
    }

    auto prev = app.getMetrics()
                    .NewMeter({"loadgen", "run", "complete"}, "run")
                    .count();
    SECTION("pay")
    {
        // Simulate payment transactions
        app.getLoadGenerator().generateLoad(GeneratedLoadConfig::txLoad(
            LoadGenMode::PAY, nAccounts, nTxs, /* txRate */ 100));
    }
    simulation->crankUntil(
        [&]() {
            return app.getMetrics()
                       .NewMeter({"loadgen", "run", "complete"}, "run")
                       .count() == prev + 1;
        },
        500 * simulation->getExpectedLedgerCloseTime(), false);
}

TEST_CASE("multiple loadgen nodes in overlay-only mode", "[loadgen]")
{
    // Regression test: each node's completion check must only count its own
    // externalized transactions, even while other nodes concurrently generate
    // load from disjoint account ranges (offsets).
    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    Simulation::pointer simulation = Topologies::pair(networkID, [&](int i) {
        auto cfg = getTestConfig(i);
        cfg.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = true;
        cfg.ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING = true;
        cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
            Config::CURRENT_LEDGER_PROTOCOL_VERSION;
        cfg.GENESIS_TEST_ACCOUNT_COUNT = 1000;
        return cfg;
    });

    simulation->startAllNodes();
    simulation->crankUntil(
        [&]() { return simulation->haveAllExternalized(3, 1); },
        10 * simulation->getExpectedLedgerCloseTime(), false);
    auto nodes = simulation->getNodes();

    for (auto& node : nodes)
    {
        node->setRunInOverlayOnlyMode(true);
    }

    uint32_t const nAccountsPerNode = 500;
    uint32_t const nTxs = 100;

    auto completedRuns = [&](Application& app) {
        return app.getMetrics()
            .NewMeter({"loadgen", "run", "complete"}, "run")
            .count();
    };
    auto prev0 = completedRuns(*nodes[0]);
    auto prev1 = completedRuns(*nodes[1]);

    // Both nodes generate load concurrently, from disjoint account ranges.
    nodes[0]->getLoadGenerator().generateLoad(
        GeneratedLoadConfig::txLoad(LoadGenMode::PAY, nAccountsPerNode, nTxs,
                                    /* txRate */ 100, /* offset */ 0));
    nodes[1]->getLoadGenerator().generateLoad(GeneratedLoadConfig::txLoad(
        LoadGenMode::PAY, nAccountsPerNode, nTxs,
        /* txRate */ 100, /* offset */ nAccountsPerNode));

    simulation->crankUntil(
        [&]() {
            return completedRuns(*nodes[0]) == prev0 + 1 &&
                   completedRuns(*nodes[1]) == prev1 + 1;
        },
        500 * simulation->getExpectedLedgerCloseTime(), false);
}

TEST_CASE("mixed pregen and synthetic soroban in overlay-only mode",
          "[loadgen]")
{
    uint32_t const nAccounts = 200;
    uint32_t const genesisAccountCount = nAccounts;
    uint32_t const nTxs = 60;

    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    Simulation::pointer simulation = Topologies::pair(networkID, [&](int i) {
        auto cfg = getTestConfig(i);
        cfg.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = true;
        cfg.ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING = true;
        cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
            Config::CURRENT_LEDGER_PROTOCOL_VERSION;
        cfg.GENESIS_TEST_ACCOUNT_COUNT = genesisAccountCount;
        return cfg;
    });

    simulation->startAllNodes();
    simulation->crankUntil(
        [&]() { return simulation->haveAllExternalized(3, 1); },
        10 * simulation->getExpectedLedgerCloseTime(), false);

    auto nodes = simulation->getNodes();
    auto& app = *nodes[0];

    // Max out Soroban network limits so synthetic footprints pass checkValid.
    upgradeSorobanNetworkConfig(
        [&](SorobanNetworkConfig& cfg) {
            auto mx = std::numeric_limits<uint32_t>::max();
            cfg.mLedgerMaxTxCount = mx;
            cfg.mLedgerMaxInstructions = mx;
            cfg.mLedgerMaxTransactionsSizeBytes = mx;
            cfg.mLedgerMaxDiskReadEntries = mx;
            cfg.mLedgerMaxDiskReadBytes = mx;
            cfg.mLedgerMaxWriteLedgerEntries = mx;
            cfg.mLedgerMaxWriteBytes = mx;
            cfg.mTxMaxInstructions = mx;
            cfg.mTxMaxDiskReadEntries = mx;
            cfg.mTxMaxDiskReadBytes = mx;
            cfg.mTxMaxWriteLedgerEntries = mx;
            cfg.mTxMaxWriteBytes = mx;
            cfg.mTxMaxFootprintEntries = mx;
            cfg.mTxMaxSizeBytes = mx;
            cfg.mTxMaxContractEventsSizeBytes = mx;
        },
        simulation);

    // Both classic pregen and synthetic soroban streams draw from the same
    // account pool; cross-queue source-account conflicts are allowed in
    // overlay-only mode (HerderImpl bypasses the one-tx-per-source-per-ledger
    // check), so no disjointness is required.
    std::string fileName =
        app.getConfig().LOADGEN_PREGENERATED_TRANSACTIONS_FILE;
    auto cleanup = gsl::finally([&]() { std::remove(fileName.c_str()); });
    generateTransactions(app, fileName, nTxs, nAccounts, /* offset */ 0);

    for (auto& node : nodes)
    {
        node->setRunInOverlayOnlyMode(true);
    }

    auto prev = app.getMetrics()
                    .NewMeter({"loadgen", "run", "complete"}, "run")
                    .count();

    auto runMixed = [&](LoadGenMode mode) {
        GeneratedLoadConfig cfg =
            GeneratedLoadConfig::txLoad(mode, nAccounts, nTxs,
                                        /* txRate */ 1, /* offset */ 0);
        cfg.preloadedTransactionsFile =
            app.getConfig().LOADGEN_PREGENERATED_TRANSACTIONS_FILE;
        auto& mix = cfg.getMutMixPregenSorobanConfig();
        mix.classicTxRate = 100;
        mix.sorobanTxRate = 50;
        cfg.txRate = mix.classicTxRate + mix.sorobanTxRate;
        app.getLoadGenerator().generateLoad(cfg);
    };

    SECTION("sac payment")
    {
        runMixed(LoadGenMode::MIXED_PREGEN_SAC_PAYMENT);
    }
    SECTION("oz token transfer")
    {
        runMixed(LoadGenMode::MIXED_PREGEN_OZ_TOKEN_TRANSFER);
    }
    SECTION("soroswap swap")
    {
        runMixed(LoadGenMode::MIXED_PREGEN_SOROSWAP_SWAP);
    }

    simulation->crankUntil(
        [&]() {
            return app.getMetrics()
                       .NewMeter({"loadgen", "run", "complete"}, "run")
                       .count() == prev + 1;
        },
        100 * simulation->getExpectedLedgerCloseTime(), false);
}

TEST_CASE("generate load with unique accounts", "[loadgen]")
{
    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    uint32_t const nAccounts = 1000;
    // Sized for a real-time simulation (the Rust overlay has no virtual-time
    // mode): each section finishes in well under a minute.
    uint32_t const nTxs = 2000;

    Simulation::pointer simulation = Topologies::pair(networkID, [&](int i) {
        auto cfg = getTestConfig(i);
        cfg.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = true;
        cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 5000;
        uint32_t baseSize = 148;
        uint32_t opSize = 56;
        cfg.ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING = true;
        cfg.LOADGEN_BYTE_COUNT_FOR_TESTING = {0, baseSize + opSize * 2,
                                              baseSize + opSize * 10};
        cfg.LOADGEN_BYTE_COUNT_DISTRIBUTION_FOR_TESTING = {80, 19, 1};
        cfg.GENESIS_TEST_ACCOUNT_COUNT = nAccounts * 10;
        return cfg;
    });

    simulation->startAllNodes();
    simulation->crankUntil(
        [&]() { return simulation->haveAllExternalized(3, 1); },
        10 * simulation->getExpectedLedgerCloseTime(), false);

    auto nodes = simulation->getNodes();
    auto& app = *nodes[0]; // pick a node to generate load

    std::string fileName =
        app.getConfig().LOADGEN_PREGENERATED_TRANSACTIONS_FILE;
    auto cleanup = gsl::finally([&]() { std::remove(fileName.c_str()); });

    generateTransactions(app, fileName, nTxs, nAccounts,
                         /* offset */ nAccounts);

    auto& loadGen = app.getLoadGenerator();

    auto getSuccessfulTxCount = [&]() {
        return nodes[0]
            ->getMetrics()
            .NewCounter({"ledger", "apply", "success"})
            .count();
    };

    SECTION("pregenerated transactions")
    {
        auto const& cfg = app.getConfig();
        loadGen.generateLoad(GeneratedLoadConfig::pregeneratedTxLoad(
            nAccounts, /* nTxs */ nTxs, /* txRate */ 200,
            /* offset*/ nAccounts, cfg.LOADGEN_PREGENERATED_TRANSACTIONS_FILE));
        simulation->crankUntil(
            [&]() {
                return app.getMetrics()
                           .NewMeter({"loadgen", "run", "complete"}, "run")
                           .count() == 1;
            },
            120 * simulation->getExpectedLedgerCloseTime(), false);
        REQUIRE(getSuccessfulTxCount() == nTxs);
    }
    SECTION("success")
    {
        uint32_t const nTxs = 1000;

        loadGen.generateLoad(GeneratedLoadConfig::txLoad(LoadGenMode::PAY,
                                                         nAccounts, nTxs,
                                                         /* txRate */ 100));
        simulation->crankUntil(
            [&]() {
                return app.getMetrics()
                           .NewMeter({"loadgen", "run", "complete"}, "run")
                           .count() == 1;
            },
            120 * simulation->getExpectedLedgerCloseTime(), false);
        REQUIRE(getSuccessfulTxCount() == nTxs);
    }
    SECTION("invalid loadgen parameters")
    {
        uint32 numAccounts = 100;
        loadGen.generateLoad(
            GeneratedLoadConfig::txLoad(LoadGenMode::PAY,
                                        /* nAccounts */ numAccounts,
                                        /* nTxs */ numAccounts * 2,
                                        /* txRate */ 100));
        simulation->crankUntil(
            [&]() {
                return app.getMetrics()
                           .NewMeter({"loadgen", "run", "failed"}, "run")
                           .count() == 1;
            },
            10 * simulation->getExpectedLedgerCloseTime(), false);
    }
    SECTION("stop loadgen")
    {
        loadGen.generateLoad(GeneratedLoadConfig::txLoad(LoadGenMode::PAY,
                                                         /* nAccounts */ 1000,
                                                         /* nTxs */ 1000 * 2,
                                                         /* txRate */ 1));
        simulation->crankForAtLeast(std::chrono::seconds(10), false);
        auto& acc = app.getMetrics().NewMeter({"loadgen", "account", "created"},
                                              "account");
        auto numAccounts = acc.count();
        REQUIRE(app.getMetrics()
                    .NewMeter({"loadgen", "run", "failed"}, "run")
                    .count() == 0);
        loadGen.stop();
        REQUIRE(app.getMetrics()
                    .NewMeter({"loadgen", "run", "failed"}, "run")
                    .count() == 1);
        // No new txs submitted
        simulation->crankForAtLeast(std::chrono::seconds(10), false);
        REQUIRE(acc.count() == numAccounts);
    }
}

TEST_CASE("modify soroban network config", "[loadgen][soroban]")
{
    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    Simulation::pointer simulation = Topologies::pair(networkID, [&](int i) {
        auto cfg = getTestConfig(i);
        cfg.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = true;
        cfg.ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING = true;
        return cfg;
    });

    simulation->startAllNodes();
    simulation->crankUntil(
        [&]() { return simulation->haveAllExternalized(3, 1); },
        10 * simulation->getExpectedLedgerCloseTime(), false);
    auto nodes = simulation->getNodes();
    auto& app = *nodes[0]; // pick a node to generate load

    uint32_t const ledgerMaxTxCount = 42;
    uint32_t const liveSorobanStateSizeWindowSampleSize = 99;
    // Upgrade the network config.
    upgradeSorobanNetworkConfig(
        [&](SorobanNetworkConfig& cfg) {
            cfg.mLedgerMaxTxCount = ledgerMaxTxCount;
            cfg.mStateArchivalSettings.liveSorobanStateSizeWindowSampleSize =
                liveSorobanStateSizeWindowSampleSize;
        },
        simulation);
    // Check that the settings were properly updated.
    LedgerTxn ltx(app.getLedgerTxnRoot());
    auto contractExecutionLanesSettingsEntry =
        ltx.load(configSettingKey(CONFIG_SETTING_CONTRACT_EXECUTION_LANES));
    auto stateArchivalConfigSettinsgEntry =
        ltx.load(configSettingKey(CONFIG_SETTING_STATE_ARCHIVAL));
    auto& contractExecutionLanesSettings =
        contractExecutionLanesSettingsEntry.current().data.configSetting();
    auto& stateArchivalSettings =
        stateArchivalConfigSettinsgEntry.current().data.configSetting();
    REQUIRE(contractExecutionLanesSettings.contractExecutionLanes()
                .ledgerMaxTxCount == ledgerMaxTxCount);
    REQUIRE(stateArchivalSettings.stateArchivalSettings()
                .liveSorobanStateSizeWindowSampleSize ==
            liveSorobanStateSizeWindowSampleSize);
}

TEST_CASE("Multi-byte payment transactions are valid", "[loadgen]")
{
    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    uint32_t constexpr baseSize = 148;
    uint32_t constexpr opSize = 56;
    uint32_t constexpr frameSize = baseSize + opSize * 3;
    Simulation::pointer simulation = Topologies::pair(networkID, [](int i) {
        auto cfg = getTestConfig(i);
        cfg.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = true;
        cfg.ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING = true;
        cfg.LOADGEN_BYTE_COUNT_FOR_TESTING = {frameSize};
        cfg.LOADGEN_BYTE_COUNT_DISTRIBUTION_FOR_TESTING = {1};
        cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 1000;
        cfg.GENESIS_TEST_ACCOUNT_COUNT = 100;
        return cfg;
    });

    simulation->startAllNodes();
    simulation->crankUntil(
        [&]() { return simulation->haveAllExternalized(3, 1); },
        10 * simulation->getExpectedLedgerCloseTime(), false);

    auto nodes = simulation->getNodes();
    auto& app = *nodes[0]; // pick a node to generate load

    uint32_t txRate = 5;
    auto& loadGen = app.getLoadGenerator();
    try
    {
        auto config = GeneratedLoadConfig::txLoad(
            LoadGenMode::PAY, app.getConfig().GENESIS_TEST_ACCOUNT_COUNT, 100,
            txRate);
        loadGen.generateLoad(config);
        simulation->crankUntil(
            [&]() {
                return app.getMetrics()
                           .NewMeter({"loadgen", "run", "complete"}, "run")
                           .count() == 1;
            },
            60 * simulation->getExpectedLedgerCloseTime(), false);
    }
    catch (...)
    {
        auto problems = loadGen.checkAccountSynced(app);
        REQUIRE(problems.empty());
    }

    REQUIRE(app.getMetrics()
                .NewMeter({"loadgen", "txn", "rejected"}, "txn")
                .count() == 0);
    auto ops = app.getMetrics()
                   .NewMeter({"loadgen", "payment", "submitted"}, "op")
                   .count();
    REQUIRE(ops == 100);

    auto bytes = app.getMetrics()
                     .NewMeter({"loadgen", "payment", "bytes"}, "txn")
                     .count();
    REQUIRE(bytes == ops * frameSize);
}

TEST_CASE("apply load", "[loadgen][applyload][acceptance]")
{
    auto const timingPhases =
        GENERATE(ApplyLoadTimingPhases::APPLY_ONLY,
                 ApplyLoadTimingPhases::TX_SET_VALIDATION_AND_APPLY);
    auto cfg = getTestConfig();
    cfg.APPLY_LOAD_MODE = ApplyLoadMode::LIMIT_BASED;
    cfg.APPLY_LOAD_TIMING_PHASES = timingPhases;
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 1000;
    cfg.USE_CONFIG_FOR_GENESIS = true;
    cfg.LEDGER_PROTOCOL_VERSION = Config::CURRENT_LEDGER_PROTOCOL_VERSION;
    cfg.MANUAL_CLOSE = true;
    cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS = false;
    cfg.GENESIS_TEST_ACCOUNT_COUNT = 10000;

    cfg.APPLY_LOAD_CLASSIC_TXS_PER_LEDGER = 100;

    // BL generation parameters
    cfg.APPLY_LOAD_BL_SIMULATED_LEDGERS = 10000;
    cfg.APPLY_LOAD_BL_WRITE_FREQUENCY = 1000;
    cfg.APPLY_LOAD_BL_BATCH_SIZE = 1000;
    cfg.APPLY_LOAD_BL_LAST_BATCH_LEDGERS = 300;
    cfg.APPLY_LOAD_BL_LAST_BATCH_SIZE = 100;

    cfg.APPLY_LOAD_EVENT_COUNT = {100};
    cfg.APPLY_LOAD_EVENT_COUNT_DISTRIBUTION = {1};

    // Ledger and transaction limits
    cfg.APPLY_LOAD_LEDGER_MAX_INSTRUCTIONS = 500'000'000;
    cfg.APPLY_LOAD_TX_MAX_INSTRUCTIONS = 100'000'000;
    cfg.APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS = 2;

    cfg.APPLY_LOAD_LEDGER_MAX_DISK_READ_LEDGER_ENTRIES = 200;
    cfg.APPLY_LOAD_TX_MAX_DISK_READ_LEDGER_ENTRIES = 10;
    cfg.APPLY_LOAD_TX_MAX_FOOTPRINT_SIZE = 100;

    cfg.APPLY_LOAD_LEDGER_MAX_DISK_READ_BYTES = 1'000'000;
    cfg.APPLY_LOAD_TX_MAX_DISK_READ_BYTES = 200'000;

    cfg.APPLY_LOAD_LEDGER_MAX_WRITE_LEDGER_ENTRIES = 1250;
    cfg.APPLY_LOAD_TX_MAX_WRITE_LEDGER_ENTRIES = 50;

    cfg.APPLY_LOAD_LEDGER_MAX_WRITE_BYTES = 700'000;
    cfg.APPLY_LOAD_TX_MAX_WRITE_BYTES = 66560;

    cfg.APPLY_LOAD_MAX_TX_SIZE_BYTES = 71680;
    cfg.APPLY_LOAD_MAX_LEDGER_TX_SIZE_BYTES = 800'000;

    cfg.APPLY_LOAD_MAX_CONTRACT_EVENT_SIZE_BYTES = 8198;
    cfg.APPLY_LOAD_MAX_SOROBAN_TX_COUNT = 50;

    cfg.APPLY_LOAD_NUM_LEDGERS = 10;

    cfg.ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING = true;

    VirtualClock clock(VirtualClock::REAL_TIME);
    auto app = createTestApplication(clock, cfg);

    ApplyLoad al(*app);

    // Sample a few indices to verify hot archive is properly initialized
    uint32_t expectedArchivedEntries = al.getTotalHotArchiveEntries();
    std::vector<uint32_t> sampleIndices = {0, expectedArchivedEntries / 2,
                                           expectedArchivedEntries - 1};
    std::set<LedgerKey, LedgerEntryIdCmp> sampleKeys;

    auto ledgerView = app->getLedgerManager().copyImmutableLedgerView();

    for (auto idx : sampleIndices)
    {
        sampleKeys.insert(ApplyLoad::getKeyForArchivedEntry(idx));
    }

    auto sampleEntries = ledgerView.loadArchiveKeys(sampleKeys, "test");
    REQUIRE(sampleEntries.size() == sampleKeys.size());

    auto const lclBeforeExecute =
        app->getLedgerManager().getLastClosedLedgerNum();
    al.execute();

    REQUIRE(app->getLedgerManager().getLastClosedLedgerNum() >=
            lclBeforeExecute + cfg.APPLY_LOAD_NUM_LEDGERS);
    REQUIRE(1.0 - al.successRate() < std::numeric_limits<double>::epsilon());
    if (timingPhases == ApplyLoadTimingPhases::TX_SET_VALIDATION_AND_APPLY)
    {
        // Each benchmark ledger runs at least one cold tx set validation.
        REQUIRE(app->getMetrics()
                    .NewTimer({"herder", "txset", "validate"})
                    .count() >= cfg.APPLY_LOAD_NUM_LEDGERS);
    }
}

TEST_CASE("apply load benchmark model tx",
          "[loadgen][applyload][soroban][acceptance]")
{
    auto const timingPhases =
        GENERATE(ApplyLoadTimingPhases::APPLY_ONLY,
                 ApplyLoadTimingPhases::TX_SET_VALIDATION_AND_APPLY);
    auto cfg = getTestConfig();
    cfg.APPLY_LOAD_MODE = ApplyLoadMode::BENCHMARK_MODEL_TX;
    cfg.APPLY_LOAD_MODEL_TX = ApplyLoadModelTx::SAC;
    cfg.APPLY_LOAD_TIMING_PHASES = timingPhases;
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 1000;
    cfg.USE_CONFIG_FOR_GENESIS = true;
    cfg.LEDGER_PROTOCOL_VERSION = Config::CURRENT_LEDGER_PROTOCOL_VERSION;
    cfg.MANUAL_CLOSE = true;
    cfg.IGNORE_MESSAGE_LIMITS_FOR_TESTING = true;
    cfg.GENESIS_TEST_ACCOUNT_COUNT = 2000;

    cfg.APPLY_LOAD_NUM_LEDGERS = 10;
    cfg.APPLY_LOAD_MAX_SOROBAN_TX_COUNT = 500;
    cfg.APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS = 2;
    cfg.APPLY_LOAD_BATCH_SAC_COUNT = 2;
    cfg.APPLY_LOAD_CLASSIC_TXS_PER_LEDGER = 100;

    VirtualClock clock(VirtualClock::REAL_TIME);
    auto app = createTestApplication(clock, cfg);

    ApplyLoad al(*app);

    auto const lclBeforeExecute =
        app->getLedgerManager().getLastClosedLedgerNum();
    al.execute();

    REQUIRE(app->getLedgerManager().getLastClosedLedgerNum() >=
            lclBeforeExecute + cfg.APPLY_LOAD_NUM_LEDGERS);
    REQUIRE(1.0 - al.successRate() < std::numeric_limits<double>::epsilon());

    auto& successCountMetric =
        app->getMetrics().NewCounter({"ledger", "apply-soroban", "success"});
    REQUIRE(successCountMetric.count() > 0);
    if (timingPhases == ApplyLoadTimingPhases::TX_SET_VALIDATION_AND_APPLY)
    {
        // Each benchmark ledger runs at least one cold tx set validation.
        REQUIRE(app->getMetrics()
                    .NewTimer({"herder", "txset", "validate"})
                    .count() >= cfg.APPLY_LOAD_NUM_LEDGERS);
    }
}
