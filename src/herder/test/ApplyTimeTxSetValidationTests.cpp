// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// Tests of apply-time transaction set validation
// (APPLY_TIME_TX_SET_VALIDATION_PROTOCOL_VERSION): validators vote for a
// value without validating its transaction set, and a set that turns out to
// be invalid when its ledger is applied closes that ledger as an empty one.

#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "herder/Herder.h"
#include "herder/HerderImpl.h"
#include "herder/HerderSCPDriver.h"
#include "herder/PendingEnvelopes.h"
#include "herder/test/TestTxSetUtils.h"
#include "ledger/LedgerHeaderUtils.h"
#include "ledger/LedgerManager.h"
#include "ledger/test/LedgerTestUtils.h"
#include "main/Application.h"
#include "main/PersistentState.h"
#include "overlay/RustOverlayManager.h"
#include "simulation/Simulation.h"
#include "simulation/Topologies.h"
#include "test/Catch2.h"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/TransactionBridge.h"
#include "util/MetricsRegistry.h"
#include "util/ProtocolVersion.h"
#include "util/XDROperators.h"
#include <filesystem>
#include <fmt/format.h>
#include <set>

using namespace stellar;
using namespace stellar::txbridge;
using namespace stellar::txtest;

namespace
{
uint32_t const GATE_PROTOCOL =
    static_cast<uint32_t>(APPLY_TIME_TX_SET_VALIDATION_PROTOCOL_VERSION);

Config
configForProtocol(int configNum, Config::TestDbMode mode, uint32_t protocol)
{
    auto cfg = getTestConfig(configNum, mode);
    cfg.LEDGER_PROTOCOL_VERSION = protocol;
    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION = protocol;
    return cfg;
}

Hash
emptyResultSetHash()
{
    return xdrSha256(TransactionResultSet{});
}

int64_t
skippedTxSets(Application& app)
{
    return app.getMetrics()
        .NewCounter({"ledger", "apply", "txset-skipped"})
        .count();
}

// A transaction set on top of the LCL that holds `txs` in its classic phase.
// Nothing about it is validated.
TxSetXDRFrameConstPtr
makeUnvalidatedTxSet(Application& app,
                     std::vector<TransactionFrameBasePtr> const& txs)
{
    auto res = testtxset::makeNonValidatedGeneralizedTxSet(
        {{std::make_pair(std::nullopt, txs)}, {}}, app,
        app.getLedgerManager().getLastClosedLedgerHeader().hash);
    REQUIRE(res.first);
    return res.first;
}

// A payment of 1 stroop from `from` to the root account with an explicit
// sequence number.
TransactionFrameBasePtr
paymentWithSeq(Application& app, TestAccount& from, SequenceNumber seq)
{
    return transactionFromOperations(
        app, from.getSecretKey(), seq,
        {payment(app.getRoot()->getPublicKey(), 1)}, 100);
}

struct AccountState
{
    int64_t mBalance;
    SequenceNumber mSeq;
    bool
    operator==(AccountState const& other) const
    {
        return mBalance == other.mBalance && mSeq == other.mSeq;
    }
};

AccountState
stateOf(TestAccount& acc)
{
    return AccountState{acc.getBalance(), acc.getLastSequenceNumber()};
}
}

