// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketManager.h"
#include "crypto/SHA.h"
#include "crypto/SecretKey.h"
#include "ledger/LedgerManager.h"
#include "main/Config.h"
#include "scp/QuorumSetUtils.h"
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
    Simulation::pointer simulation =
        Topologies::pair(Simulation::OVER_LOOPBACK, networkID, [&](int i) {
            auto cfg = getTestConfig(i);
            cfg.APPLY_LOAD_NUM_DISK_READ_ENTRIES = {10};
            cfg.APPLY_LOAD_NUM_DISK_READ_ENTRIES_DISTRIBUTION = {100};
            cfg.APPLY_LOAD_NUM_RW_ENTRIES = {5};
            cfg.APPLY_LOAD_NUM_RW_ENTRIES_DISTRIBUTION = {100};
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

    // Upgrade the network config.
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
            LoadGenMode::PAY, nAccounts, nTxs, /* txRate */ 1));
    }
    SECTION("invoke realistic")
    {
        // Simulate realistic invoke transactions
        app.getLoadGenerator().generateLoad(
            GeneratedLoadConfig::txLoad(LoadGenMode::SOROBAN_INVOKE_APPLY_LOAD,
                                        nAccounts, nTxs, /* txRate */ 1));
    }
    simulation->crankUntil(
        [&]() {
            return app.getMetrics()
                       .NewMeter({"loadgen", "run", "complete"}, "run")
                       .count() == prev + 1;
        },
        500 * simulation->getExpectedLedgerCloseTime(), false);
}

TEST_CASE("generate load in protocol 1", "[loadgen]")
{
    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    Simulation::pointer simulation =
        Topologies::pair(Simulation::OVER_LOOPBACK, networkID, [](int i) {
            auto cfg = getTestConfig(i);
            cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 5000;
            cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION = 1;
            cfg.GENESIS_TEST_ACCOUNT_COUNT = 10000;
            cfg.ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING = true;
            return cfg;
        });

    simulation->startAllNodes();
    simulation->crankUntil(
        [&]() { return simulation->haveAllExternalized(3, 1); },
        2 * simulation->getExpectedLedgerCloseTime(), false);

    auto nodes = simulation->getNodes();
    auto& app = *nodes[0]; // pick a node to generate load

    auto& loadGen = app.getLoadGenerator();
    loadGen.generateLoad(GeneratedLoadConfig::txLoad(
        LoadGenMode::PAY, app.getConfig().GENESIS_TEST_ACCOUNT_COUNT, 1000,
        /* txRate */ 10));
    simulation->crankUntil(
        [&]() {
            return app.getMetrics()
                       .NewMeter({"loadgen", "run", "complete"}, "run")
                       .count() == 1;
        },
        100 * simulation->getExpectedLedgerCloseTime(), false);
}

