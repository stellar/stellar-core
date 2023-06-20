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

TEST_CASE("account creation", "[loadgen]")
{
    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    Simulation::pointer simulation =
        Topologies::pair(Simulation::OVER_LOOPBACK, networkID, [](int i) {
            auto cfg = getTestConfig(i);
            cfg.LIMIT_TX_QUEUE_SOURCE_ACCOUNT = true;
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
            /* txRate */ 1,
            /* batchSize */ 100));
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
                                                         /* txRate */ 10,
                                                         /* batchSize */ 100));
        simulation->crankUntil(
            [&]() {
                return app.getMetrics()
                           .NewMeter({"loadgen", "run", "complete"}, "run")
                           .count() == 2;
            },
            300 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);
    }
    SECTION("invalid creation parameters")
    {
        size_t batch = 10;
        loadGen.generateLoad(GeneratedLoadConfig::createAccountsLoad(
            /* nAccounts */ 10000,
            /* txRate */ batch / Herder::EXP_LEDGER_TIMESPAN_SECONDS.count() +
                1,
            /* batchSize */ batch));
        simulation->crankUntil(
            [&]() {
                return app.getMetrics()
                           .NewMeter({"loadgen", "run", "failed"}, "run")
                           .count() == 1;
            },
            10 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);
    }
    SECTION("invalid loadgen parameters")
    {
        // Succesfully create accounts
        size_t numAccounts = 100;
        loadGen.generateLoad(GeneratedLoadConfig::createAccountsLoad(
            /* nAccounts */ 100,
            /* txRate */ 1,
            /* batchSize */ 100));
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
                                        /* txRate */ 100,
                                        /* batchSize */ 100));
        simulation->crankUntil(
            [&]() {
                return app.getMetrics()
                           .NewMeter({"loadgen", "run", "failed"}, "run")
                           .count() == 1;
            },
            10 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);
    }
}

TEST_CASE("Multi-op pretend transactions are valid", "[loadgen]")
{
    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    Simulation::pointer simulation =
        Topologies::pair(Simulation::OVER_LOOPBACK, networkID, [](int i) {
            auto cfg = getTestConfig(i);
            cfg.LIMIT_TX_QUEUE_SOURCE_ACCOUNT = true;
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
        /* txRate */ txRate,
        /* batchSize */ 100));
    try
    {
        simulation->crankUntil(
            [&]() {
                return app.getMetrics()
                           .NewMeter({"loadgen", "run", "complete"}, "run")
                           .count() == 1;
            },
            3 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

        loadGen.generateLoad(GeneratedLoadConfig::txLoad(
            LoadGenMode::PRETEND, nAccounts, 5, txRate, 100));

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
            cfg.LIMIT_TX_QUEUE_SOURCE_ACCOUNT = true;
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
        txRate * Herder::EXP_LEDGER_TIMESPAN_SECONDS.count() * 3;
    auto& loadGen = app.getLoadGenerator();
    loadGen.generateLoad(GeneratedLoadConfig::createAccountsLoad(
        /* nAccounts */ numAccounts,
        /* txRate */ txRate,
        /* batchSize */ 100));
    try
    {
        simulation->crankUntil(
            [&]() {
                return app.getMetrics()
                           .NewMeter({"loadgen", "run", "complete"}, "run")
                           .count() == 1;
            },
            3 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);
        auto config = GeneratedLoadConfig::txLoad(
            LoadGenMode::MIXED_TXS, numAccounts, 100, txRate, 100);
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