TEST_CASE("invalid tx set applies as an empty ledger",
          "[herder][txset][apply-time-validation]")
{
    // Both the in-memory ledger (validation reads through a LedgerTxn on the
    // main thread) and the BucketList-backed one (validation reads apply's
    // snapshot, on the apply thread when ledgers apply in the background).
    auto mode =
        GENERATE(Config::TESTDB_IN_MEMORY, Config::TESTDB_BUCKET_DB_PERSISTENT);
    CAPTURE(mode);

    VirtualClock clock;
    auto app =
        createTestApplication(clock, configForProtocol(0, mode, GATE_PROTOCOL));
    auto& lm = app->getLedgerManager();
    auto root = app->getRoot();
    auto const minBalance = lm.getLastMinBalance(2);
    auto a = root->create("A", minBalance + 10'000'000);
    auto b = root->create("B", minBalance + 10'000'000);

    auto const aBefore = stateOf(a);
    auto const bBefore = stateOf(b);
    auto const rootBalanceBefore = root->getBalance();
    auto const skippedBefore = skippedTxSets(*app);

    // One valid payment and one whose sequence number is too far ahead: the
    // set as a whole is invalid.
    auto good = paymentWithSeq(*app, a, aBefore.mSeq + 1);
    auto bad = paymentWithSeq(*app, b, bBefore.mSeq + 10);

    SECTION("a set with an invalid transaction")
    {
        auto txSet = makeUnvalidatedTxSet(*app, {good, bad});
        {
            auto applicable = txSet->prepareForApply(
                *app, lm.getLastClosedLedgerHeader().header);
            REQUIRE(applicable);
            REQUIRE(applicable->checkValidWithResult(*app, 0, 0) ==
                    TxSetValidationResult::TX_VALIDATION_FAILED);
        }

        auto const lclBefore = lm.getLastClosedLedgerHeader();
        auto results = closeLedger(*app, txSet);

        // The ledger closed, carrying the externalized value, but nothing of
        // the set was applied: no results, no fees, no sequence bumps.
        auto const& lcl = lm.getLastClosedLedgerHeader();
        REQUIRE(lcl.header.ledgerSeq == lclBefore.header.ledgerSeq + 1);
        REQUIRE(lcl.header.previousLedgerHash == lclBefore.hash);
        REQUIRE(lcl.header.scpValue.txSetHash == txSet->getContentsHash());
        REQUIRE(!isEmptyTxSetStellarValue(lcl.header.scpValue));
        REQUIRE(lcl.header.txSetResultHash == emptyResultSetHash());
        REQUIRE(lcl.header.feePool == lclBefore.header.feePool);
        REQUIRE(results.results.empty());
        REQUIRE(stateOf(a) == aBefore);
        REQUIRE(stateOf(b) == bBefore);
        REQUIRE(root->getBalance() == rootBalanceBefore);
        REQUIRE(skippedTxSets(*app) == skippedBefore + 1);

        // Meta keeps the externalized set, with no transaction processed.
        auto lcm = lm.getLastClosedLedgerCloseMeta();
        REQUIRE(lcm);
        REQUIRE(lcm->getTransactionResultMetaCount() == 0);
        REQUIRE(lcm->getXDR().v() >= 1);
        GeneralizedTransactionSet metaTxSet;
        switch (lcm->getXDR().v())
        {
        case 1:
            metaTxSet = lcm->getXDR().v1().txSet;
            break;
        case 2:
            metaTxSet = lcm->getXDR().v2().txSet;
            break;
        default:
            REQUIRE(false);
        }
        REQUIRE(xdrSha256(metaTxSet) == txSet->getContentsHash());

        // The next ledger applies normally, and the valid payment of the
        // skipped set is still valid.
        auto validSet = makeUnvalidatedTxSet(*app, {good});
        results = closeLedger(*app, validSet);
        REQUIRE(results.results.size() == 1);
        REQUIRE(results.results[0].result.result.code() == txSUCCESS);
        REQUIRE(stateOf(a).mSeq == aBefore.mSeq + 1);
        REQUIRE(stateOf(a).mBalance == aBefore.mBalance - 100 - 1);
        REQUIRE(skippedTxSets(*app) == skippedBefore + 1);
        REQUIRE(lm.getLastClosedLedgerHeader().header.txSetResultHash !=
                emptyResultSetHash());
    }

    SECTION("a valid set is applied")
    {
        auto txSet = makeUnvalidatedTxSet(*app, {good});
        auto results = closeLedger(*app, txSet);
        REQUIRE(results.results.size() == 1);
        REQUIRE(results.results[0].result.result.code() == txSUCCESS);
        REQUIRE(stateOf(a).mSeq == aBefore.mSeq + 1);
        REQUIRE(skippedTxSets(*app) == skippedBefore);
    }

    SECTION("a malformed set")
    {
        // A generalized set must have exactly two phases: this one can't
        // even be prepared for apply.
        GeneralizedTransactionSet xdrTxSet(1);
        xdrTxSet.v1TxSet().previousLedgerHash =
            lm.getLastClosedLedgerHeader().hash;
        xdrTxSet.v1TxSet().phases.emplace_back(0);
        auto txSet = TxSetXDRFrame::makeFromWire(xdrTxSet);
        REQUIRE(!txSet->prepareForApply(*app,
                                        lm.getLastClosedLedgerHeader().header));

        auto results = closeLedger(*app, txSet);
        auto const& lcl = lm.getLastClosedLedgerHeader();
        REQUIRE(lcl.header.scpValue.txSetHash == txSet->getContentsHash());
        REQUIRE(lcl.header.txSetResultHash == emptyResultSetHash());
        REQUIRE(results.results.empty());
        REQUIRE(skippedTxSets(*app) == skippedBefore + 1);
    }

    SECTION("upgrades of a skipped value still apply")
    {
        auto txSet = makeUnvalidatedTxSet(*app, {good, bad});
        auto const lclBefore = lm.getLastClosedLedgerHeader();
        uint32_t const newBaseFee = lclBefore.header.baseFee + 7;
        LedgerUpgrade upgrade(LEDGER_UPGRADE_BASE_FEE);
        upgrade.newBaseFee() = newBaseFee;
        xdr::xvector<UpgradeType, 6> upgrades;
        upgrades.emplace_back(LedgerTestUtils::toUpgradeType(upgrade));

        app->getHerder().externalizeValue(
            txSet, lclBefore.header.ledgerSeq + 1,
            ConsensusTime::fromApplyTime(ApplyTime::fromTimePoint(
                lclBefore.header.scpValue.closeTime + 1)),
            upgrades);

        auto const& lcl = lm.getLastClosedLedgerHeader();
        REQUIRE(lcl.header.ledgerSeq == lclBefore.header.ledgerSeq + 1);
        REQUIRE(lcl.header.scpValue.txSetHash == txSet->getContentsHash());
        REQUIRE(lcl.header.txSetResultHash == emptyResultSetHash());
        REQUIRE(lcl.header.baseFee == newBaseFee);
        REQUIRE(stateOf(a) == aBefore);
        REQUIRE(skippedTxSets(*app) == skippedBefore + 1);
    }
}