TEST_CASE("generate load with unique accounts", "[loadgen]")
{
    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    uint32_t const nAccounts = 1000;
    uint32_t const nTxs = 100000;

    Simulation::pointer simulation =
        Topologies::pair(Simulation::OVER_LOOPBACK, networkID, [&](int i) {
            auto cfg = getTestConfig(i);
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
        2 * simulation->getExpectedLedgerCloseTime(), false);

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
            nAccounts, /* nTxs */ nTxs, /* txRate */ 50,
            /* offset*/ nAccounts, cfg.LOADGEN_PREGENERATED_TRANSACTIONS_FILE));
        simulation->crankUntil(
            [&]() {
                return app.getMetrics()
                           .NewMeter({"loadgen", "run", "complete"}, "run")
                           .count() == 1;
            },
            500 * simulation->getExpectedLedgerCloseTime(), false);
        REQUIRE(getSuccessfulTxCount() == nTxs);
    }
    SECTION("success")
    {
        uint32_t const nTxs = 10000;

        loadGen.generateLoad(GeneratedLoadConfig::txLoad(LoadGenMode::PAY,
                                                         nAccounts, nTxs,
                                                         /* txRate */ 50));
        simulation->crankUntil(
            [&]() {
                return app.getMetrics()
                           .NewMeter({"loadgen", "run", "complete"}, "run")
                           .count() == 1;
            },
            300 * simulation->getExpectedLedgerCloseTime(), false);
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
    Simulation::pointer simulation =
        Topologies::pair(Simulation::OVER_LOOPBACK, networkID, [&](int i) {
            auto cfg = getTestConfig(i);
            cfg.ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING = true;
            return cfg;
        });

    simulation->startAllNodes();
    simulation->crankUntil(
        [&]() { return simulation->haveAllExternalized(3, 1); },
        2 * simulation->getExpectedLedgerCloseTime(), false);
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

TEST_CASE("generate soroban load", "[loadgen][soroban]")
{
    uint32_t const numDataEntries = 5;
    uint32_t const ioKiloBytes = 15;

    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    Simulation::pointer simulation =
        Topologies::pair(Simulation::OVER_LOOPBACK, networkID, [&](int i) {
            auto cfg = getTestConfig(i);
            cfg.USE_CONFIG_FOR_GENESIS = false;
            cfg.ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING = true;
            cfg.UPDATE_SOROBAN_COSTS_DURING_PROTOCOL_UPGRADE_FOR_TESTING = true;
            cfg.GENESIS_TEST_ACCOUNT_COUNT = 20;
            //  Use tight bounds to we can verify storage works properly
            cfg.LOADGEN_NUM_DATA_ENTRIES_FOR_TESTING = {numDataEntries};
            cfg.LOADGEN_NUM_DATA_ENTRIES_DISTRIBUTION_FOR_TESTING = {1};
            cfg.LOADGEN_IO_KILOBYTES_FOR_TESTING = {ioKiloBytes};
            cfg.LOADGEN_IO_KILOBYTES_DISTRIBUTION_FOR_TESTING = {1};

            cfg.LOADGEN_TX_SIZE_BYTES_FOR_TESTING = {20'000, 50'000, 80'000};
            cfg.LOADGEN_TX_SIZE_BYTES_DISTRIBUTION_FOR_TESTING = {1, 2, 1};
            cfg.LOADGEN_INSTRUCTIONS_FOR_TESTING = {1'000'000, 5'000'000,
                                                    10'000'000};
            cfg.LOADGEN_INSTRUCTIONS_DISTRIBUTION_FOR_TESTING = {1, 2, 3};
            return cfg;
        });

    simulation->startAllNodes();
    simulation->crankUntil(
        [&]() { return simulation->haveAllExternalized(3, 1); },
        2 * simulation->getExpectedLedgerCloseTime(), false);

    auto nodes = simulation->getNodes();

    auto& app = *nodes[0]; // pick a node to generate load
    Upgrades::UpgradeParameters scheduledUpgrades;
    auto lclCloseTime =
        VirtualClock::from_time_t(app.getLedgerManager()
                                      .getLastClosedLedgerHeader()
                                      .header.scpValue.closeTime);
    scheduledUpgrades.mUpgradeTime = lclCloseTime;
    scheduledUpgrades.mProtocolVersion =
        Config::CURRENT_LEDGER_PROTOCOL_VERSION;
    for (auto const& node : nodes)
    {
        node->getHerder().setUpgrades(scheduledUpgrades);
    }
    simulation->crankForAtLeast(std::chrono::seconds(20), false);

    auto& loadGen = app.getLoadGenerator();
    auto getSuccessfulTxCount = [&]() {
        return nodes[0]
            ->getMetrics()
            .NewCounter({"ledger", "apply-soroban", "success"})
            .count();
    };

    auto nAccounts = 20;
    // Accounts are created via GENESIS_TEST_ACCOUNT_COUNT
    auto& complete =
        app.getMetrics().NewMeter({"loadgen", "run", "complete"}, "run");
    auto completeCount = complete.count();

    // Before creating any contracts, test that loadgen correctly
    // reports an error when trying to run a soroban invoke setup.
    SECTION("misconfigured soroban loadgen mode usage")
    {
        // Users are required to run SOROBAN_INVOKE_SETUP_LOAD before running
        // SOROBAN_INVOKE_LOAD. Running a SOROBAN_INVOKE_LOAD without a prior
        // SOROBAN_INVOKE_SETUP_LOAD should throw a helpful exception explaining
        // the misconfiguration.
        auto invokeLoadCfg =
            GeneratedLoadConfig::txLoad(LoadGenMode::SOROBAN_INVOKE,
                                        /* nAccounts*/ 1, /* numSorobanTxs */ 1,
                                        /* txRate */ 1);
        REQUIRE_THROWS_WITH(
            loadGen.generateLoad(invokeLoadCfg),
            "Before running MODE::SOROBAN_INVOKE, please run "
            "MODE::SOROBAN_INVOKE_SETUP to set up your contract first.");
    }
    int64_t numTxsBefore = getSuccessfulTxCount();

    // Make sure config upgrade works with initial network config settings
    loadGen.generateLoad(GeneratedLoadConfig::createSorobanUpgradeSetupLoad());
    completeCount = complete.count();
    simulation->crankUntil(
        [&]() { return complete.count() == completeCount + 1; },
        100 * simulation->getExpectedLedgerCloseTime(), false);

    // Check that Soroban TXs were successfully applied
    for (auto node : nodes)
    {
        auto& txsSucceeded = node->getMetrics().NewCounter(
            {"ledger", "apply-soroban", "success"});
        auto& txsFailed = node->getMetrics().NewCounter(
            {"ledger", "apply-soroban", "failure"});

        // Should be 1 upload wasm TX followed by one instance deploy TX
        REQUIRE(txsSucceeded.count() == numTxsBefore + 2);
        REQUIRE(txsFailed.count() == 0);
    }

    auto createUpgradeLoadGenConfig = GeneratedLoadConfig::txLoad(
        LoadGenMode::SOROBAN_CREATE_UPGRADE, nAccounts, 10,
        /* txRate */ 1);
    auto& upgradeCfg = createUpgradeLoadGenConfig.getMutSorobanUpgradeConfig();

    upgradeCfg.maxContractSizeBytes =
        rand_uniform<uint32_t>(UINT32_MAX - 10'000, UINT32_MAX);
    upgradeCfg.maxContractDataKeySizeBytes =
        rand_uniform<uint32_t>(UINT32_MAX - 10'000, UINT32_MAX);
    upgradeCfg.maxContractDataEntrySizeBytes =
        rand_uniform<uint32_t>(UINT32_MAX - 10'000, UINT32_MAX);
    upgradeCfg.ledgerMaxInstructions =
        rand_uniform<int64_t>(INT64_MAX - 10'000, INT64_MAX);
    upgradeCfg.txMaxInstructions =
        rand_uniform<int64_t>(INT64_MAX - 10'000, INT64_MAX);
    upgradeCfg.txMemoryLimit =
        rand_uniform<uint32_t>(UINT32_MAX - 10'000, UINT32_MAX);
    upgradeCfg.ledgerMaxDiskReadEntries =
        rand_uniform<uint32_t>(UINT32_MAX - 10'000, UINT32_MAX);
    upgradeCfg.ledgerMaxDiskReadBytes =
        rand_uniform<uint32_t>(UINT32_MAX - 10'000, UINT32_MAX);
    upgradeCfg.ledgerMaxWriteLedgerEntries =
        rand_uniform<uint32_t>(UINT32_MAX - 10'000, UINT32_MAX);
    upgradeCfg.ledgerMaxWriteBytes =
        rand_uniform<uint32_t>(UINT32_MAX - 10'000, UINT32_MAX);
    upgradeCfg.ledgerMaxTxCount =
        rand_uniform<uint32_t>(UINT32_MAX - 10'000, UINT32_MAX);
    upgradeCfg.txMaxDiskReadEntries =
        rand_uniform<uint32_t>(UINT32_MAX - 10'000, UINT32_MAX);
    upgradeCfg.txMaxDiskReadBytes =
        rand_uniform<uint32_t>(UINT32_MAX - 10'000, UINT32_MAX);
    upgradeCfg.txMaxWriteLedgerEntries =
        rand_uniform<uint32_t>(UINT32_MAX - 10'000, UINT32_MAX);
    upgradeCfg.txMaxWriteBytes =
        rand_uniform<uint32_t>(UINT32_MAX - 10'000, UINT32_MAX);
    upgradeCfg.txMaxContractEventsSizeBytes =
        rand_uniform<uint32_t>(UINT32_MAX - 10'000, UINT32_MAX);
    upgradeCfg.ledgerMaxTransactionsSizeBytes =
        rand_uniform<uint32_t>(UINT32_MAX - 10'000, UINT32_MAX);
    upgradeCfg.txMaxSizeBytes =
        rand_uniform<uint32_t>(UINT32_MAX - 10'000, UINT32_MAX);
    upgradeCfg.liveSorobanStateSizeWindowSampleSize =
        rand_uniform<uint32_t>(UINT32_MAX - 10'000, UINT32_MAX);
    upgradeCfg.evictionScanSize =
        rand_uniform<int64_t>(INT64_MAX - 10'000, INT64_MAX);
    upgradeCfg.startingEvictionScanLevel = rand_uniform<uint32_t>(4, 8);

    if (protocolVersionStartsFrom(Config::CURRENT_LEDGER_PROTOCOL_VERSION,
                                  ProtocolVersion::V_23))
    {
        upgradeCfg.ledgerMaxDependentTxClusters = rand_uniform<uint32_t>(2, 10);
        upgradeCfg.txMaxFootprintEntries =
            rand_uniform<uint32_t>(UINT32_MAX - 10'000, UINT32_MAX);
        upgradeCfg.feeFlatRateWrite1KB =
            rand_uniform<int64_t>(INT64_MAX - 10'000, INT64_MAX);

        upgradeCfg.ledgerTargetCloseTimeMilliseconds =
            rand_uniform<uint32_t>(4000, 5000);
        upgradeCfg.nominationTimeoutInitialMilliseconds =
            rand_uniform<uint32_t>(1000, 1500);
        upgradeCfg.nominationTimeoutIncrementMilliseconds =
            rand_uniform<uint32_t>(1000, 1500);
        upgradeCfg.ballotTimeoutInitialMilliseconds =
            rand_uniform<uint32_t>(1000, 1500);
        upgradeCfg.ballotTimeoutIncrementMilliseconds =
            rand_uniform<uint32_t>(1000, 1500);
    }

    auto upgradeSetKey = loadGen.getConfigUpgradeSetKey(
        createUpgradeLoadGenConfig.getSorobanUpgradeConfig());

    numTxsBefore = getSuccessfulTxCount();
    loadGen.generateLoad(createUpgradeLoadGenConfig);
    completeCount = complete.count();
    simulation->crankUntil(
        [&]() { return complete.count() == completeCount + 1; },
        300 * simulation->getExpectedLedgerCloseTime(), false);

    for (auto node : nodes)
    {
        auto& txsSucceeded = node->getMetrics().NewCounter(
            {"ledger", "apply-soroban", "success"});
        auto& txsFailed = node->getMetrics().NewCounter(
            {"ledger", "apply-soroban", "failure"});

        // Should be a single contract invocation
        REQUIRE(txsSucceeded.count() == numTxsBefore + 1);
        REQUIRE(txsFailed.count() == 0);
    }

    // Check that the upgrade entry was properly written
    SCVal upgradeHashBytes(SCV_BYTES);
    upgradeHashBytes.bytes() = xdr::xdr_to_opaque(upgradeSetKey.contentHash);

    SCAddress addr(SC_ADDRESS_TYPE_CONTRACT);
    addr.contractId() = upgradeSetKey.contractID;

    LedgerKey upgradeLK(CONTRACT_DATA);
    upgradeLK.contractData().durability = TEMPORARY;
    upgradeLK.contractData().contract = addr;
    upgradeLK.contractData().key = upgradeHashBytes;

    ConfigUpgradeSet upgrades;
    {
        LedgerTxn ltx(app.getLedgerTxnRoot());
        auto entry = ltx.load(upgradeLK);
        REQUIRE(entry);
        xdr::xdr_from_opaque(entry.current().data.contractData().val.bytes(),
                             upgrades);
    }

    for (auto const& setting : upgrades.updatedEntry)
    {
        // Loadgen doesn't update the cost types and non-upgradeable settings
        REQUIRE(!SorobanNetworkConfig::isNonUpgradeableConfigSettingEntry(
            setting.configSettingID()));
        REQUIRE(setting.configSettingID() !=
                CONFIG_SETTING_CONTRACT_COST_PARAMS_CPU_INSTRUCTIONS);
        REQUIRE(setting.configSettingID() !=
                CONFIG_SETTING_CONTRACT_COST_PARAMS_MEMORY_BYTES);

        switch (setting.configSettingID())
        {
        case CONFIG_SETTING_CONTRACT_MAX_SIZE_BYTES:
            REQUIRE(setting.contractMaxSizeBytes() ==
                    upgradeCfg.maxContractSizeBytes);
            break;
        case CONFIG_SETTING_CONTRACT_COMPUTE_V0:
            REQUIRE(setting.contractCompute().ledgerMaxInstructions ==
                    upgradeCfg.ledgerMaxInstructions);
            REQUIRE(setting.contractCompute().txMaxInstructions ==
                    upgradeCfg.txMaxInstructions);
            REQUIRE(setting.contractCompute().txMemoryLimit ==
                    upgradeCfg.txMemoryLimit);
            break;
        case CONFIG_SETTING_CONTRACT_LEDGER_COST_V0:
            REQUIRE(setting.contractLedgerCost().ledgerMaxDiskReadEntries ==
                    upgradeCfg.ledgerMaxDiskReadEntries);
            REQUIRE(setting.contractLedgerCost().ledgerMaxDiskReadBytes ==
                    upgradeCfg.ledgerMaxDiskReadBytes);
            REQUIRE(setting.contractLedgerCost().ledgerMaxWriteLedgerEntries ==
                    upgradeCfg.ledgerMaxWriteLedgerEntries);
            REQUIRE(setting.contractLedgerCost().ledgerMaxWriteBytes ==
                    upgradeCfg.ledgerMaxWriteBytes);
            REQUIRE(setting.contractLedgerCost().txMaxDiskReadEntries ==
                    upgradeCfg.txMaxDiskReadEntries);
            REQUIRE(setting.contractLedgerCost().txMaxDiskReadBytes ==
                    upgradeCfg.txMaxDiskReadBytes);
            REQUIRE(setting.contractLedgerCost().txMaxWriteLedgerEntries ==
                    upgradeCfg.txMaxWriteLedgerEntries);
            REQUIRE(setting.contractLedgerCost().txMaxWriteBytes ==
                    upgradeCfg.txMaxWriteBytes);
            break;
        case CONFIG_SETTING_CONTRACT_HISTORICAL_DATA_V0:
            break;
        case CONFIG_SETTING_CONTRACT_EVENTS_V0:
            REQUIRE(setting.contractEvents().txMaxContractEventsSizeBytes ==
                    upgradeCfg.txMaxContractEventsSizeBytes);
            break;
        case CONFIG_SETTING_CONTRACT_BANDWIDTH_V0:
            REQUIRE(setting.contractBandwidth().ledgerMaxTxsSizeBytes ==
                    upgradeCfg.ledgerMaxTransactionsSizeBytes);
            REQUIRE(setting.contractBandwidth().txMaxSizeBytes ==
                    upgradeCfg.txMaxSizeBytes);
            break;
        case CONFIG_SETTING_CONTRACT_COST_PARAMS_CPU_INSTRUCTIONS:
        case CONFIG_SETTING_CONTRACT_COST_PARAMS_MEMORY_BYTES:
            break;
        case CONFIG_SETTING_CONTRACT_DATA_KEY_SIZE_BYTES:
            REQUIRE(setting.contractDataKeySizeBytes() ==
                    upgradeCfg.maxContractDataKeySizeBytes);
            break;
        case CONFIG_SETTING_CONTRACT_DATA_ENTRY_SIZE_BYTES:
            REQUIRE(setting.contractDataEntrySizeBytes() ==
                    upgradeCfg.maxContractDataEntrySizeBytes);
            break;
        case CONFIG_SETTING_STATE_ARCHIVAL:
        {
            auto& ses = setting.stateArchivalSettings();
            REQUIRE(ses.liveSorobanStateSizeWindowSampleSize ==
                    upgradeCfg.liveSorobanStateSizeWindowSampleSize);
            REQUIRE(ses.evictionScanSize == upgradeCfg.evictionScanSize);
            REQUIRE(ses.startingEvictionScanLevel ==
                    upgradeCfg.startingEvictionScanLevel);
        }
        break;
        case CONFIG_SETTING_CONTRACT_EXECUTION_LANES:
            REQUIRE(setting.contractExecutionLanes().ledgerMaxTxCount ==
                    upgradeCfg.ledgerMaxTxCount);
            break;
        case CONFIG_SETTING_CONTRACT_PARALLEL_COMPUTE_V0:
            REQUIRE(setting.contractParallelCompute()
                        .ledgerMaxDependentTxClusters ==
                    upgradeCfg.ledgerMaxDependentTxClusters);
            break;
        case CONFIG_SETTING_CONTRACT_LEDGER_COST_EXT_V0:
            REQUIRE(setting.contractLedgerCostExt().txMaxFootprintEntries ==
                    upgradeCfg.txMaxFootprintEntries);
            REQUIRE(setting.contractLedgerCostExt().feeWrite1KB ==
                    upgradeCfg.feeFlatRateWrite1KB);
            break;
        case CONFIG_SETTING_SCP_TIMING:
            REQUIRE(
                setting.contractSCPTiming().ledgerTargetCloseTimeMilliseconds ==
                upgradeCfg.ledgerTargetCloseTimeMilliseconds);
            REQUIRE(setting.contractSCPTiming()
                        .nominationTimeoutInitialMilliseconds ==
                    upgradeCfg.nominationTimeoutInitialMilliseconds);
            REQUIRE(setting.contractSCPTiming()
                        .nominationTimeoutIncrementMilliseconds ==
                    upgradeCfg.nominationTimeoutIncrementMilliseconds);
            REQUIRE(
                setting.contractSCPTiming().ballotTimeoutInitialMilliseconds ==
                upgradeCfg.ballotTimeoutInitialMilliseconds);
            REQUIRE(setting.contractSCPTiming()
                        .ballotTimeoutIncrementMilliseconds ==
                    upgradeCfg.ballotTimeoutIncrementMilliseconds);
            break;
        default:
            REQUIRE(false);
            break;
        }
    }

    upgradeSorobanNetworkConfig(
        [&](SorobanNetworkConfig& cfg) {
            setSorobanNetworkConfigForTest(cfg);

            // Entries should never expire
            cfg.mStateArchivalSettings.maxEntryTTL = 2'000'000;
            cfg.mStateArchivalSettings.minPersistentTTL = 1'000'000;

            // Set write limits so that we can write all keys in a single TX
            // during setup
            cfg.mTxMaxWriteLedgerEntries = cfg.mTxMaxDiskReadEntries;
            cfg.mTxMaxWriteBytes = cfg.mTxMaxDiskReadBytes;

            // Allow every TX to have the maximum TX resources
            cfg.mLedgerMaxInstructions =
                cfg.mTxMaxInstructions * cfg.mLedgerMaxTxCount;
            cfg.mLedgerMaxDiskReadEntries =
                cfg.mTxMaxDiskReadEntries * cfg.mLedgerMaxTxCount;
            cfg.mLedgerMaxDiskReadBytes =
                cfg.mTxMaxDiskReadBytes * cfg.mLedgerMaxTxCount;
            cfg.mLedgerMaxWriteLedgerEntries =
                cfg.mTxMaxWriteLedgerEntries * cfg.mLedgerMaxTxCount;
            cfg.mLedgerMaxWriteBytes =
                cfg.mTxMaxWriteBytes * cfg.mLedgerMaxTxCount;
            cfg.mLedgerMaxTransactionsSizeBytes =
                cfg.mTxMaxSizeBytes * cfg.mLedgerMaxTxCount;
        },
        simulation);
    auto const numInstances = nAccounts;
    auto const numSorobanTxs = 150;

    numTxsBefore = getSuccessfulTxCount();

    loadGen.generateLoad(GeneratedLoadConfig::createSorobanInvokeSetupLoad(
        /* nAccounts */ nAccounts, numInstances,
        /* txRate */ 1));
    completeCount = complete.count();
    simulation->crankUntil(
        [&]() { return complete.count() == completeCount + 1; },
        100 * simulation->getExpectedLedgerCloseTime(), false);

    // Check that Soroban TXs were successfully applied
    for (auto node : nodes)
    {
        auto& txsSucceeded = node->getMetrics().NewCounter(
            {"ledger", "apply-soroban", "success"});
        auto& txsFailed = node->getMetrics().NewCounter(
            {"ledger", "apply-soroban", "failure"});

        // Should be 1 upload wasm TX followed by one instance deploy TX per
        // account
        REQUIRE(txsSucceeded.count() == numTxsBefore + numInstances + 1);
        REQUIRE(txsFailed.count() == 0);
    }

    numTxsBefore = getSuccessfulTxCount();

    auto invokeLoadCfg = GeneratedLoadConfig::txLoad(
        LoadGenMode::SOROBAN_INVOKE, nAccounts, numSorobanTxs,
        /* txRate */ 1);

    invokeLoadCfg.getMutSorobanConfig().nInstances = numInstances;
    invokeLoadCfg.setMinSorobanPercentSuccess(100);

    loadGen.generateLoad(invokeLoadCfg);
    completeCount = complete.count();
    simulation->crankUntil(
        [&]() { return complete.count() == completeCount + 1; },
        300 * simulation->getExpectedLedgerCloseTime(), false);

    // Check that Soroban TXs were successfully applied
    for (auto node : nodes)
    {
        auto& txsSucceeded = node->getMetrics().NewCounter(
            {"ledger", "apply-soroban", "success"});
        auto& txsFailed = node->getMetrics().NewCounter(
            {"ledger", "apply-soroban", "failure"});
        REQUIRE(txsSucceeded.count() == numTxsBefore + numSorobanTxs);
        REQUIRE(txsFailed.count() == 0);
    }

    auto instanceKeys = loadGen.getContractInstanceKeysForTesting();
    auto codeKeyOp = loadGen.getCodeKeyForTesting();
    REQUIRE(codeKeyOp);
    REQUIRE(codeKeyOp->type() == CONTRACT_CODE);
    REQUIRE(instanceKeys.size() == static_cast<size_t>(numInstances));

    // Check that each key is unique and exists in the DB
    // This ugly math mimics what we do in loadgen, where we calculate the total
    // number of bytes we can write, then divide the bytes between the number of
    // data entries we want to write and convert this value back to
    // kilobytes for the contract invocation. Thus we need to redundantly divide
    // then multiply by 1024 to mimic rounding behavior.
    auto expectedDataEntrySize =
        ((ioKiloBytes * 1024 - loadGen.getContactOverheadBytesForTesting()) /
         numDataEntries / 1024) *
        1024;

    UnorderedSet<LedgerKey> keys;
    for (auto const& instanceKey : instanceKeys)
    {
        REQUIRE(instanceKey.type() == CONTRACT_DATA);
        REQUIRE(instanceKey.contractData().key.type() ==
                SCV_LEDGER_KEY_CONTRACT_INSTANCE);
        REQUIRE(keys.find(instanceKey) == keys.end());
        keys.insert(instanceKey);

        auto const& contractID = instanceKey.contractData().contract;
        for (auto i = 0; i < numDataEntries; ++i)
        {
            auto lk = contractDataKey(contractID, txtest::makeU32(i),
                                      ContractDataDurability::PERSISTENT);

            LedgerTxn ltx(app.getLedgerTxnRoot());
            auto entry = ltx.load(lk);
            REQUIRE(entry);
            uint32_t sizeBytes =
                static_cast<uint32_t>(xdr::xdr_size(entry.current()));
            REQUIRE((sizeBytes > expectedDataEntrySize &&
                     sizeBytes < 100 + expectedDataEntrySize));

            REQUIRE(keys.find(lk) == keys.end());
            keys.insert(lk);
        }
    }

    // Test MIXED_CLASSIC_SOROBAN mode
    SECTION("Mix with classic")
    {
        constexpr uint32_t numMixedTxs = 200;
        auto mixLoadCfg = GeneratedLoadConfig::txLoad(
            LoadGenMode::MIXED_CLASSIC_SOROBAN, nAccounts, numMixedTxs,
            /* txRate */ 1);

        auto& mixCfg = mixLoadCfg.getMutMixClassicSorobanConfig();
        mixCfg.payWeight = 50;
        mixCfg.sorobanInvokeWeight = 45;
        constexpr uint32_t uploadWeight = 5;
        mixCfg.sorobanUploadWeight = uploadWeight;

        mixLoadCfg.setMinSorobanPercentSuccess(100);

        loadGen.generateLoad(mixLoadCfg);
        completeCount = complete.count();
        simulation->crankUntil(
            [&]() { return complete.count() == completeCount + 1; },
            300 * simulation->getExpectedLedgerCloseTime(), false);

        // Check results
        for (auto node : nodes)
        {
            auto& totalFailed =
                node->getMetrics().NewCounter({"ledger", "apply", "failure"});
            REQUIRE(totalFailed.count() == 0);
        }
    }
}

TEST_CASE("Multi-byte payment transactions are valid", "[loadgen]")
{
    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    uint32_t constexpr baseSize = 148;
    uint32_t constexpr opSize = 56;
    uint32_t constexpr frameSize = baseSize + opSize * 3;
    Simulation::pointer simulation =
        Topologies::pair(Simulation::OVER_LOOPBACK, networkID, [](int i) {
            auto cfg = getTestConfig(i);
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
        2 * simulation->getExpectedLedgerCloseTime(), false);

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
            15 * simulation->getExpectedLedgerCloseTime(), false);
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

TEST_CASE("Upgrade setup with metrics reset", "[loadgen]")
{
    // Create a simulation with two nodes
    Simulation::pointer sim = Topologies::pair(
        Simulation::OVER_LOOPBACK, sha256(getTestConfig().NETWORK_PASSPHRASE),
        [&](int i) {
            auto cfg = getTestConfig(i);
            cfg.ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING = true;
            cfg.GENESIS_TEST_ACCOUNT_COUNT = 1; // Create account at genesis
            return cfg;
        });
    sim->startAllNodes();
    sim->crankUntil([&]() { return sim->haveAllExternalized(3, 1); },
                    2 * sim->getExpectedLedgerCloseTime(), false);

    Application::pointer app = sim->getNodes().front();
    LoadGenerator& loadgen = app->getLoadGenerator();
    medida::Meter& runsComplete =
        app->getMetrics().NewMeter({"loadgen", "run", "complete"}, "run");
    medida::Meter& runsFailed =
        app->getMetrics().NewMeter({"loadgen", "run", "failed"}, "run");

    // Clear metrics to reset run count
    app->clearMetrics("");

    // Setup a soroban limit upgrade that must succeed
    GeneratedLoadConfig upgradeSetupCfg =
        GeneratedLoadConfig::createSorobanUpgradeSetupLoad();
    upgradeSetupCfg.setMinSorobanPercentSuccess(100);
    loadgen.generateLoad(upgradeSetupCfg);
    sim->crankUntil([&]() { return runsComplete.count() == 1; },
                    5 * sim->getExpectedLedgerCloseTime(), false);
    REQUIRE(runsFailed.count() == 0);

    // Clear metrics again to reset run count
    app->clearMetrics("");

    // Setup again. This should succeed even though it's the same account with
    // the same `runsComplete` value performing the setup
    loadgen.generateLoad(upgradeSetupCfg);
    sim->crankUntil([&]() { return runsComplete.count() == 1; },
                    5 * sim->getExpectedLedgerCloseTime(), false);
    REQUIRE(runsFailed.count() == 0);
}

TEST_CASE("apply load", "[loadgen][applyload][acceptance]")
{
    auto cfg = getTestConfig();
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 1000;
    cfg.USE_CONFIG_FOR_GENESIS = true;
    cfg.LEDGER_PROTOCOL_VERSION = Config::CURRENT_LEDGER_PROTOCOL_VERSION;
    cfg.MANUAL_CLOSE = true;
    cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS = false;

    cfg.APPLY_LOAD_CLASSIC_TXS_PER_LEDGER = 100;

    cfg.APPLY_LOAD_DATA_ENTRY_SIZE = 1000;

    // BL generation parameters
    cfg.APPLY_LOAD_BL_SIMULATED_LEDGERS = 10000;
    cfg.APPLY_LOAD_BL_WRITE_FREQUENCY = 1000;
    cfg.APPLY_LOAD_BL_BATCH_SIZE = 1000;
    cfg.APPLY_LOAD_BL_LAST_BATCH_LEDGERS = 300;
    cfg.APPLY_LOAD_BL_LAST_BATCH_SIZE = 100;

    // Load generation parameters
    cfg.APPLY_LOAD_NUM_DISK_READ_ENTRIES = {0, 1, 2};
    cfg.APPLY_LOAD_NUM_DISK_READ_ENTRIES_DISTRIBUTION = {3, 2, 1};

    cfg.APPLY_LOAD_NUM_RW_ENTRIES = {1, 5, 10};
    cfg.APPLY_LOAD_NUM_RW_ENTRIES_DISTRIBUTION = {1, 1, 1};

    cfg.APPLY_LOAD_EVENT_COUNT = {100};
    cfg.APPLY_LOAD_EVENT_COUNT_DISTRIBUTION = {1};

    cfg.APPLY_LOAD_TX_SIZE_BYTES = {1'000, 2'000, 5'000};
    cfg.APPLY_LOAD_TX_SIZE_BYTES_DISTRIBUTION = {3, 2, 1};

    cfg.APPLY_LOAD_INSTRUCTIONS = {10'000'000, 50'000'000};
    cfg.APPLY_LOAD_INSTRUCTIONS_DISTRIBUTION = {5, 1};

    // Ledger and transaction limits
    cfg.APPLY_LOAD_LEDGER_MAX_INSTRUCTIONS = 500'000'000;
    cfg.APPLY_LOAD_TX_MAX_INSTRUCTIONS = 100'000'000;
    cfg.APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS = 2;

    cfg.APPLY_LOAD_LEDGER_MAX_DISK_READ_LEDGER_ENTRIES = 2000;
    cfg.APPLY_LOAD_TX_MAX_DISK_READ_LEDGER_ENTRIES = 100;
    cfg.APPLY_LOAD_TX_MAX_FOOTPRINT_SIZE = 100;

    cfg.APPLY_LOAD_LEDGER_MAX_DISK_READ_BYTES = 50'000'000;
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

    ApplyLoad al(*app, ApplyLoadMode::LIMIT_BASED);

    // Sample a few indices to verify hot archive is properly initialized
    uint32_t expectedArchivedEntries =
        ApplyLoad::calculateRequiredHotArchiveEntries(
            ApplyLoadMode::LIMIT_BASED, cfg);
    std::vector<uint32_t> sampleIndices = {0, expectedArchivedEntries / 2,
                                           expectedArchivedEntries - 1};
    std::set<LedgerKey, LedgerEntryIdCmp> sampleKeys;

    auto hotArchive = app->getBucketManager()
                          .getBucketSnapshotManager()
                          .copySearchableHotArchiveBucketListSnapshot();

    for (auto idx : sampleIndices)
    {
        sampleKeys.insert(ApplyLoad::getKeyForArchivedEntry(idx));
    }

    auto sampleEntries = hotArchive->loadKeys(sampleKeys);
    REQUIRE(sampleEntries.size() == sampleKeys.size());

    al.execute();

    REQUIRE(1.0 - al.successRate() < std::numeric_limits<double>::epsilon());
}

TEST_CASE("apply load find max limits for model tx",
          "[loadgen][applyload][acceptance]")
{
    auto cfg = getTestConfig();
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 1000;
    cfg.USE_CONFIG_FOR_GENESIS = true;
    cfg.LEDGER_PROTOCOL_VERSION = Config::CURRENT_LEDGER_PROTOCOL_VERSION;
    cfg.MANUAL_CLOSE = true;
    cfg.ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING = true;

    // Also generate that many classic simple payments.
    cfg.APPLY_LOAD_CLASSIC_TXS_PER_LEDGER = 100;

    // Close 30 ledgers per iteration.
    cfg.APPLY_LOAD_NUM_LEDGERS = 30;
    // The target close time is 500ms.
    cfg.APPLY_LOAD_TARGET_CLOSE_TIME_MS = 500;

    // Size of each data entry to be used in the test.
    cfg.APPLY_LOAD_DATA_ENTRY_SIZE = 100;

    // BL generation parameters
    cfg.APPLY_LOAD_BL_SIMULATED_LEDGERS = 1000;
    cfg.APPLY_LOAD_BL_WRITE_FREQUENCY = 1000;
    cfg.APPLY_LOAD_BL_BATCH_SIZE = 1000;
    cfg.APPLY_LOAD_BL_LAST_BATCH_LEDGERS = 300;
    cfg.APPLY_LOAD_BL_LAST_BATCH_SIZE = 100;

    // Load generation parameters
    cfg.APPLY_LOAD_NUM_DISK_READ_ENTRIES = {1};
    cfg.APPLY_LOAD_NUM_DISK_READ_ENTRIES_DISTRIBUTION = {1};

    cfg.APPLY_LOAD_NUM_RW_ENTRIES = {4};
    cfg.APPLY_LOAD_NUM_RW_ENTRIES_DISTRIBUTION = {1};

    cfg.APPLY_LOAD_EVENT_COUNT = {2};
    cfg.APPLY_LOAD_EVENT_COUNT_DISTRIBUTION = {1};

    cfg.APPLY_LOAD_TX_SIZE_BYTES = {1000};
    cfg.APPLY_LOAD_TX_SIZE_BYTES_DISTRIBUTION = {1};

    cfg.APPLY_LOAD_INSTRUCTIONS = {2'000'000};
    cfg.APPLY_LOAD_INSTRUCTIONS_DISTRIBUTION = {1};

    // Only a few ledger limits need to be specified, the rest will be found by
    // the benchmark itself.
    // Number of soroban txs per ledger is the upper bound of the binary
    // search for the number of the model txs to include in each ledger.
    cfg.APPLY_LOAD_MAX_SOROBAN_TX_COUNT = 1000;
    // Use 2 clusters/threads.
    cfg.APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS = 2;

    VirtualClock clock(VirtualClock::REAL_TIME);
    auto app = createTestApplication(clock, cfg);

    ApplyLoad al(*app, ApplyLoadMode::FIND_LIMITS_FOR_MODEL_TX);

    al.execute();

    REQUIRE(1.0 - al.successRate() < std::numeric_limits<double>::epsilon());
}

TEST_CASE("apply load find max SAC TPS",
          "[loadgen][applyload][soroban][acceptance]")
{
    auto cfg = getTestConfig();
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 1000;
    cfg.USE_CONFIG_FOR_GENESIS = true;
    cfg.LEDGER_PROTOCOL_VERSION = Config::CURRENT_LEDGER_PROTOCOL_VERSION;
    cfg.MANUAL_CLOSE = true;
    cfg.IGNORE_MESSAGE_LIMITS_FOR_TESTING = true;

    // Configure test parameters for MAX_SAC_TPS mode
    cfg.APPLY_LOAD_TARGET_CLOSE_TIME_MS = 1500;
    cfg.APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS = 2;
    cfg.APPLY_LOAD_MAX_SAC_TPS_MIN_TPS = 1;
    cfg.APPLY_LOAD_MAX_SAC_TPS_MAX_TPS = 1500;
    cfg.APPLY_LOAD_NUM_LEDGERS = 30;
    cfg.APPLY_LOAD_BATCH_SAC_COUNT = 2;

    VirtualClock clock(VirtualClock::REAL_TIME);
    auto app = createTestApplication(clock, cfg);

    ApplyLoad al(*app, ApplyLoadMode::MAX_SAC_TPS);

    // Run the MAX_SAC_TPS test
    al.execute();

    // Verify that we actually applied something in parallel
    auto& maxClustersMetric = app->getMetrics().NewCounter(
        {"ledger", "apply-soroban", "max-clusters"});
    auto& successCountMetric =
        app->getMetrics().NewCounter({"ledger", "apply-soroban", "success"});

    REQUIRE(maxClustersMetric.count() ==
            cfg.APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS);
    REQUIRE(successCountMetric.count() > 200);
}

TEST_CASE("noisy binary search", "[applyload]")
{
    std::mt19937 rng(12345); // Fixed seed for reproducibility

    // Helper to create a noisy monotone function with normally distributed
    // noise.
    // meanFunc: function that computes the true mean at x
    // stddevFunc: function that computes the standard deviation at x
    auto makeNoisyMonotone = [&rng](std::function<double(uint32_t)> meanFunc,
                                    std::function<double(uint32_t)> stddevFunc)
        -> std::function<double(uint32_t)> {
        return [&rng, meanFunc, stddevFunc](uint32_t x) {
            double mean = meanFunc(x);
            double stddev = stddevFunc(x);
            std::normal_distribution<double> dist(mean, stddev);
            return dist(rng);
        };
    };

    // Mean functions: linear, sqrt, x^2
    auto linearMean = [](uint32_t x) { return static_cast<double>(x); };
    auto sqrtMean = [](uint32_t x) {
        return std::sqrt(static_cast<double>(x));
    };
    auto quadraticMean = [](uint32_t x) {
        return static_cast<double>(x) * static_cast<double>(x);
    };

    // Variance functions: constant, proportional to x
    auto constStddev = [](uint32_t) { return 50.0; };
    auto proportionalStddev = [](uint32_t x) {
        return std::max(1.0, static_cast<double>(x) * 0.1);
    };

    double const confidence = 0.99;
    size_t const maxSamples = 1000;

    SECTION("linear mean, constant variance")
    {
        // Search for x where E[f(x)] = target
        uint32_t const trueX = 500;
        double const target = linearMean(trueX);
        uint32_t const xMin = 100;
        uint32_t const xMax = 1000;
        uint32_t const tolerance = 50;

        auto f = makeNoisyMonotone(linearMean, constStddev);
        auto [lo, hi] = noisyBinarySearch(f, target, xMin, xMax, confidence,
                                          tolerance, maxSamples);

        REQUIRE(lo <= trueX);
        REQUIRE(hi >= trueX);
        REQUIRE(hi - lo <= tolerance);
    }

    SECTION("linear mean, proportional variance")
    {
        uint32_t const trueX = 750;
        double const target = linearMean(trueX);
        uint32_t const xMin = 100;
        uint32_t const xMax = 1000;
        uint32_t const tolerance = 200;

        auto f = makeNoisyMonotone(linearMean, proportionalStddev);
        auto [lo, hi] = noisyBinarySearch(f, target, xMin, xMax, confidence,
                                          tolerance, maxSamples);

        REQUIRE(lo <= trueX);
        REQUIRE(hi >= trueX);
        REQUIRE(hi - lo <= tolerance);
    }

    SECTION("sqrt mean, constant variance")
    {
        uint32_t const trueX = 2500;
        double const target = sqrtMean(trueX); // sqrt(2500) = 50
        uint32_t const xMin = 100;
        uint32_t const xMax = 10000;
        uint32_t const tolerance = 100;

        // Use smaller stddev relative to the signal range (10..100)
        auto smallConstStddev = [](uint32_t) { return 2.0; };
        auto f = makeNoisyMonotone(sqrtMean, smallConstStddev);
        auto [lo, hi] = noisyBinarySearch(f, target, xMin, xMax, confidence,
                                          tolerance, maxSamples);

        REQUIRE(lo <= trueX);
        REQUIRE(hi >= trueX);
        REQUIRE(hi - lo <= tolerance);
    }

    SECTION("sqrt mean, proportional variance")
    {
        uint32_t const trueX = 4000;
        double const target = sqrtMean(trueX);
        uint32_t const xMin = 500;
        uint32_t const xMax = 10000;
        uint32_t const tolerance = 100;

        auto smallProportionalStddev = [](uint32_t x) {
            return std::max(1.0, std::sqrt(static_cast<double>(x)) * 0.05);
        };
        auto f = makeNoisyMonotone(sqrtMean, smallProportionalStddev);
        auto [lo, hi] = noisyBinarySearch(f, target, xMin, xMax, confidence,
                                          tolerance, maxSamples);

        REQUIRE(lo <= trueX);
        REQUIRE(hi >= trueX);
        REQUIRE(hi - lo <= tolerance);
    }

    SECTION("quadratic mean, constant variance")
    {
        uint32_t const trueX = 100;
        double const target = quadraticMean(trueX); // 100^2 = 10000
        uint32_t const xMin = 10;
        uint32_t const xMax = 500;
        uint32_t const tolerance = 20;

        auto f = makeNoisyMonotone(quadraticMean, constStddev);
        auto [lo, hi] = noisyBinarySearch(f, target, xMin, xMax, confidence,
                                          tolerance, maxSamples);

        REQUIRE(lo <= trueX);
        REQUIRE(hi >= trueX);
        REQUIRE(hi - lo <= tolerance);
    }

    SECTION("quadratic mean, proportional variance")
    {
        uint32_t const trueX = 200;
        double const target = quadraticMean(trueX);
        uint32_t const xMin = 50;
        uint32_t const xMax = 500;
        uint32_t const tolerance = 50;

        auto f = makeNoisyMonotone(quadraticMean, proportionalStddev);
        auto [lo, hi] = noisyBinarySearch(f, target, xMin, xMax, confidence,
                                          tolerance, maxSamples);

        REQUIRE(lo <= trueX);
        REQUIRE(hi >= trueX);
        REQUIRE(hi - lo <= tolerance);
    }

    SECTION("narrow search range")
    {
        uint32_t const trueX = 55;
        double const target = linearMean(trueX);
        uint32_t const xMin = 50;
        uint32_t const xMax = 60;
        uint32_t const tolerance = 10;

        auto f = makeNoisyMonotone(linearMean, constStddev);
        auto [lo, hi] = noisyBinarySearch(f, target, xMin, xMax, confidence,
                                          tolerance, maxSamples);

        REQUIRE(lo <= trueX);
        REQUIRE(hi >= trueX);
        REQUIRE(lo >= xMin);
        REQUIRE(hi <= xMax);
    }

    SECTION("tight tolerance")
    {
        uint32_t const trueX = 500;
        double const target = linearMean(trueX);
        uint32_t const xMin = 100;
        uint32_t const xMax = 1000;
        uint32_t const tolerance = 10;

        // Use lower noise for tight tolerance test
        auto lowNoiseStddev = [](uint32_t) { return 1.0; };
        auto f = makeNoisyMonotone(linearMean, lowNoiseStddev);
        auto [lo, hi] = noisyBinarySearch(f, target, xMin, xMax, confidence,
                                          tolerance, maxSamples);

        REQUIRE(lo <= trueX);
        REQUIRE(hi >= trueX);
        REQUIRE(hi - lo <= tolerance);
    }

    SECTION("target at boundary - near min")
    {
        uint32_t const trueX = 110;
        double const target = linearMean(trueX);
        uint32_t const xMin = 100;
        uint32_t const xMax = 1000;
        uint32_t const tolerance = 50;

        auto f = makeNoisyMonotone(linearMean, constStddev);
        auto [lo, hi] = noisyBinarySearch(f, target, xMin, xMax, confidence,
                                          tolerance, maxSamples);

        REQUIRE(lo <= trueX);
        REQUIRE(hi >= trueX);
        REQUIRE(lo >= xMin);
    }

    SECTION("target at boundary - near max")
    {
        uint32_t const trueX = 950;
        double const target = linearMean(trueX);
        uint32_t const xMin = 100;
        uint32_t const xMax = 1000;
        uint32_t const tolerance = 100;

        auto f = makeNoisyMonotone(linearMean, constStddev);
        auto [lo, hi] = noisyBinarySearch(f, target, xMin, xMax, confidence,
                                          tolerance, maxSamples);

        REQUIRE(lo <= trueX);
        REQUIRE(hi >= trueX);
        REQUIRE(hi <= xMax);
    }

    SECTION("single point range")
    {
        uint32_t const xMin = 500;
        uint32_t const xMax = 500;
        double const target = linearMean(xMin);
        uint32_t const tolerance = 0;

        auto f = makeNoisyMonotone(linearMean, constStddev);
        auto [lo, hi] = noisyBinarySearch(f, target, xMin, xMax, confidence,
                                          tolerance, maxSamples);

        REQUIRE(lo == xMin);
        REQUIRE(hi == xMax);
    }
    SECTION("benchmark-like: tx count to execution time")
    {
        auto benchmarkMean = [](uint32_t x) {
            double xd = static_cast<double>(x);
            // Model partially parallel execution with sequential stages that
            // scale up slowly as x increases.
            return 10.0 + 0.1 * xd + 0.5 * std::sqrt(xd);
        };

        // Variance is proportional to apply time (mean).
        auto benchmarkStddev = [&](uint32_t x) {
            return 0.01 * benchmarkMean(x);
        };

        uint32_t const trueX = 3500;
        uint32_t const targetTimeMs = benchmarkMean(trueX);
        double const target = static_cast<double>(targetTimeMs);

        uint32_t const xMin = 1000;
        uint32_t const xMax = 10000;
        uint32_t const tolerance = 200;

        auto f = makeNoisyMonotone(benchmarkMean, benchmarkStddev);
        auto [lo, hi] = noisyBinarySearch(f, target, xMin, xMax, confidence,
                                          tolerance, maxSamples);

        // Verify the interval contains the true value
        REQUIRE(lo <= trueX);
        REQUIRE(hi >= trueX);
        REQUIRE(hi - lo <= tolerance);

        // Also verify the mean at the found interval is close to target
        double loTime = benchmarkMean(lo);
        double hiTime = benchmarkMean(hi);
        REQUIRE(loTime <= target + 50);
        REQUIRE(hiTime >= target - 50);
    }
    SECTION("randomized test")
    {
        // Test many random combinations of parameters
        std::mt19937 paramRng(42); // Different seed for parameter generation

        // Mean function types
        enum class MeanType
        {
            LINEAR,
            SQRT,
            QUADRATIC,
            LOG
        };

        // Variance function types
        enum class VarianceType
        {
            CONSTANT,
            PROPORTIONAL
        };

        auto getMeanFunc =
            [](MeanType type) -> std::function<double(uint32_t)> {
            switch (type)
            {
            case MeanType::LINEAR:
                return [](uint32_t x) { return static_cast<double>(x); };
            case MeanType::SQRT:
                return [](uint32_t x) {
                    return std::sqrt(static_cast<double>(x)) * 100.0;
                };
            case MeanType::QUADRATIC:
                return [](uint32_t x) {
                    return static_cast<double>(x) * static_cast<double>(x) /
                           1000.0;
                };
            case MeanType::LOG:
                return [](uint32_t x) {
                    return std::log(static_cast<double>(x) + 1.0) * 100.0;
                };
            default:
                return [](uint32_t x) { return static_cast<double>(x); };
            }
        };

        // Stddev functions based on the mean value, not x
        // This gives us direct control over signal-to-noise ratio
        auto getStddevFunc = [](VarianceType type, double noiseRatio,
                                std::function<double(uint32_t)> const& meanFunc)
            -> std::function<double(uint32_t)> {
            switch (type)
            {
            case VarianceType::CONSTANT:
                // Constant stddev as a fraction of the mean at x
                return [noiseRatio, meanFunc](uint32_t x) {
                    return std::max(0.1, std::abs(meanFunc(x)) * noiseRatio);
                };
            case VarianceType::PROPORTIONAL:
                // Stddev proportional to sqrt of mean (like Poisson-ish)
                return [noiseRatio, meanFunc](uint32_t x) {
                    double m = std::abs(meanFunc(x));
                    return std::max(0.1, std::sqrt(m) * noiseRatio);
                };
            default:
                return [](uint32_t) { return 1.0; };
            }
        };

        size_t const numTests = 200;
        size_t passed = 0;

        for (size_t testIdx = 0; testIdx < numTests; ++testIdx)
        {
            // Generate random parameters within sane bounds
            MeanType meanType = static_cast<MeanType>(
                stellar::uniform_int_distribution<int>(0, 3)(paramRng));
            VarianceType varType = static_cast<VarianceType>(
                stellar::uniform_int_distribution<int>(0, 1)(paramRng));

            // Range parameters
            uint32_t xMin =
                stellar::uniform_int_distribution<uint32_t>(10, 500)(paramRng);
            uint32_t rangeSize = stellar::uniform_int_distribution<uint32_t>(
                100, 5000)(paramRng);
            uint32_t xMax = xMin + rangeSize;

            // True x* somewhere in the range (not too close to edges)
            uint32_t margin = rangeSize / 10;
            uint32_t trueX = stellar::uniform_int_distribution<uint32_t>(
                xMin + margin, xMax - margin)(paramRng);

            // Get the mean function and compute target
            auto meanFunc = getMeanFunc(meanType);
            double target = meanFunc(trueX);

            // Tolerance: between 1% and 20% of range
            uint32_t tolerance = stellar::uniform_int_distribution<uint32_t>(
                rangeSize / 100 + 1, rangeSize / 5 + 1)(paramRng);

            // Confidence level: 0.90 to 0.99
            double testConfidence =
                std::uniform_real_distribution<double>(0.90, 0.99)(paramRng);

            // Noise ratio: stddev as fraction of mean, kept reasonable (1-10%)
            double noiseRatio =
                std::uniform_real_distribution<double>(0.01, 0.10)(paramRng);

            auto stddevFunc = getStddevFunc(varType, noiseRatio, meanFunc);

            // Create the noisy function with a fresh RNG for each test
            std::mt19937 testRng(testIdx * 1000 + 12345);
            auto noisyFunc = [&testRng, meanFunc,
                              stddevFunc](uint32_t x) -> double {
                double mean = meanFunc(x);
                double stddev = stddevFunc(x);
                std::normal_distribution<double> dist(mean, stddev);
                return dist(testRng);
            };

            // Run the search
            auto [lo, hi] =
                noisyBinarySearch(noisyFunc, target, xMin, xMax, testConfidence,
                                  tolerance, maxSamples);

            // Check if the result is valid
            bool containsTrueX = (lo <= trueX && hi >= trueX);
            bool withinTolerance = (hi - lo <= tolerance);
            bool withinBounds = (lo >= xMin && hi <= xMax);

            if (containsTrueX && withinTolerance && withinBounds)
            {
                passed++;
            }
        }

        // We expect at least 90% of tests to pass
        double passRate = static_cast<double>(passed) / numTests;
        INFO("Passed " << passed << "/" << numTests << " tests ("
                       << (passRate * 100) << "%)");
        REQUIRE(passRate >= 0.90);
    }
}
