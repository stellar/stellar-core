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
#include "transactions/TransactionUtils.h"
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

TEST_CASE("synthetic SAC load recycles source accounts in overlay-only mode",
          "[loadgen]")
{
    // perfnet's block_time_check load: MIXED_PREGEN_SAC_PAYMENT with no
    // classic stream, in overlay-only mode, with more transactions than
    // source accounts. The soroban stream fails the run as soon as its pool
    // is empty, so the run completes only if accounts return to the pool once
    // their transactions' ledgers close. The pool covers twenty seconds of
    // load, far more than the few one-second ledgers a transaction is in
    // flight, so a stalled real-time simulation does not drain it.
    uint32_t const nAccounts = 300;
    uint32_t const sorobanTxRate = 15;
    uint32_t const nTxs = nAccounts + 100;

    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    Simulation::pointer simulation = Topologies::pair(networkID, [&](int i) {
        auto cfg = getTestConfig(i);
        cfg.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = true;
        cfg.ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING = true;
        cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
            Config::CURRENT_LEDGER_PROTOCOL_VERSION;
        cfg.GENESIS_TEST_ACCOUNT_COUNT = nAccounts;
        return cfg;
    });

    simulation->startAllNodes();
    simulation->crankUntil(
        [&]() { return simulation->haveAllExternalized(3, 1); },
        10 * simulation->getExpectedLedgerCloseTime(), false);

    auto nodes = simulation->getNodes();
    auto& app = *nodes[0];

    // Max out Soroban network limits so synthetic footprints pass checkValid
    // and every submitted transaction fits in the next ledger.
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

    // MIXED_PREGEN_* modes open a pregenerated classic file even when the
    // classic stream is off; this run never reads from it.
    std::string fileName =
        app.getConfig().LOADGEN_PREGENERATED_TRANSACTIONS_FILE;
    auto cleanup = gsl::finally([&]() { std::remove(fileName.c_str()); });
    generateTransactions(app, fileName, /* numTransactions */ 1, nAccounts,
                         /* offset */ 0);

    for (auto& node : nodes)
    {
        node->setRunInOverlayOnlyMode(true);
    }

    auto& completed =
        app.getMetrics().NewMeter({"loadgen", "run", "complete"}, "run");
    auto& failed =
        app.getMetrics().NewMeter({"loadgen", "run", "failed"}, "run");
    auto const completedBefore = completed.count();
    auto const failedBefore = failed.count();

    // Per-transaction submission metrics.
    auto& attempted =
        app.getMetrics().NewMeter({"loadgen", "txn", "attempted"}, "txn");
    auto& invokes =
        app.getMetrics().NewMeter({"loadgen", "soroban", "invoke"}, "txn");
    auto& payments =
        app.getMetrics().NewMeter({"loadgen", "payment", "submitted"}, "op");
    auto& bytes = app.getMetrics().NewMeter({"loadgen", "txn", "bytes"}, "txn");
    auto const attemptedBefore = attempted.count();
    auto const invokesBefore = invokes.count();
    auto const paymentsBefore = payments.count();
    auto const bytesBefore = bytes.count();

    // Every synthetic SAC payment has the same XDR shape, so one reference
    // transaction gives the wire size of each message loadgen submits.
    TxGenerator refGen(app);
    SCAddress refDest(SC_ADDRESS_TYPE_CONTRACT);
    refDest.contractId() = sha256("reference destination");
    auto refTx = refGen
                     .invokeSACPayment(
                         app.getLedgerManager().getLastClosedLedgerNum() + 1,
                         /* fromAccountId */ 0, refDest,
                         makeSyntheticContractInstance("loadgen_sac"),
                         /* amount */ 100, std::nullopt)
                     .second;
    auto const refMsgBytes = xdr::xdr_argpack_size(*refTx->toStellarMessage());

    GeneratedLoadConfig cfg = GeneratedLoadConfig::txLoad(
        LoadGenMode::MIXED_PREGEN_SAC_PAYMENT, nAccounts, nTxs, sorobanTxRate,
        /* offset */ 0);
    cfg.preloadedTransactionsFile = fileName;
    auto& mix = cfg.getMutMixPregenSorobanConfig();
    mix.classicTxRate = 0;
    mix.sorobanTxRate = sorobanTxRate;
    app.getLoadGenerator().generateLoad(cfg);

    simulation->crankUntil(
        [&]() {
            return completed.count() == completedBefore + 1 ||
                   failed.count() != failedBefore;
        },
        60 * simulation->getExpectedLedgerCloseTime(), false);
    REQUIRE(failed.count() == failedBefore);
    REQUIRE(completed.count() == completedBefore + 1);

    REQUIRE(attempted.count() - attemptedBefore == nTxs);
    REQUIRE(invokes.count() - invokesBefore == nTxs);
    REQUIRE(payments.count() == paymentsBefore);
    REQUIRE(bytes.count() - bytesBefore == nTxs * refMsgBytes);
}