// The verdict depends on the value as well as on the set: transactions are
// checked at the close time the value carries.
TEST_CASE("apply validates a tx set at the close time of its value",
          "[herder][txset][apply-time-validation]")
{
    VirtualClock clock;
    auto app = createTestApplication(
        clock, configForProtocol(0, Config::TESTDB_DEFAULT, GATE_PROTOCOL));
    auto& lm = app->getLedgerManager();
    auto root = app->getRoot();
    auto a = root->create("A", lm.getLastMinBalance(2) + 10'000'000);
    auto const aBefore = stateOf(a);
    auto const lcl = lm.getLastClosedLedgerHeader();
    auto const lclCloseTime = lcl.header.scpValue.closeTime;
    auto const skippedBefore = skippedTxSets(*app);

    // A payment that expires two seconds after the LCL closed.
    auto tx =
        transactionFromOperations(*app, a.getSecretKey(), aBefore.mSeq + 1,
                                  {payment(root->getPublicKey(), 1)}, 100);
    setMaxTime(tx, lclCloseTime + 2);
    auto& sig = tx->getMutableEnvelope().type() == ENVELOPE_TYPE_TX_V0
                    ? tx->getMutableEnvelope().v0().signatures
                    : tx->getMutableEnvelope().v1().signatures;
    sig.clear();
    tx->addSignature(a.getSecretKey());
    auto txSet = makeUnvalidatedTxSet(*app, {tx});

    auto closeAt = [&](TimePoint closeTime) {
        app->getHerder().externalizeValue(
            txSet, lcl.header.ledgerSeq + 1,
            ConsensusTime::fromApplyTime(ApplyTime::fromTimePoint(closeTime)),
            emptyUpgradeSteps);
        REQUIRE(lm.getLastClosedLedgerNum() == lcl.header.ledgerSeq + 1);
        REQUIRE(lm.getLastClosedLedgerHeader().header.scpValue.txSetHash ==
                txSet->getContentsHash());
    };

    SECTION("the ledger closes before the transaction expires")
    {
        closeAt(lclCloseTime + 2);
        REQUIRE(skippedTxSets(*app) == skippedBefore);
        REQUIRE(stateOf(a).mSeq == aBefore.mSeq + 1);
        REQUIRE(lm.getLastClosedLedgerHeader().header.txSetResultHash !=
                emptyResultSetHash());
    }
    SECTION("the ledger closes after the transaction expired")
    {
        closeAt(lclCloseTime + 3);
        REQUIRE(skippedTxSets(*app) == skippedBefore + 1);
        REQUIRE(stateOf(a) == aBefore);
        REQUIRE(lm.getLastClosedLedgerHeader().header.txSetResultHash ==
                emptyResultSetHash());
    }
}

