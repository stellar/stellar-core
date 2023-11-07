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

TEST_CASE("generate soroban load", "[loadgen]")
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
    for (auto node : nodes)
    {
        overrideSorobanNetworkConfigForTest(*node);
        modifySorobanNetworkConfig(*node, [&](SorobanNetworkConfig& cfg) {
            // Entries should never expire
            cfg.mStateArchivalSettings.maxEntryTTL = UINT32_MAX;
            cfg.mStateArchivalSettings.minPersistentTTL = 5'000'000;

            // Set memory and write limits so that we can write all keys in a
            // single TX during setup
            cfg.mTxMemoryLimit = UINT32_MAX;
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

    auto& app = *nodes[0]; // pick a node to generate load

    auto& loadGen = app.getLoadGenerator();

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

    auto numTxsBefore = 0;
    {
        auto& txsSucceeded =
            nodes[0]->getMetrics().NewCounter({"ledger", "apply", "success"});
        numTxsBefore = txsSucceeded.count();
    }

    auto const numSorobanTxs = 300;
    auto const numDataEntries = 5;
    auto cfg = GeneratedLoadConfig::txLoad(LoadGenMode::SOROBAN_INVOKE,
                                           nAccounts, numSorobanTxs,
                                           /* txRate */ 1);
    cfg.nDataEntries = numDataEntries;
    cfg.sorobanNoResetForTesting = true;

    loadGen.generateLoad(cfg);
    simulation->crankUntil(
        [&]() {
            return app.getMetrics()
                       .NewMeter({"loadgen", "run", "complete"}, "run")
                       .count() == 2;
        },
        300 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

    // Check that Soroban TXs were successfully applied
    for (auto node : nodes)
    {
        auto& txsSucceeded =
            node->getMetrics().NewCounter({"ledger", "apply", "success"});
        auto& txsFailed =
            node->getMetrics().NewCounter({"ledger", "apply", "failure"});
        REQUIRE(txsSucceeded.count() == numTxsBefore + numSorobanTxs);
        REQUIRE(txsFailed.count() == 0);
    }

    auto instances = loadGen.getContractInstancesForTesting();
    REQUIRE(instances.size() == static_cast<size_t>(nAccounts));

    // Check that each key is unique and exists in the DB
    UnorderedSet<LedgerKey> keys;
    std::optional<LedgerKey> codeKey;
    for (auto const& [id, instance] : instances)
    {
        REQUIRE(instance.readOnlyKeys.size() == numDataEntries + 2);
        for (size_t i = 0; i < instance.readOnlyKeys.size(); ++i)
        {
            auto const& lk = instance.readOnlyKeys[i];
            if (i == 0)
            {
                REQUIRE(lk.type() == CONTRACT_CODE);
                if (codeKey)
                {
                    REQUIRE(lk == *codeKey);
                }
                else
                {
                    codeKey = lk;
                }
            }
            else
            {
                REQUIRE(lk.type() == CONTRACT_DATA);
                if (i == 1)
                {
                    REQUIRE(lk.contractData().key.type() ==
                            SCV_LEDGER_KEY_CONTRACT_INSTANCE);
                }
                else
                {
                    REQUIRE(lk.contractData().key.type() == SCV_SYMBOL);
                }

                REQUIRE(keys.find(lk) == keys.end());
                keys.insert(lk);
            }
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
        config.dexTxPercent = 50;
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