TEST_CASE("native SAC setup and payments are applied", "[loadgen][soroban]")
{
    // SAC_SETUP creates the native asset contract with a real transaction and
    // SAC_PAYMENT pays through it, with apply on (unlike the MIXED_PREGEN_*
    // modes, whose synthetic contract only exists in overlay-only mode). The
    // run has more transactions than source accounts, so it also needs
    // accounts to return to the pool after their transactions are applied.
    uint32_t const nAccounts = 300;
    uint32_t const txRate = 15;
    uint32_t const nTxs = nAccounts + 100;

    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    Simulation::pointer simulation = Topologies::pair(networkID, [&](int i) {
        auto cfg = getTestConfig(i);
        cfg.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = true;
        cfg.ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING = true;
        cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
            Config::CURRENT_LEDGER_PROTOCOL_VERSION;
        cfg.GENESIS_TEST_ACCOUNT_COUNT = nAccounts;
        return cfg;
    });

    simulation->startAllNodes();
    simulation->crankUntil(
        [&]() { return simulation->haveAllExternalized(3, 1); },
        10 * simulation->getExpectedLedgerCloseTime(), false);

    auto nodes = simulation->getNodes();
    auto& app = *nodes[0];
    auto& loadGen = app.getLoadGenerator();

    // Limits high enough that every submitted transaction fits in the next
    // ledger.
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

    auto& completed =
        app.getMetrics().NewMeter({"loadgen", "run", "complete"}, "run");
    auto& failed =
        app.getMetrics().NewMeter({"loadgen", "run", "failed"}, "run");
    auto& attempted =
        app.getMetrics().NewMeter({"loadgen", "txn", "attempted"}, "txn");
    auto applySuccess = [](Application& node) {
        return node.getMetrics()
            .NewCounter({"ledger", "apply-soroban", "success"})
            .count();
    };
    auto applyFailure = [](Application& node) {
        return node.getMetrics()
            .NewCounter({"ledger", "apply-soroban", "failure"})
            .count();
    };
    auto runToCompletion = [&](GeneratedLoadConfig cfg) {
        auto const completedBefore = completed.count();
        auto const failedBefore = failed.count();
        loadGen.generateLoad(cfg);
        simulation->crankUntil(
            [&]() {
                return completed.count() == completedBefore + 1 ||
                       failed.count() != failedBefore;
            },
            120 * simulation->getExpectedLedgerCloseTime(), false);
        REQUIRE(failed.count() == failedBefore);
        REQUIRE(completed.count() == completedBefore + 1);
    };

    auto instanceKey = LoadGenerator::getNativeSACInstanceKey(networkID);
    auto setupCfg = GeneratedLoadConfig::txLoad(LoadGenMode::SAC_SETUP,
                                                nAccounts, /* nTxs */ 1,
                                                /* txRate */ 1);
    auto paymentCfg = GeneratedLoadConfig::txLoad(LoadGenMode::SAC_PAYMENT,
                                                  nAccounts, nTxs, txRate);
    // Any payment that fails at apply fails the run.
    paymentCfg.setMinSorobanPercentSuccess(100);

    // The payments need the contract.
    REQUIRE(!app.getLedgerManager().getLCLView()->load(instanceKey));
    REQUIRE_THROWS_WITH(
        loadGen.generateLoad(paymentCfg),
        "Before running MODE::SAC_PAYMENT, please run MODE::SAC_SETUP to "
        "create the native asset contract first.");

    // Neither mode can complete when nothing is applied.
    app.setRunInOverlayOnlyMode(true);
    REQUIRE_THROWS_WITH(loadGen.generateLoad(setupCfg),
                        "SAC_SETUP / SAC_PAYMENT need transactions to be "
                        "applied, they can not run in overlay only mode");
    REQUIRE_THROWS_WITH(loadGen.generateLoad(paymentCfg),
                        "SAC_SETUP / SAC_PAYMENT need transactions to be "
                        "applied, they can not run in overlay only mode");
    app.setRunInOverlayOnlyMode(false);

    // Setup: one applied transaction creates the instance on every node.
    auto successBefore = applySuccess(app);
    auto attemptedBefore = attempted.count();
    runToCompletion(setupCfg);
    REQUIRE(attempted.count() - attemptedBefore == 1);
    for (auto const& node : nodes)
    {
        REQUIRE(static_cast<bool>(
            node->getLedgerManager().getLCLView()->load(instanceKey)));
        REQUIRE(applySuccess(*node) == successBefore + 1);
        REQUIRE(applyFailure(*node) == 0);
    }

    // Setup again: the instance is there, so nothing is submitted.
    attemptedBefore = attempted.count();
    runToCompletion(setupCfg);
    REQUIRE(attempted.count() == attemptedBefore);
    REQUIRE(applySuccess(app) == successBefore + 1);

    // Payments: every one is applied successfully on every node.
    successBefore = applySuccess(app);
    attemptedBefore = attempted.count();
    auto const firstLedger =
        app.getLedgerManager().getLastClosedLedgerNum() + 1;
    runToCompletion(paymentCfg);
    auto const lastLedger = app.getLedgerManager().getLastClosedLedgerNum();
    REQUIRE(attempted.count() - attemptedBefore == nTxs);
    for (auto const& node : nodes)
    {
        REQUIRE(applySuccess(*node) == successBefore + nTxs);
        REQUIRE(applyFailure(*node) == 0);
    }

    // The payments moved funds: the destination of the first payment of the
    // run holds the amount paid. Its name contains the ledger the payment was
    // generated for, which is one of the ledgers of the run.
    auto balanceOf = [&](Application& node,
                         SCAddress const& dest) -> std::optional<uint64_t> {
        SCVal destVal(SCV_ADDRESS);
        destVal.address() = dest;
        LedgerKey balanceKey(CONTRACT_DATA);
        balanceKey.contractData().contract = instanceKey.contractData().contract;
        balanceKey.contractData().key =
            txtest::makeVecSCVal({makeSymbolSCVal("Balance"), destVal});
        balanceKey.contractData().durability =
            ContractDataDurability::PERSISTENT;
        auto entry = node.getLedgerManager().getLCLView()->load(balanceKey);
        if (!entry)
        {
            return std::nullopt;
        }
        for (auto const& field : *entry.current().data.contractData().val.map())
        {
            if (field.key.type() == SCV_SYMBOL && field.key.sym() == "amount")
            {
                REQUIRE(field.val.i128().hi == 0);
                return field.val.i128().lo;
            }
        }
        return std::nullopt;
    };
    for (auto const& node : nodes)
    {
        uint32_t found = 0;
        for (auto ledger = firstLedger; ledger <= lastLedger; ++ledger)
        {
            auto balance = balanceOf(
                *node, LoadGenerator::getNativeSACPaymentDestination(
                           app.getConfig().NODE_SEED.getPublicKey(), ledger,
                           /* counter */ 0));
            if (balance)
            {
                REQUIRE(*balance == 100);
                ++found;
            }
        }
        REQUIRE(found == 1);
    }
}