TEST_CASE("invalid tx set is applied before the protocol gate",
          "[herder][txset][apply-time-validation]")
{
    // Before the gate nothing is validated at apply time: the set was
    // validated before the vote, and apply processes whatever externalized.
    VirtualClock clock;
    auto app = createTestApplication(
        clock, configForProtocol(0, Config::TESTDB_DEFAULT, GATE_PROTOCOL - 1));
    auto& lm = app->getLedgerManager();
    auto root = app->getRoot();
    auto const minBalance = lm.getLastMinBalance(2);
    auto a = root->create("A", minBalance + 10'000'000);
    auto b = root->create("B", minBalance + 10'000'000);
    auto const aSeq = a.getLastSequenceNumber();
    auto const bBalance = b.getBalance();

    auto good = paymentWithSeq(*app, a, aSeq + 1);
    auto bad = paymentWithSeq(*app, b, b.getLastSequenceNumber() + 10);
    auto txSet = makeUnvalidatedTxSet(*app, {good, bad});

    auto results = closeLedger(*app, txSet);
    REQUIRE(results.results.size() == 2);
    REQUIRE(lm.getLastClosedLedgerHeader().header.txSetResultHash !=
            emptyResultSetHash());
    REQUIRE(a.getLastSequenceNumber() == aSeq + 1);
    // the invalid transaction still paid its fee
    REQUIRE(b.getBalance() == bBalance - 100);
    REQUIRE(skippedTxSets(*app) == 0);
}

TEST_CASE("tx sets are not validated before voting from the protocol gate",
          "[herder][txset][apply-time-validation]")
{
    auto runTest = [](uint32_t protocol, bool validatedBeforeVoting) {
        VirtualClock clock;
        auto cfg = configForProtocol(0, Config::TESTDB_DEFAULT, protocol);
        auto app = createTestApplication(clock, cfg);
        auto& lm = app->getLedgerManager();
        auto& herder = static_cast<HerderImpl&>(app->getHerder());
        auto& driver = herder.getHerderSCPDriver();
        auto root = app->getRoot();
        auto a = root->create("A", lm.getLastMinBalance(2) + 10'000'000);

        auto const& lcl = lm.getLastClosedLedgerHeader();
        uint64_t const slot = lcl.header.ledgerSeq + 1;
        auto const closeTime = ConsensusTime::fromApplyTime(
            ApplyTime::fromTimePoint(lcl.header.scpValue.closeTime + 1));
        auto valueFor = [&](TxSetXDRFrameConstPtr const& txSet) {
            herder.getPendingEnvelopes().putTxSet(txSet->getContentsHash(),
                                                  slot, txSet);
            return xdr::xdr_to_opaque(
                herder.makeStellarValue(txSet->getContentsHash(), closeTime,
                                        emptyUpgradeSteps, cfg.NODE_SEED));
        };

        auto bad = paymentWithSeq(*app, a, a.getLastSequenceNumber() + 10);
        auto invalidSet = makeUnvalidatedTxSet(*app, {bad});
        auto good = paymentWithSeq(*app, a, a.getLastSequenceNumber() + 1);
        auto validSet = makeUnvalidatedTxSet(*app, {good});

        for (bool proposal : {false, true})
        {
            CAPTURE(proposal);
            REQUIRE(driver.validateValue(slot, valueFor(validSet), proposal) ==
                    SCPDriver::kFullyValidatedValue);
            // An invalid set is voted for like any other from the gate on;
            // before it, the ballot protocol only keeps it as a candidate
            // for replacement by the empty-tx-set value.
            REQUIRE(
                driver.validateValue(slot, valueFor(invalidSet), proposal) ==
                (validatedBeforeVoting ? SCPDriver::kStructurallyValidValue
                                       : SCPDriver::kFullyValidatedValue));
        }

        // A set that is not built on the LCL is still not voted for: that is
        // not a question of the validity of its transactions.
        Hash otherLedger = sha256("some other ledger");
        auto otherSet =
            testtxset::makeNonValidatedGeneralizedTxSet(
                {{std::make_pair(std::nullopt,
                                 std::vector<TransactionFrameBasePtr>{good})},
                 {}},
                *app, lcl.hash)
                .first;
        GeneralizedTransactionSet otherXdr;
        otherSet->toXDR(otherXdr);
        otherXdr.v1TxSet().previousLedgerHash = otherLedger;
        auto wrongBase = TxSetXDRFrame::makeFromWire(otherXdr);
        REQUIRE(driver.validateValue(slot, valueFor(wrongBase), false) ==
                SCPDriver::kStructurallyValidValue);
    };

    SECTION("before the protocol gate")
    {
        runTest(GATE_PROTOCOL - 1, /* validatedBeforeVoting */ true);
    }
    SECTION("from the protocol gate")
    {
        runTest(GATE_PROTOCOL, /* validatedBeforeVoting */ false);
    }
}

