// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SHA.h"
#include "crypto/SecretKey.h"
#include "lib/catch.hpp"
#include "main/Config.h"
#include "scp/QuorumSetUtils.h"
#include "simulation/LoadGenerator.h"
#include "simulation/Topologies.h"
#include "test/test.h"
#include "transactions/test/SorobanTxTestUtils.h"
#include "util/Math.h"
#include <fmt/format.h>

using namespace stellar;

TEST_CASE("generate load with unique accounts", "[loadgen]")
{
    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    Simulation::pointer simulation =
        Topologies::pair(Simulation::OVER_LOOPBACK, networkID, [](int i) {
            auto cfg = getTestConfig(i);
            cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 5000;
            return cfg;
        });

    simulation->startAllNodes();
    simulation->crankUntil(
        [&]() { return simulation->haveAllExternalized(3, 1); },
        2 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

    auto nodes = simulation->getNodes();
    auto& app = *nodes[0]; // pick a node to generate load

    auto& loadGen = app.getLoadGenerator();

    SECTION("success")
    {
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

        loadGen.generateLoad(GeneratedLoadConfig::txLoad(LoadGenMode::PAY,
                                                         /* nAccounts */ 10000,
                                                         /* nTxs */ 10000,
                                                         /* txRate */ 10));
        simulation->crankUntil(
            [&]() {
                return app.getMetrics()
                           .NewMeter({"loadgen", "run", "complete"}, "run")
                           .count() == 2;
            },
            300 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);
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
}

TEST_CASE("generate soroban load", "[loadgen][soroban]")
{
    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    Simulation::pointer simulation =
        Topologies::pair(Simulation::OVER_LOOPBACK, networkID, [](int i) {
            auto cfg = getTestConfig(i);
            cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 5000;
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

    auto nAccounts = 20;
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

    int64_t numTxsBefore = getSuccessfulTxCount();

    // Make sure config upgrade works with initial network config settings
    loadGen.generateLoad(GeneratedLoadConfig::createSorobanUpgradeSetupLoad());
    simulation->crankUntil(
        [&]() {
            return app.getMetrics()
                       .NewMeter({"loadgen", "run", "complete"}, "run")
                       .count() == 2;
        },
        100 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

    // Check that Soroban TXs were successfully applied
    for (auto node : nodes)
    {
        auto& txsSucceeded =
            node->getMetrics().NewCounter({"ledger", "apply", "success"});
        auto& txsFailed =
            node->getMetrics().NewCounter({"ledger", "apply", "failure"});

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

    auto upgradeSetKey =
        loadGen.getConfigUpgradeSetKey(createUpgradeLoadGenConfig);

    numTxsBefore = getSuccessfulTxCount();
    loadGen.generateLoad(createUpgradeLoadGenConfig);
    simulation->crankUntil(
        [&]() {
            return app.getMetrics()
                       .NewMeter({"loadgen", "run", "complete"}, "run")
                       .count() == 3;
        },
        300 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

    for (auto node : nodes)
    {
        auto& txsSucceeded =
            node->getMetrics().NewCounter({"ledger", "apply", "success"});
        auto& txsFailed =
            node->getMetrics().NewCounter({"ledger", "apply", "failure"});

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

    for (uint32_t i = 0;
         i < static_cast<uint32_t>(CONFIG_SETTING_BUCKETLIST_SIZE_WINDOW); ++i)
    {
        auto setting = upgrades.updatedEntry[i];
        REQUIRE(setting.configSettingID() == static_cast<ConfigSettingID>(i));
        switch (static_cast<ConfigSettingID>(i))
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

    // Manually override network settings for invoke load gen
    for (auto node : nodes)
    {
        overrideSorobanNetworkConfigForTest(*node);
        modifySorobanNetworkConfig(*node, [&](SorobanNetworkConfig& cfg) {
            // Entries should never expire
            cfg.mStateArchivalSettings.maxEntryTTL = 1'000'000;
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
        });
    }

    auto const numInstances = 10;
    auto const numSorobanTxs = 100;
    auto const numDataEntries = 5;
    auto const ioKiloBytes = 15;

    numTxsBefore = getSuccessfulTxCount();
    loadGen.generateLoad(GeneratedLoadConfig::createSorobanInvokeSetupLoad(
        /* nAccounts */ nAccounts, numInstances,
        /* txRate */ 1));
    simulation->crankUntil(
        [&]() {
            return app.getMetrics()
                       .NewMeter({"loadgen", "run", "complete"}, "run")
                       .count() == 4;
        },
        100 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

    // Check that Soroban TXs were successfully applied
    for (auto node : nodes)
    {
        auto& txsSucceeded =
            node->getMetrics().NewCounter({"ledger", "apply", "success"});
        auto& txsFailed =
            node->getMetrics().NewCounter({"ledger", "apply", "failure"});

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

    // Use tight bounds to we can verify storage works properly
    auto& invokeCfg = invokeLoadCfg.getMutSorobanInvokeConfig();
    invokeCfg.nDataEntriesLow = numDataEntries;
    invokeCfg.nDataEntriesHigh = numDataEntries;
    invokeCfg.ioKiloBytesLow = ioKiloBytes;
    invokeCfg.ioKiloBytesHigh = ioKiloBytes;

    invokeCfg.txSizeBytesHigh = 100'000;
    invokeCfg.instructionsHigh = 10'000'000;

    loadGen.generateLoad(invokeLoadCfg);
    simulation->crankUntil(
        [&]() {
            return app.getMetrics()
                       .NewMeter({"loadgen", "run", "complete"}, "run")
                       .count() == 5;
        },
        300 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

    // Check that Soroban TXs were successfully applied
    for (auto node : nodes)
    {
        auto& txsSucceeded =
            node->getMetrics().NewCounter({"ledger", "apply", "success"});
        auto& txsFailed =
            node->getMetrics().NewCounter({"ledger", "apply", "failure"});

        // Because we can't preflight TXs, some invocations will fail due to too
        // few resources. This is expected, as our instruction counts are
        // approximations. The following checks will make sure all set up
        // phases succeeded, so only the invoke phase may have acceptable failed
        // TXs
        REQUIRE(txsSucceeded.count() > numTxsBefore + numSorobanTxs - 5);
        REQUIRE(txsFailed.count() < 5);
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
        auto config = GeneratedLoadConfig::txLoad(LoadGenMode::MIXED_TXS,
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
