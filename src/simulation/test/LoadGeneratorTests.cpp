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

namespace
{

void
configureOverlayV2Pair(Config& cfg, int i)
{
    if (i == 1 || i == 2)
    {
        auto peerIndex = i == 1 ? 2 : 1;
        cfg.KNOWN_PEERS.push_back(
            fmt::format("127.0.0.1:{}", getTestConfig(peerIndex).PEER_PORT));
    }
}

}

TEST_CASE("loadgen in overlay-only mode", "[loadgen]")
{
    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    Simulation::pointer simulation =
        Topologies::pair(networkID, [&](int i) {
            auto cfg = getTestConfig(i);
            configureOverlayV2Pair(cfg, i);
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

TEST_CASE("mixed pregen and synthetic soroban in overlay-only mode",
          "[loadgen]")
{
    uint32_t const nAccounts = 200;
    uint32_t const genesisAccountCount = nAccounts;
    uint32_t const nTxs = 60;

    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    Simulation::pointer simulation =
        Topologies::pair(networkID, [&](int i) {
            auto cfg = getTestConfig(i);
            configureOverlayV2Pair(cfg, i);
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
