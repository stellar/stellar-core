// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SHA.h"
#include "crypto/SecretKey.h"
#include "ledger/LedgerManager.h"
#include "lib/catch.hpp"
#include "main/Config.h"
#include "scp/QuorumSetUtils.h"
#include "simulation/ApplyLoad.h"
#include "simulation/LoadGenerator.h"
#include "simulation/Topologies.h"
#include "test/test.h"
#include "transactions/test/SorobanTxTestUtils.h"
#include "util/Math.h"
#include <fmt/format.h>

using namespace stellar;

TEST_CASE("generate load in protocol 1")
{
    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    Simulation::pointer simulation =
        Topologies::pair(Simulation::OVER_LOOPBACK, networkID, [](int i) {
            auto cfg = getTestConfig(i);
            cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 5000;
            cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION = 1;
            return cfg;
        });

    simulation->startAllNodes();
    simulation->crankUntil(
        [&]() { return simulation->haveAllExternalized(3, 1); },
        2 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

    auto nodes = simulation->getNodes();
    auto& app = *nodes[0]; // pick a node to generate load

    auto& loadGen = app.getLoadGenerator();
    loadGen.generateLoad(GeneratedLoadConfig::createAccountsLoad(
        /* nAccounts */ 10000,
        /* txRate */ 1));
    simulation->crankUntil(
        [&]() {
            return app.getMetrics()
                       .NewMeter({"loadgen", "run", "complete"}, "run")
                       .count() == 1;
        },
        100 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);
}

TEST_CASE("generate load with unique accounts", "[loadgen]")
{
    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    Simulation::pointer simulation =
        Topologies::pair(Simulation::OVER_LOOPBACK, networkID, [](int i) {
            auto cfg = getTestConfig(i);
            cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 5000;
            cfg.LOADGEN_OP_COUNT_FOR_TESTING = {1, 2, 10};
            cfg.LOADGEN_OP_COUNT_DISTRIBUTION_FOR_TESTING = {80, 19, 1};
            return cfg;
        });

    simulation->startAllNodes();
    simulation->crankUntil(
        [&]() { return simulation->haveAllExternalized(3, 1); },
        2 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

    auto nodes = simulation->getNodes();
    auto& app = *nodes[0]; // pick a node to generate load

    auto& loadGen = app.getLoadGenerator();

    auto getSuccessfulTxCount = [&]() {
        return nodes[0]
            ->getMetrics()
            .NewCounter({"ledger", "apply", "success"})
            .count();
    };

    SECTION("success")
    {
        uint32_t const nAccounts = 1000;
        uint32_t const nAccountCreationTxs = nAccounts / 100;
        uint32_t const nTxs = 10000;

        loadGen.generateLoad(GeneratedLoadConfig::createAccountsLoad(
            /* nAccounts */ nAccounts,
            /* txRate */ 1));
        simulation->crankUntil(
            [&]() {
                return app.getMetrics()
                           .NewMeter({"loadgen", "run", "complete"}, "run")
                           .count() == 1;
            },
            100 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);
        REQUIRE(getSuccessfulTxCount() == nAccountCreationTxs);

        loadGen.generateLoad(GeneratedLoadConfig::txLoad(LoadGenMode::PAY,
                                                         nAccounts, nTxs,
                                                         /* txRate */ 50));
        simulation->crankUntil(
            [&]() {
                return app.getMetrics()
                           .NewMeter({"loadgen", "run", "complete"}, "run")
                           .count() == 2;
            },
            300 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);
        REQUIRE(getSuccessfulTxCount() == nAccountCreationTxs + nTxs);
    }
    SECTION("invalid loadgen parameters")
    {
        // Succesfully create accounts
        uint32 numAccounts = 100;
        loadGen.generateLoad(GeneratedLoadConfig::createAccountsLoad(
            /* nAccounts */ 100,
            /* txRate */ 1));
        simulation->crankUntil(
            [&]() {
                return app.getMetrics()
                           .NewMeter({"loadgen", "run", "complete"}, "run")
                           .count() == 1;
            },
            100 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

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
            10 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);
    }
    SECTION("stop loadgen")
    {
        loadGen.generateLoad(GeneratedLoadConfig::createAccountsLoad(
            /* nAccounts */ 10000,
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
            return cfg;
        });

    simulation->startAllNodes();
    simulation->crankUntil(
        [&]() { return simulation->haveAllExternalized(3, 1); },
        2 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);
    auto nodes = simulation->getNodes();
    auto& app = *nodes[0]; // pick a node to generate load

    const uint32_t ledgerMaxTxCount = 42;
    const uint32_t bucketListSizeWindowSampleSize = 99;
    // Upgrade the network config.
    upgradeSorobanNetworkConfig(
        [&](SorobanNetworkConfig& cfg) {
            cfg.mLedgerMaxTxCount = ledgerMaxTxCount;
            cfg.stateArchivalSettings().bucketListSizeWindowSampleSize =
                bucketListSizeWindowSampleSize;
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
                .bucketListSizeWindowSampleSize ==
            bucketListSizeWindowSampleSize);
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
        2 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

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
    loadGen.generateLoad(GeneratedLoadConfig::createAccountsLoad(
        /* nAccounts */ nAccounts,
        /* txRate */ 1));
    auto& complete =
        app.getMetrics().NewMeter({"loadgen", "run", "complete"}, "run");
    auto completeCount = complete.count();
    simulation->crankUntil(
        [&]() { return complete.count() == completeCount + 1; },
        100 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

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
        100 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

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
    upgradeCfg.ledgerMaxReadLedgerEntries =
        rand_uniform<uint32_t>(UINT32_MAX - 10'000, UINT32_MAX);
    upgradeCfg.ledgerMaxReadBytes =
        rand_uniform<uint32_t>(UINT32_MAX - 10'000, UINT32_MAX);
    upgradeCfg.ledgerMaxWriteLedgerEntries =
        rand_uniform<uint32_t>(UINT32_MAX - 10'000, UINT32_MAX);
    upgradeCfg.ledgerMaxWriteBytes =
        rand_uniform<uint32_t>(UINT32_MAX - 10'000, UINT32_MAX);
    upgradeCfg.ledgerMaxTxCount =
        rand_uniform<uint32_t>(UINT32_MAX - 10'000, UINT32_MAX);
    upgradeCfg.txMaxReadLedgerEntries =
        rand_uniform<uint32_t>(UINT32_MAX - 10'000, UINT32_MAX);
    upgradeCfg.txMaxReadBytes =
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
    upgradeCfg.bucketListSizeWindowSampleSize =
        rand_uniform<uint32_t>(UINT32_MAX - 10'000, UINT32_MAX);
    upgradeCfg.evictionScanSize =
        rand_uniform<int64_t>(INT64_MAX - 10'000, INT64_MAX);
    upgradeCfg.startingEvictionScanLevel = rand_uniform<uint32_t>(4, 8);

    auto upgradeSetKey = loadGen.getConfigUpgradeSetKey(
        createUpgradeLoadGenConfig.getSorobanUpgradeConfig());

    numTxsBefore = getSuccessfulTxCount();
    loadGen.generateLoad(createUpgradeLoadGenConfig);
    completeCount = complete.count();
    simulation->crankUntil(
        [&]() { return complete.count() == completeCount + 1; },
        300 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

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

    // Loadgen doesn't update the cost types.
    REQUIRE(upgrades.updatedEntry.size() == 10);
    for (auto const& setting : upgrades.updatedEntry)
    {
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
            REQUIRE(setting.contractLedgerCost().ledgerMaxReadLedgerEntries ==
                    upgradeCfg.ledgerMaxReadLedgerEntries);
            REQUIRE(setting.contractLedgerCost().ledgerMaxReadBytes ==
                    upgradeCfg.ledgerMaxReadBytes);
            REQUIRE(setting.contractLedgerCost().ledgerMaxWriteLedgerEntries ==
                    upgradeCfg.ledgerMaxWriteLedgerEntries);
            REQUIRE(setting.contractLedgerCost().ledgerMaxWriteBytes ==
                    upgradeCfg.ledgerMaxWriteBytes);
            REQUIRE(setting.contractLedgerCost().txMaxReadLedgerEntries ==
                    upgradeCfg.txMaxReadLedgerEntries);
            REQUIRE(setting.contractLedgerCost().txMaxReadBytes ==
                    upgradeCfg.txMaxReadBytes);
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
            REQUIRE(ses.bucketListSizeWindowSampleSize ==
                    upgradeCfg.bucketListSizeWindowSampleSize);
            REQUIRE(ses.evictionScanSize == upgradeCfg.evictionScanSize);
            REQUIRE(ses.startingEvictionScanLevel ==
                    upgradeCfg.startingEvictionScanLevel);
        }
        break;
        case CONFIG_SETTING_CONTRACT_EXECUTION_LANES:
            REQUIRE(setting.contractExecutionLanes().ledgerMaxTxCount ==
                    upgradeCfg.ledgerMaxTxCount);
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
            cfg.mTxMaxWriteLedgerEntries = cfg.mTxMaxReadLedgerEntries;
            cfg.mTxMaxWriteBytes = cfg.mTxMaxReadBytes;

            // Allow every TX to have the maximum TX resources
            cfg.mLedgerMaxInstructions =
                cfg.mTxMaxInstructions * cfg.mLedgerMaxTxCount;
            cfg.mLedgerMaxReadLedgerEntries =
                cfg.mTxMaxReadLedgerEntries * cfg.mLedgerMaxTxCount;
            cfg.mLedgerMaxReadBytes =
                cfg.mTxMaxReadBytes * cfg.mLedgerMaxTxCount;
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
        100 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

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
        300 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

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
        auto numSuccessBefore = getSuccessfulTxCount();
        auto numFailedBefore =
            app.getMetrics()
                .NewCounter({"ledger", "apply-soroban", "failure"})
                .count();
        completeCount = complete.count();
        simulation->crankUntil(
            [&]() { return complete.count() == completeCount + 1; },
            300 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

        // Check results
        for (auto node : nodes)
        {
            auto& totalFailed =
                node->getMetrics().NewCounter({"ledger", "apply", "failure"});
            REQUIRE(totalFailed.count() == 0);
        }
    }
}

TEST_CASE("Multi-op pretend transactions are valid", "[loadgen]")
{
    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    Simulation::pointer simulation =
        Topologies::pair(Simulation::OVER_LOOPBACK, networkID, [](int i) {
            auto cfg = getTestConfig(i);
            // 50% of transactions contain 2 ops,
            // and 50% of transactions contain 3 ops.
            cfg.LOADGEN_OP_COUNT_FOR_TESTING = {2, 3};
            cfg.LOADGEN_OP_COUNT_DISTRIBUTION_FOR_TESTING = {1, 1};
            return cfg;
        });

    simulation->startAllNodes();
    simulation->crankUntil(
        [&]() { return simulation->haveAllExternalized(3, 1); },
        2 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

    auto nodes = simulation->getNodes();
    auto& app = *nodes[0]; // pick a node to generate load

    auto& loadGen = app.getLoadGenerator();
    uint32_t nAccounts = 5;
    uint32_t txRate = 5;

    loadGen.generateLoad(GeneratedLoadConfig::createAccountsLoad(
        /* nAccounts */ nAccounts,
        /* txRate */ txRate));
    try
    {
        simulation->crankUntil(
            [&]() {
                return app.getMetrics()
                           .NewMeter({"loadgen", "run", "complete"}, "run")
                           .count() == 1;
            },
            3 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

        loadGen.generateLoad(GeneratedLoadConfig::txLoad(LoadGenMode::PRETEND,
                                                         nAccounts, 5, txRate));

        simulation->crankUntil(
            [&]() {
                return app.getMetrics()
                           .NewMeter({"loadgen", "run", "complete"}, "run")
                           .count() == 2;
            },
            2 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);
    }
    catch (...)
    {
        auto problems = loadGen.checkAccountSynced(app, false);
        REQUIRE(problems.empty());
    }

    REQUIRE(app.getMetrics()
                .NewMeter({"loadgen", "txn", "rejected"}, "txn")
                .count() == 0);
    REQUIRE(app.getMetrics()
                .NewMeter({"loadgen", "account", "created"}, "account")
                .count() == nAccounts);
    REQUIRE(app.getMetrics()
                .NewMeter({"loadgen", "payment", "submitted"}, "op")
                .count() == 0);
    REQUIRE(app.getMetrics()
                .NewMeter({"loadgen", "pretend", "submitted"}, "op")
                .count() >= 2 * 5);
    REQUIRE(app.getMetrics()
                .NewMeter({"loadgen", "pretend", "submitted"}, "op")
                .count() <= 3 * 5);
}

TEST_CASE("Multi-op mixed transactions are valid", "[loadgen]")
{
    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    Simulation::pointer simulation =
        Topologies::pair(Simulation::OVER_LOOPBACK, networkID, [](int i) {
            auto cfg = getTestConfig(i);
            cfg.LOADGEN_OP_COUNT_FOR_TESTING = {3};
            cfg.LOADGEN_OP_COUNT_DISTRIBUTION_FOR_TESTING = {1};
            cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 1000;
            return cfg;
        });

    simulation->startAllNodes();
    simulation->crankUntil(
        [&]() { return simulation->haveAllExternalized(3, 1); },
        2 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

    auto nodes = simulation->getNodes();
    auto& app = *nodes[0]; // pick a node to generate load

    uint32_t txRate = 5;
    uint32_t numAccounts =
        txRate *
        static_cast<uint32>(Herder::EXP_LEDGER_TIMESPAN_SECONDS.count() * 3);
    auto& loadGen = app.getLoadGenerator();
    loadGen.generateLoad(GeneratedLoadConfig::createAccountsLoad(
        /* nAccounts */ numAccounts,
        /* txRate */ txRate));
    try
    {
        simulation->crankUntil(
            [&]() {
                return app.getMetrics()
                           .NewMeter({"loadgen", "run", "complete"}, "run")
                           .count() == 1;
            },
            3 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);
        auto config = GeneratedLoadConfig::txLoad(LoadGenMode::MIXED_CLASSIC,
                                                  numAccounts, 100, txRate);
        config.getMutDexTxPercent() = 50;
        loadGen.generateLoad(config);
        simulation->crankUntil(
            [&]() {
                return app.getMetrics()
                           .NewMeter({"loadgen", "run", "complete"}, "run")
                           .count() == 2;
            },
            15 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);
    }
    catch (...)
    {
        auto problems = loadGen.checkAccountSynced(app, false);
        REQUIRE(problems.empty());
    }

    REQUIRE(app.getMetrics()
                .NewMeter({"loadgen", "txn", "rejected"}, "txn")
                .count() == 0);
    REQUIRE(app.getMetrics()
                .NewMeter({"loadgen", "account", "created"}, "account")
                .count() == numAccounts);
    auto nonDexOps = app.getMetrics()
                         .NewMeter({"loadgen", "payment", "submitted"}, "op")
                         .count();
    auto dexOps = app.getMetrics()
                      .NewMeter({"loadgen", "manageoffer", "submitted"}, "op")
                      .count();
    REQUIRE(nonDexOps > 0);
    REQUIRE(dexOps > 0);
    REQUIRE(dexOps + nonDexOps == 3 * 100);
}

TEST_CASE("Upgrade setup with metrics reset", "[loadgen]")
{
    // Create a simulation with two nodes
    Simulation::pointer sim = Topologies::pair(
        Simulation::OVER_LOOPBACK, sha256(getTestConfig().NETWORK_PASSPHRASE));
    sim->startAllNodes();
    sim->crankUntil([&]() { return sim->haveAllExternalized(3, 1); },
                    2 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

    Application::pointer app = sim->getNodes().front();
    LoadGenerator& loadgen = app->getLoadGenerator();
    medida::Meter& runsComplete =
        app->getMetrics().NewMeter({"loadgen", "run", "complete"}, "run");
    medida::Meter& runsFailed =
        app->getMetrics().NewMeter({"loadgen", "run", "failed"}, "run");

    // Add an account
    loadgen.generateLoad(GeneratedLoadConfig::createAccountsLoad(
        /* nAccounts */ 1, /* txRate */ 1));
    sim->crankUntil([&]() { return runsComplete.count() == 1; },
                    5 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

    // Clear metrics to reset run count
    app->clearMetrics("");

    // Setup a soroban limit upgrade that must succeed
    GeneratedLoadConfig upgradeSetupCfg =
        GeneratedLoadConfig::createSorobanUpgradeSetupLoad();
    upgradeSetupCfg.setMinSorobanPercentSuccess(100);
    loadgen.generateLoad(upgradeSetupCfg);
    sim->crankUntil([&]() { return runsComplete.count() == 1; },
                    5 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);
    REQUIRE(runsFailed.count() == 0);

    // Clear metrics again to reset run count
    app->clearMetrics("");

    // Setup again. This should succeed even though it's the same account with
    // the same `runsComplete` value performing the setup
    loadgen.generateLoad(upgradeSetupCfg);
    sim->crankUntil([&]() { return runsComplete.count() == 1; },
                    5 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);
    REQUIRE(runsFailed.count() == 0);
}

TEST_CASE("apply load", "[loadgen][applyload]")
{
    auto cfg = getTestConfig();
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 1000;
    cfg.USE_CONFIG_FOR_GENESIS = true;
    cfg.LEDGER_PROTOCOL_VERSION = Config::CURRENT_LEDGER_PROTOCOL_VERSION;
    cfg.MANUAL_CLOSE = true;

    cfg.APPLY_LOAD_DATA_ENTRY_SIZE_FOR_TESTING = 1000;

    cfg.APPLY_LOAD_BL_SIMULATED_LEDGERS = 10000;
    cfg.APPLY_LOAD_BL_WRITE_FREQUENCY = 1000;
    cfg.APPLY_LOAD_BL_BATCH_SIZE = 1000;
    cfg.APPLY_LOAD_BL_LAST_BATCH_LEDGERS = 300;
    cfg.APPLY_LOAD_BL_LAST_BATCH_SIZE = 100;

    cfg.APPLY_LOAD_NUM_RO_ENTRIES_FOR_TESTING = {5, 10, 30};
    cfg.APPLY_LOAD_NUM_RO_ENTRIES_DISTRIBUTION_FOR_TESTING = {1, 1, 1};

    cfg.APPLY_LOAD_NUM_RW_ENTRIES_FOR_TESTING = {1, 5, 10};
    cfg.APPLY_LOAD_NUM_RW_ENTRIES_DISTRIBUTION_FOR_TESTING = {1, 1, 1};

    cfg.APPLY_LOAD_EVENT_COUNT_FOR_TESTING = {100};
    cfg.APPLY_LOAD_EVENT_COUNT_DISTRIBUTION_FOR_TESTING = {1};

    cfg.LOADGEN_TX_SIZE_BYTES_FOR_TESTING = {1'000, 2'000, 5'000};
    cfg.LOADGEN_TX_SIZE_BYTES_DISTRIBUTION_FOR_TESTING = {3, 2, 1};

    cfg.LOADGEN_INSTRUCTIONS_FOR_TESTING = {10'000'000, 50'000'000};
    cfg.LOADGEN_INSTRUCTIONS_DISTRIBUTION_FOR_TESTING = {5, 1};

    cfg.APPLY_LOAD_LEDGER_MAX_INSTRUCTIONS = 500'000'000;
    cfg.APPLY_LOAD_TX_MAX_INSTRUCTIONS = 100'000'000;

    cfg.APPLY_LOAD_LEDGER_MAX_READ_LEDGER_ENTRIES = 2000;
    cfg.APPLY_LOAD_TX_MAX_READ_LEDGER_ENTRIES = 100;

    cfg.APPLY_LOAD_LEDGER_MAX_READ_BYTES = 50'000'000;
    cfg.APPLY_LOAD_TX_MAX_READ_BYTES = 200'000;

    cfg.APPLY_LOAD_LEDGER_MAX_WRITE_LEDGER_ENTRIES = 1250;
    cfg.APPLY_LOAD_TX_MAX_WRITE_LEDGER_ENTRIES = 50;

    cfg.APPLY_LOAD_LEDGER_MAX_WRITE_BYTES = 700'000;
    cfg.APPLY_LOAD_TX_MAX_WRITE_BYTES = 66560;

    cfg.APPLY_LOAD_MAX_TX_SIZE_BYTES = 71680;
    cfg.APPLY_LOAD_MAX_LEDGER_TX_SIZE_BYTES = 800'000;

    cfg.APPLY_LOAD_MAX_CONTRACT_EVENT_SIZE_BYTES = 8198;
    cfg.APPLY_LOAD_MAX_TX_COUNT = 50;

    VirtualClock clock(VirtualClock::REAL_TIME);
    auto app = createTestApplication(clock, cfg);

    ApplyLoad al(*app);

    auto& ledgerClose =
        app->getMetrics().NewTimer({"ledger", "ledger", "close"});
    ledgerClose.Clear();

    auto& cpuInsRatio = app->getMetrics().NewHistogram(
        {"soroban", "host-fn-op", "invoke-time-fsecs-cpu-insn-ratio"});
    cpuInsRatio.Clear();

    auto& cpuInsRatioExclVm = app->getMetrics().NewHistogram(
        {"soroban", "host-fn-op", "invoke-time-fsecs-cpu-insn-ratio-excl-vm"});
    cpuInsRatioExclVm.Clear();

    auto& declaredInsnsUsageRatio = app->getMetrics().NewHistogram(
        {"soroban", "host-fn-op", "declared-cpu-insns-usage-ratio"});
    declaredInsnsUsageRatio.Clear();

    for (size_t i = 0; i < 100; ++i)
    {
        app->getBucketManager().getLiveBucketList().resolveAllFutures();
        releaseAssert(
            app->getBucketManager().getLiveBucketList().futuresAllResolved());

        al.benchmark();
    }
    REQUIRE(1.0 - al.successRate() < std::numeric_limits<double>::epsilon());
    CLOG_INFO(Perf, "Max ledger close: {} milliseconds", ledgerClose.max());
    CLOG_INFO(Perf, "Min ledger close: {} milliseconds", ledgerClose.min());
    CLOG_INFO(Perf, "Mean ledger close:  {} milliseconds", ledgerClose.mean());
    CLOG_INFO(Perf, "stddev ledger close:  {} milliseconds",
              ledgerClose.std_dev());

    CLOG_INFO(Perf, "Max CPU ins ratio: {}", cpuInsRatio.max() / 1000000);
    CLOG_INFO(Perf, "Mean CPU ins ratio:  {}", cpuInsRatio.mean() / 1000000);

    CLOG_INFO(Perf, "Max CPU ins ratio excl VM: {}",
              cpuInsRatioExclVm.max() / 1000000);
    CLOG_INFO(Perf, "Mean CPU ins ratio excl VM:  {}",
              cpuInsRatioExclVm.mean() / 1000000);
    CLOG_INFO(Perf, "stddev CPU ins ratio excl VM:  {}",
              cpuInsRatioExclVm.std_dev() / 1000000);

    CLOG_INFO(Perf, "Min CPU declared insns ratio: {}",
              declaredInsnsUsageRatio.min() / 1000000.0);
    CLOG_INFO(Perf, "Mean CPU declared insns ratio:  {}",
              declaredInsnsUsageRatio.mean() / 1000000.0);
    CLOG_INFO(Perf, "stddev CPU declared insns ratio:  {}",
              declaredInsnsUsageRatio.std_dev() / 1000000.0);

    CLOG_INFO(Perf, "Tx count utilization {}%",
              al.getTxCountUtilization().mean() / 1000.0);
    CLOG_INFO(Perf, "Instruction utilization {}%",
              al.getInstructionUtilization().mean() / 1000.0);
    CLOG_INFO(Perf, "Tx size utilization {}%",
              al.getTxSizeUtilization().mean() / 1000.0);
    CLOG_INFO(Perf, "Read bytes utilization {}%",
              al.getReadByteUtilization().mean() / 1000.0);
    CLOG_INFO(Perf, "Write bytes utilization {}%",
              al.getWriteByteUtilization().mean() / 1000.0);
    CLOG_INFO(Perf, "Read entry utilization {}%",
              al.getReadEntryUtilization().mean() / 1000.0);
    CLOG_INFO(Perf, "Write entry utilization {}%",
              al.getWriteEntryUtilization().mean() / 1000.0);
}