// A leader that proposes an invalid transaction set gets it externalized
// (nobody validates it before voting), and every node closes that ledger as
// an empty one, with the same ledger hash.
TEST_CASE("network externalizes an invalid tx set and applies it as empty",
          "[herder][txset][apply-time-validation][simulation]")
{
    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    auto simulation = Topologies::core(4, 0.75, networkID, [](int i) {
        auto cfg = configForProtocol(i, Config::TESTDB_DEFAULT, GATE_PROTOCOL);
        cfg.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = true;
        return cfg;
    });
    auto nodes = simulation->getNodes();

    // Node 0 proposes, whenever it leads, a set with one payment from the
    // root account whose sequence number is too far ahead.
    auto& badLeader = *nodes[0];
    std::set<Hash> invalidSets;
    static_cast<HerderImpl&>(badLeader.getHerder()).mProposedTxSetOverride =
        [&](LedgerHeaderHistoryEntry const& lcl) {
            auto root = badLeader.getRoot();
            auto bad = paymentWithSeq(badLeader, *root,
                                      root->getLastSequenceNumber() + 10);
            auto txSet = testtxset::makeNonValidatedGeneralizedTxSet(
                             {{std::make_pair(
                                  std::nullopt,
                                  std::vector<TransactionFrameBasePtr>{bad})},
                              {}},
                             badLeader, lcl.hash)
                             .first;
            invalidSets.insert(txSet->getContentsHash());
            return txSet;
        };
    simulation->startAllNodes();

    // Watch every ledger an honest node closes.
    auto& lm = nodes[1]->getLedgerManager();
    uint32_t seen = lm.getLastClosedLedgerNum();
    uint32_t const target = seen + 20;
    uint32_t invalidExternalized = 0;
    simulation->crankUntil(
        [&]() {
            auto const& lcl = lm.getLastClosedLedgerHeader();
            if (lcl.header.ledgerSeq > seen)
            {
                REQUIRE(lcl.header.ledgerSeq == seen + 1);
                seen = lcl.header.ledgerSeq;
                if (invalidSets.count(lcl.header.scpValue.txSetHash))
                {
                    ++invalidExternalized;
                    // closed without applying the set
                    REQUIRE(lcl.header.txSetResultHash == emptyResultSetHash());
                }
            }
            return seen >= target;
        },
        30 * simulation->getExpectedLedgerCloseTime(), false);
    REQUIRE(seen >= target);

    // Node 0 led some of those ledgers, and its invalid sets went through
    // consensus.
    CAPTURE(invalidExternalized);
    REQUIRE(invalidExternalized > 0);

    // All nodes skipped the same ledgers and agree on the ledger chain.
    simulation->crankUntil(
        [&]() { return simulation->haveAllExternalized(target, 1); },
        10 * simulation->getExpectedLedgerCloseTime(), false);
    auto const expected = lm.getLastClosedLedgerHeader();
    for (auto const& node : nodes)
    {
        auto& nodeLm = node->getLedgerManager();
        simulation->crankUntil(
            [&]() {
                return nodeLm.getLastClosedLedgerNum() >=
                       expected.header.ledgerSeq;
            },
            10 * simulation->getExpectedLedgerCloseTime(), false);
        REQUIRE(skippedTxSets(*node) >= invalidExternalized);
        if (nodeLm.getLastClosedLedgerNum() == expected.header.ledgerSeq)
        {
            REQUIRE(nodeLm.getLastClosedLedgerHeader().hash == expected.hash);
        }
    }
    // the root account never paid for any of them
    REQUIRE(nodes[1]->getRoot()->getLastSequenceNumber() ==
            nodes[0]->getRoot()->getLastSequenceNumber());
}