TEST_CASE("externalized tx set hashes and sources match transaction frames",
          "[loadgen][herder][txset]")
{
    // Herder hands the overlay getTransactionFullHashes() and loadgen the
    // envelopes from forEachTransactionEnvelope() instead of building a frame
    // per transaction. Pin both to what frames built from the same set give:
    // same order, same full hashes, same source accounts.
    Hash const networkID = sha256("externalized tx set test network");
    auto key = [](std::string const& name) {
        return txtest::getAccount(name).getPublicKey();
    };
    uint32_t nextSeq = 1;
    auto v1Tx = [&](PublicKey const& source,
                    std::optional<uint64_t> muxId = std::nullopt) {
        TransactionEnvelope env(ENVELOPE_TYPE_TX);
        env.v1().tx.sourceAccount = toMuxedAccount(source, muxId);
        env.v1().tx.fee = 100;
        env.v1().tx.seqNum = nextSeq++;
        env.v1().tx.operations.emplace_back(txtest::payment(key("dest"), 1));
        return env;
    };
    auto v0Tx = [&](PublicKey const& source) {
        TransactionEnvelope env(ENVELOPE_TYPE_TX_V0);
        env.v0().tx.sourceAccountEd25519 = source.ed25519();
        env.v0().tx.fee = 100;
        env.v0().tx.seqNum = nextSeq++;
        env.v0().tx.operations.emplace_back(txtest::payment(key("dest"), 1));
        return env;
    };
    auto feeBumpTx = [&](PublicKey const& feeSource, PublicKey const& source) {
        TransactionEnvelope env(ENVELOPE_TYPE_TX_FEE_BUMP);
        env.feeBump().tx.feeSource = toMuxedAccount(feeSource, 7);
        env.feeBump().tx.fee = 400;
        env.feeBump().tx.innerTx.type(ENVELOPE_TYPE_TX);
        env.feeBump().tx.innerTx.v1() = v1Tx(source).v1();
        return env;
    };

    auto checkMatchesFrames = [&](TxSetXDRFrameConstPtr const& txSet,
                                  size_t expectedTxs) {
        std::vector<TransactionFrameBasePtr> frames;
        for (auto const& phase : txSet->createTransactionFrames(networkID))
        {
            frames.insert(frames.end(), phase.begin(), phase.end());
        }
        std::vector<TransactionEnvelope> envelopes;
        txSet->forEachTransactionEnvelope(
            [&](TransactionEnvelope const& env) { envelopes.push_back(env); });
        auto hashes = txSet->getTransactionFullHashes();

        REQUIRE(frames.size() == expectedTxs);
        REQUIRE(envelopes.size() == expectedTxs);
        REQUIRE(hashes.size() == expectedTxs);
        for (size_t i = 0; i < expectedTxs; ++i)
        {
            REQUIRE(envelopes[i] == frames[i]->getEnvelope());
            REQUIRE(hashes[i] == frames[i]->getFullHash());
            REQUIRE(LoadGenerator::getEnvelopeSourceID(envelopes[i]) ==
                    frames[i]->getSourceID());
        }
    };

    SECTION("source account of each envelope type")
    {
        REQUIRE(LoadGenerator::getEnvelopeSourceID(v0Tx(key("a"))) == key("a"));
        REQUIRE(LoadGenerator::getEnvelopeSourceID(v1Tx(key("b"))) == key("b"));
        // Muxed sources resolve to the underlying account.
        REQUIRE(LoadGenerator::getEnvelopeSourceID(v1Tx(key("c"), 42)) ==
                key("c"));
        // A fee bump's source is its inner transaction's, not the fee source.
        REQUIRE(LoadGenerator::getEnvelopeSourceID(
                    feeBumpTx(key("fee"), key("inner"))) == key("inner"));
    }
    SECTION("legacy tx set")
    {
        TransactionSet xdrSet;
        xdrSet.previousLedgerHash = sha256("previous ledger");
        xdrSet.txs = {v0Tx(key("a")), v1Tx(key("b")), v1Tx(key("c"), 42),
                      feeBumpTx(key("fee"), key("inner"))};
        checkMatchesFrames(TxSetXDRFrame::makeFromWire(xdrSet), 4);
    }
    SECTION("generalized tx set with sequential and parallel phases")
    {
        GeneralizedTransactionSet xdrSet(1);
        auto& v1Set = xdrSet.v1TxSet();
        v1Set.previousLedgerHash = sha256("previous ledger");

        auto& classic = v1Set.phases.emplace_back(0);
        for (auto const& txs :
             {std::vector<TransactionEnvelope>{v1Tx(key("a")), v0Tx(key("b"))},
              std::vector<TransactionEnvelope>{
                  v1Tx(key("c"), 42), feeBumpTx(key("fee"), key("d"))}})
        {
            auto& component = classic.v0Components().emplace_back(
                TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
            component.txsMaybeDiscountedFee().txs.assign(txs.begin(),
                                                         txs.end());
        }

        auto& parallel = v1Set.phases.emplace_back(1);
        auto& stages = parallel.parallelTxsComponent().executionStages;
        auto& stage0 = stages.emplace_back();
        stage0.push_back({v1Tx(key("e")), v1Tx(key("f"))});
        stage0.push_back({feeBumpTx(key("fee"), key("g"))});
        auto& stage1 = stages.emplace_back();
        stage1.push_back({v1Tx(key("h"), 9)});

        checkMatchesFrames(TxSetXDRFrame::makeFromWire(xdrSet), 8);
    }
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
    SECTION("recycle available accounts across ledgers and runs")
    {
        // More submissions than accounts requires returning accounts to the
        // pool once their transactions have applied. Every transaction must
        // apply successfully, and a run completes only once each account's
        // cached sequence number matches the ledger. A second run exercises
        // resetting the pool.
        uint32_t const txsPerRun = 2 * nAccounts;
        auto& completed =
            app.getMetrics().NewMeter({"loadgen", "run", "complete"}, "run");
        auto& failed =
            app.getMetrics().NewMeter({"loadgen", "run", "failed"}, "run");
        for (uint32_t run = 0; run < 2; ++run)
        {
            auto before = getSuccessfulTxCount();
            auto runsBefore = completed.count();
            auto failedBefore = failed.count();
            loadGen.generateLoad(GeneratedLoadConfig::txLoad(
                LoadGenMode::PAY, nAccounts, txsPerRun,
                /* txRate */ 200, /* offset */ nAccounts));
            simulation->crankUntil(
                [&]() {
                    return completed.count() == runsBefore + 1 ||
                           failed.count() != failedBefore;
                },
                120 * simulation->getExpectedLedgerCloseTime(), false);
            REQUIRE(failed.count() == failedBefore);
            REQUIRE(getSuccessfulTxCount() - before == txsPerRun);
        }
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
    // Check that the settings were properly updated. Read from the last
    // closed ledger view rather than the LedgerTxn root: the node is still
    // closing ledgers and the root belongs to the apply thread while it does.
    auto ledgerView = app.getLedgerManager().getLCLView();
    auto contractExecutionLanesSettingsEntry = ledgerView->load(
        configSettingKey(CONFIG_SETTING_CONTRACT_EXECUTION_LANES));
    auto stateArchivalConfigSettinsgEntry =
        ledgerView->load(configSettingKey(CONFIG_SETTING_STATE_ARCHIVAL));
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
    Simulation::pointer simulation = Topologies::pair(networkID, [&](int i) {
        auto cfg = getTestConfig(i);
        cfg.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = true;
        cfg.USE_CONFIG_FOR_GENESIS = false;
        cfg.ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING = true;
        cfg.UPDATE_SOROBAN_COSTS_DURING_PROTOCOL_UPGRADE_FOR_TESTING = true;
        cfg.GENESIS_TEST_ACCOUNT_COUNT = 500;
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
        10 * simulation->getExpectedLedgerCloseTime(), false);

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

    // One account per invoke transaction (see numSorobanTxs below): the Rust
    // mempool is fee-ordered and sequence-number-oblivious, so chained
    // transactions from one account get sampled out of order and trimmed as
    // invalid at nomination.
    auto nAccounts = 500;
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
        auto ledgerView = app.getLedgerManager().getLCLView();
        auto entry = ledgerView->load(upgradeLK);
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
    auto const numInstances = 20;
    auto const numSorobanTxs = 500;

    numTxsBefore = getSuccessfulTxCount();

    // Real-time simulation: 1 tx/s (fine in virtual time) would take minutes
    // here, so submit as fast as the load generator allows: it requires at
    // least 3x as many accounts as transactions per ledger.
    uint32_t const txRate = nAccounts / 3;
    loadGen.generateLoad(GeneratedLoadConfig::createSorobanInvokeSetupLoad(
        /* nAccounts */ nAccounts, numInstances, txRate));
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
        LoadGenMode::SOROBAN_INVOKE, nAccounts, numSorobanTxs, txRate);

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

            auto ledgerView = app.getLedgerManager().getLCLView();
            auto entry = ledgerView->load(lk);
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
            LoadGenMode::MIXED_CLASSIC_SOROBAN, nAccounts, numMixedTxs, txRate);

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
    auto cfg = getTestConfig();
    cfg.APPLY_LOAD_MODE = ApplyLoadMode::LIMIT_BASED;
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
    // Each benchmark ledger runs at least one cold tx set validation.
    REQUIRE(
        app->getMetrics().NewTimer({"herder", "txset", "validate"}).count() >=
        cfg.APPLY_LOAD_NUM_LEDGERS);
}

TEST_CASE("apply load benchmark model tx",
          "[loadgen][applyload][soroban][acceptance]")
{
    auto cfg = getTestConfig();
    cfg.APPLY_LOAD_MODE = ApplyLoadMode::BENCHMARK_MODEL_TX;
    cfg.APPLY_LOAD_MODEL_TX = ApplyLoadModelTx::SAC;
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
    // Each benchmark ledger runs at least one cold tx set validation.
    REQUIRE(
        app->getMetrics().NewTimer({"herder", "txset", "validate"}).count() >=
        cfg.APPLY_LOAD_NUM_LEDGERS);
}