// The overlay process writes every tx set to disk (in its compressed form)
// before Core can vote for it, so Core no longer stores tx sets in its
// database with the SCP state; after a restart it gets the sets its restored
// SCP state names back from the overlay's store.
TEST_CASE("tx sets are stored by the overlay and come back after a restart",
          "[herder][overlay-ipc][txset-store]")
{
    auto cfg = getTestConfig(0, Config::TESTDB_BUCKET_DB_PERSISTENT);
    cfg.RUN_STANDALONE = false;
    cfg.MANUAL_CLOSE = false;
    cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS = false;
    cfg.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = true;
    // {self, peer} with threshold 2 and no peer around: the node proposes
    // and votes, but never closes a ledger.
    auto const peerKey = SecretKey::fromSeed(sha256("tx set store peer"));
    cfg.QUORUM_SET.validators.emplace_back(peerKey.getPublicKey());
    cfg.QUORUM_SET.threshold = 2;

    auto const storeDir = std::filesystem::path(cfg.BUCKET_DIR_PATH) / "txsets";
    uint64_t slot = 0;
    Hash txSetHash;
    Value value;

    {
        VirtualClock clock(VirtualClock::REAL_TIME);
        auto app = createTestApplication(clock, cfg);
        auto& herder = static_cast<HerderImpl&>(app->getHerder());
        REQUIRE(app->getOverlayManager().storesTxSets());
        slot = app->getLedgerManager().getLastClosedLedgerNum() + 1;

        // Wait for the node to vote for a value of its own.
        std::optional<SCPEnvelope> emitted;
        testutil::crankUntil(
            *app,
            [&]() {
                for (auto const& e :
                     herder.getSCP().getLatestMessagesSend(slot))
                {
                    if (e.statement.pledges.type() == SCP_ST_PREPARE)
                    {
                        emitted = e;
                    }
                }
                return emitted.has_value();
            },
            std::chrono::seconds(60));
        REQUIRE(emitted);
        REQUIRE(app->getLedgerManager().getLastClosedLedgerNum() == slot - 1);
        value = emitted->statement.pledges.prepare().ballot.value;
        StellarValue sv;
        xdr::xdr_from_opaque(value, sv);
        REQUIRE(!isEmptyTxSetStellarValue(sv));
        txSetHash = sv.txSetHash;

        // The SCP state went to the database without the tx set, which is on
        // disk in the overlay's store instead.
        REQUIRE(!app->getPersistentState().getSCPStateAllSlots().empty());
        REQUIRE(!app->getPersistentState().hasTxSet(txSetHash));
        REQUIRE(app->getPersistentState().getTxSetsForAllSlots().empty());
        // (The overlay is another process: it writes the set before it lets
        // the vote out of the node, which is not before Core sees its own
        // statement.)
        auto const stored = storeDir / (binToHex(txSetHash) + ".txset");
        CAPTURE(stored.string());
        testutil::crankUntil(
            *app, [&]() { return std::filesystem::exists(stored); },
            std::chrono::seconds(30));
        REQUIRE(std::filesystem::exists(stored));
        REQUIRE(std::filesystem::file_size(stored) > 4);
    }

    // Restart on the same database and store.
    VirtualClock clock(VirtualClock::REAL_TIME);
    auto app = createTestApplication(clock, cfg, /*newDB*/ false);
    auto& herder = static_cast<HerderImpl&>(app->getHerder());
    REQUIRE(app->getLedgerManager().getLastClosedLedgerNum() == slot - 1);
    // The ballot state was restored
    REQUIRE(!herder.getSCP().getLatestMessagesSend(slot).empty());

    // and the tx set it names arrives from the overlay, which read it back
    // from its store (there is no peer to fetch it from).
    auto restoredSet = [&]() -> TxSetXDRFrameConstPtr {
        auto res = herder.getPendingEnvelopes().getTxSet(txSetHash);
        auto const* frame = std::get_if<TxSetXDRFrameConstPtr>(&res);
        return frame ? *frame : nullptr;
    };
    testutil::crankUntil(
        *app, [&]() { return restoredSet() != nullptr; },
        std::chrono::seconds(30));
    auto txSet = restoredSet();
    REQUIRE(txSet);
    REQUIRE(txSet->getContentsHash() == txSetHash);
    REQUIRE(!app->getPersistentState().hasTxSet(txSetHash));
}
