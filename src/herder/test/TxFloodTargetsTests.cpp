// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// Tests of transaction dissemination to the upcoming round leaders: a node
// checks a submitted transaction against its last closed ledger, keeps it,
// and has its overlay send it to the leaders of the current and the next
// round only (HerderImpl::computeTxFloodTargets) instead of flooding it to
// every peer.

#include "crypto/SHA.h"
#include "herder/Herder.h"
#include "herder/HerderImpl.h"
#include "herder/LeaderSchedule.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "overlay/RustOverlayManager.h"
#include "simulation/LoadGenerator.h"
#include "simulation/Simulation.h"
#include "simulation/Topologies.h"
#include "test/Catch2.h"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/TransactionUtils.h"
#include "util/BatchExecutor.h"
#include "util/Logging.h"
#include "util/MetricsRegistry.h"
#include <fmt/format.h>
#include <map>

using namespace stellar;
using namespace stellar::txtest;

namespace
{
int const NUM_ACCOUNTS = 40;
// Config::TX_FLOOD_TARGET_LEADERS's default.
uint32_t const TARGET_LEADERS = 2;

Config
acceleratedConfig(int configNum)
{
    auto cfg = getTestConfig(configNum);
    cfg.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = true;
    cfg.GENESIS_TEST_ACCOUNT_COUNT = NUM_ACCOUNTS;
    return cfg;
}

HerderImpl&
herderOf(Application::pointer const& app)
{
    return static_cast<HerderImpl&>(app->getHerder());
}

// Network totals of the transaction dissemination counters.
struct TxFloodCounts
{
    // Bodies sent straight to a target, and received without a demand.
    int64_t mDirectSent{0};
    int64_t mDirectRecv{0};
    // The flood: adverts sent, bodies demanded, own transactions flooded.
    int64_t mAdvertised{0};
    int64_t mDemanded{0};
    int64_t mLocalFlooded{0};

    std::string
    toString() const
    {
        return fmt::format(
            "direct sent {} received {}; advertised {} demanded {} flooded {}",
            mDirectSent, mDirectRecv, mAdvertised, mDemanded, mLocalFlooded);
    }
};

TxFloodCounts
countTxFlood(std::vector<Application::pointer> const& nodes)
{
    TxFloodCounts res;
    for (auto const& app : nodes)
    {
        app->getOverlayManager().syncOverlayMetrics();
        auto& m = app->getOverlayManager().getOverlayMetrics();
        res.mDirectSent += static_cast<int64_t>(m.mTxDirectSent.count());
        res.mDirectRecv += static_cast<int64_t>(m.mTxDirectRecv.count());
        res.mAdvertised +=
            static_cast<int64_t>(m.mSendFloodAdvertMeter.count());
        res.mDemanded += static_cast<int64_t>(m.mMessagesDemanded.count());
        res.mLocalFlooded += static_cast<int64_t>(m.mTxLocalFlooded.count());
    }
    return res;
}

int64_t
identifiedPeers(Application::pointer const& app)
{
    app->getOverlayManager().syncOverlayMetrics();
    return app->getOverlayManager()
        .getOverlayMetrics()
        .mNodeIdIdentified.count();
}

// Whether the slot starts a new leader term (LeaderSchedule.h: every
// multiple of LEADER_TERM_LEDGERS does).
bool
startsLeaderTerm(uint64_t slot)
{
    return slot % LEADER_TERM_LEDGERS == 0;
}

// Whether `update.targets` is what the leader schedule gives for the state
// the node was in. Computed from the LCL alone, with the schedule functions
// (not with the Herder's own schedule cache):
//  1. the leaders of the node's round of the slot after the LCL and of the
//     round after it, then
//  2. if the slot after that starts a new leader term: the first node of the
//     order seeded with the LCL's hash, unless it is in the list already.
bool
isScheduledTargetList(HerderImpl::TxFloodTargetUpdate const& update,
                      std::vector<NodeID> const& validators)
{
    if (!update.targets)
    {
        return false;
    }
    REQUIRE(update.slot == update.lcl.header.ledgerSeq + 1);
    auto const schedule = computeNextLeaderSchedule(
        update.lcl.header, validators, /*ledgersHaveResults=*/true);
    REQUIRE(schedule.size() == validators.size());
    std::vector<NodeID> expected;
    auto const round = std::max<uint32_t>(1, update.round);
    for (uint64_t r = round; r < uint64_t(round) + TARGET_LEADERS; ++r)
    {
        auto const& leader = schedule[(r - 1) % schedule.size()];
        if (std::find(expected.begin(), expected.end(), leader) !=
            expected.end())
        {
            break;
        }
        expected.emplace_back(leader);
    }
    if (startsLeaderTerm(update.slot + 1))
    {
        auto const next = computeLeaderSchedule(update.lcl.hash, validators);
        if (std::find(expected.begin(), expected.end(), next.front()) ==
            expected.end())
        {
            expected.emplace_back(next.front());
        }
    }
    return *update.targets == expected;
}

// What the lists a node sent say about the first leader of each new term,
// one ledger ahead: for every list sent with the LCL at N, where N+2 starts
// a term and the list of N+2's real leaders is known (a list sent with the
// LCL at N+1), whether it named the node that leads round 1 of N+2. The
// prediction is only wrong when ledger N+1 cost that node its turn (a
// ledger that applies nothing sends its own leader to the end).
struct TermPredictions
{
    int mRight = 0;
    int mWrong = 0;
    int mWrongWithoutForfeit = 0;
};

TermPredictions
checkTermPredictions(std::deque<HerderImpl::TxFloodTargetUpdate> const& history,
                     std::vector<NodeID> const& validators)
{
    std::map<uint32_t, LedgerHeader> headers;
    for (auto const& update : history)
    {
        headers[update.lcl.header.ledgerSeq] = update.lcl.header;
    }
    TermPredictions res;
    for (auto const& update : history)
    {
        auto next = headers.find(update.lcl.header.ledgerSeq + 1);
        if (!update.targets || !startsLeaderTerm(update.slot + 1) ||
            next == headers.end())
        {
            continue;
        }
        auto const real = computeNextLeaderSchedule(
            next->second, validators, /*ledgersHaveResults=*/true);
        if (std::find(update.targets->begin(), update.targets->end(),
                      real.front()) != update.targets->end())
        {
            ++res.mRight;
        }
        else
        {
            ++res.mWrong;
            if (!leaderTermOutcome(next->second, true).mForfeited)
            {
                ++res.mWrongWithoutForfeit;
            }
        }
    }
    return res;
}

std::vector<NodeID>
validatorsOf(std::vector<Application::pointer> const& nodes)
{
    std::vector<NodeID> res;
    for (auto const& app : nodes)
    {
        res.emplace_back(app->getConfig().NODE_SEED.getPublicKey());
    }
    return res;
}

// Whether every node closed ledger `seq`. (Simulation::haveAllExternalized
// throws once the nodes are more than a few ledgers past the number it is
// asked about, which happens here while waiting for something else.)
bool
allClosed(std::vector<Application::pointer> const& nodes, uint32_t seq)
{
    return std::all_of(nodes.begin(), nodes.end(), [&](auto const& app) {
        return app->getLedgerManager().getLastClosedLedgerNum() >= seq;
    });
}

int64_t
appliedTxs(Application::pointer const& app)
{
    return static_cast<int64_t>(
        app->getMetrics()
            .NewCounter({"ledger", "apply", "success"})
            .count());
}
}

TEST_CASE("transactions go to the round leaders only",
          "[herder][tx-flood-targets][simulation]")
{
    // 4 validators, every node connected to every other one.
    auto simulation =
        Topologies::core(4, 0.75, sha256(getTestConfig().NETWORK_PASSPHRASE),
                         acceleratedConfig);
    simulation->startAllNodes();
    auto nodes = simulation->getNodes();

    // Let the network close a few ledgers, every overlay learn which
    // validator each of its peers is, and every Herder publish targets.
    simulation->crankUntil(
        [&]() {
            if (!allClosed(nodes, 4))
            {
                return false;
            }
            for (auto const& app : nodes)
            {
                if (identifiedPeers(app) != 3 ||
                    !herderOf(app).getTxFloodTargetsForTesting())
                {
                    return false;
                }
            }
            return true;
        },
        std::chrono::seconds(120), false);
    for (auto const& app : nodes)
    {
        REQUIRE(identifiedPeers(app) == 3);
    }
    auto const before = countTxFlood(nodes);
    INFO("before: " << before.toString());
    // Nothing was submitted so far.
    REQUIRE(before.mDirectSent == 0);
    REQUIRE(before.mAdvertised == 0);

    // The one function that decides: the leaders of two rounds of the slot
    // after the LCL (two of the four nodes) and, one ledger before a new
    // leader term, that term's first leader.
    for (auto const& app : nodes)
    {
        auto& herder = herderOf(app);
        auto const state = herder.describeTxFloodTargetsForTesting(
            herder.computeTxFloodTargets());
        REQUIRE(state.targets);
        REQUIRE(isScheduledTargetList(state, validatorsOf(nodes)));
        REQUIRE(state.targets->size() >= TARGET_LEADERS);
        REQUIRE(state.targets->size() <=
                TARGET_LEADERS + (startsLeaderTerm(state.slot + 1) ? 1 : 0));
    }

    // Submit payments at every node over several ledgers, each from its own
    // account. A node is often not a leader for the ledger it submits in.
    auto const appliedBefore = appliedTxs(nodes[0]);
    int submitted = 0;
    auto const startLedger =
        nodes[0]->getLedgerManager().getLastClosedLedgerNum();
    for (int batch = 0; batch < 8; ++batch)
    {
        for (size_t n = 0; n < nodes.size(); ++n)
        {
            auto source = getGenesisAccount(*nodes[n], submitted);
            auto dest = getGenesisAccount(*nodes[n], NUM_ACCOUNTS - 1);
            auto tx = source.tx({payment(dest.getPublicKey(), 1000)});
            REQUIRE(nodes[n]->getHerder().recvTransaction(tx, false) ==
                    TxSubmitStatus::TX_STATUS_PENDING);
            ++submitted;
        }
        auto const next =
            nodes[0]->getLedgerManager().getLastClosedLedgerNum() + 1;
        simulation->crankUntil(
            [&]() { return allClosed(nodes, next); },
            std::chrono::seconds(60), false);
    }
    REQUIRE(submitted < NUM_ACCOUNTS - 1);

    // Every transaction is applied on every node, although only the leaders
    // ever saw it outside a tx set.
    simulation->crankUntil(
        [&]() {
            for (auto const& app : nodes)
            {
                if (appliedTxs(app) - appliedBefore < submitted)
                {
                    return false;
                }
            }
            return true;
        },
        std::chrono::seconds(120), false);
    for (auto const& app : nodes)
    {
        REQUIRE(appliedTxs(app) - appliedBefore == submitted);
    }
    // Go on until a node was sent a list with a third node: the first
    // leader of a new term that is not one of the two round leaders of the
    // ledger before it (every second term start, with four nodes).
    auto sawNextTermLeader = [&]() {
        for (auto const& app : nodes)
        {
            for (auto const& update :
                 herderOf(app).mTxFloodTargetHistoryForTesting)
            {
                if (update.targets && update.targets->size() > TARGET_LEADERS)
                {
                    return true;
                }
            }
        }
        return false;
    };
    for (int i = 0; i < 150 && !sawNextTermLeader(); ++i)
    {
        auto const next =
            nodes[0]->getLedgerManager().getLastClosedLedgerNum() + 1;
        simulation->crankUntil([&]() { return allClosed(nodes, next); },
                               std::chrono::seconds(60), false);
    }
    auto const ledgers =
        nodes[0]->getLedgerManager().getLastClosedLedgerNum() - startLedger;

    auto const after = countTxFlood(nodes);
    INFO("after: " << after.toString() << ", " << submitted
                   << " transactions in " << ledgers << " ledgers");
    // The network is a full mesh of identified validators, so nothing was
    // advertised, demanded or flooded: every transaction was sent as a body
    // to the targets.
    REQUIRE(after.mAdvertised == 0);
    REQUIRE(after.mDemanded == 0);
    REQUIRE(after.mLocalFlooded == 0);
    // At least one send per transaction that its submitter did not lead
    // itself; at most three targets (two round leaders and the next term's
    // first leader) per ledger it stayed pending.
    REQUIRE(after.mDirectSent > 0);
    // Nothing is sent twice to a peer within the test's run time (60 s
    // between sends), so every body sent is one body received.
    REQUIRE(after.mDirectRecv == after.mDirectSent);
    REQUIRE(after.mDirectSent <=
            static_cast<int64_t>(submitted) * (TARGET_LEADERS + 1) *
                (ledgers + 1));

    // Every target list a node sent to its overlay during the run is what
    // the schedule gives for the state the node was in: two nodes, and a
    // third one only in the ledger before a new term. The test ran through
    // at least one term start.
    int checked = 0;
    int beforeNewTerm = 0;
    int withNextTermLeader = 0;
    TermPredictions predictions;
    for (auto const& app : nodes)
    {
        for (auto const& update :
             herderOf(app).mTxFloodTargetHistoryForTesting)
        {
            if (!update.targets)
            {
                continue;
            }
            INFO(fmt::format("slot {}, round {}, {} targets", update.slot,
                             update.round, update.targets->size()));
            REQUIRE(isScheduledTargetList(update, validatorsOf(nodes)));
            ++checked;
            if (startsLeaderTerm(update.slot + 1))
            {
                ++beforeNewTerm;
                withNextTermLeader +=
                    update.targets->size() > TARGET_LEADERS ? 1 : 0;
            }
            else
            {
                REQUIRE(update.targets->size() <= TARGET_LEADERS);
            }
        }
        auto const p = checkTermPredictions(
            herderOf(app).mTxFloodTargetHistoryForTesting,
            validatorsOf(nodes));
        predictions.mRight += p.mRight;
        predictions.mWrong += p.mWrong;
        predictions.mWrongWithoutForfeit += p.mWrongWithoutForfeit;
    }
    INFO(fmt::format("target lists checked {}, in the ledger before a new "
                     "term {} ({} with a third node); the new term's first "
                     "leader was in the list {} times, not {} times ({} "
                     "without a forfeit)",
                     checked, beforeNewTerm, withNextTermLeader,
                     predictions.mRight, predictions.mWrong,
                     predictions.mWrongWithoutForfeit));
    REQUIRE(beforeNewTerm > 0);
    REQUIRE(withNextTermLeader > 0);
    // A node that was told one ledger ahead who leads the new term was told
    // right, unless the ledger in between cost that node its turn.
    REQUIRE(predictions.mRight > 0);
    REQUIRE(predictions.mWrongWithoutForfeit == 0);
}

TEST_CASE("transactions go to every validator when no leader count is set",
          "[herder][tx-flood-targets][simulation]")
{
    // TX_FLOOD_TARGET_LEADERS = 0: every validator is a target. Still no
    // adverts and no demands, and every transaction goes to each of the
    // three other nodes exactly once.
    auto simulation = Topologies::core(
        4, 0.75, sha256(getTestConfig().NETWORK_PASSPHRASE), [](int i) {
            auto cfg = acceleratedConfig(i);
            cfg.TX_FLOOD_TARGET_LEADERS = 0;
            return cfg;
        });
    simulation->startAllNodes();
    auto nodes = simulation->getNodes();
    simulation->crankUntil(
        [&]() {
            if (!allClosed(nodes, 4))
            {
                return false;
            }
            for (auto const& app : nodes)
            {
                if (identifiedPeers(app) != 3 ||
                    !herderOf(app).getTxFloodTargetsForTesting())
                {
                    return false;
                }
            }
            return true;
        },
        std::chrono::seconds(120), false);
    for (auto const& app : nodes)
    {
        auto targets = herderOf(app).computeTxFloodTargets();
        REQUIRE(targets);
        REQUIRE(targets->size() == 4);
        REQUIRE(std::is_sorted(targets->begin(), targets->end()));
        // The same list whatever the slot, so it was sent once.
        REQUIRE(herderOf(app).mTxFloodTargetHistoryForTesting.size() == 1);
    }

    auto const appliedBefore = appliedTxs(nodes[0]);
    int submitted = 0;
    for (int batch = 0; batch < 5; ++batch)
    {
        for (size_t n = 0; n < nodes.size(); ++n)
        {
            auto source = getGenesisAccount(*nodes[n], submitted);
            auto dest = getGenesisAccount(*nodes[n], NUM_ACCOUNTS - 1);
            auto tx = source.tx({payment(dest.getPublicKey(), 1000)});
            REQUIRE(nodes[n]->getHerder().recvTransaction(tx, false) ==
                    TxSubmitStatus::TX_STATUS_PENDING);
            ++submitted;
        }
        auto const next =
            nodes[0]->getLedgerManager().getLastClosedLedgerNum() + 1;
        simulation->crankUntil([&]() { return allClosed(nodes, next); },
                               std::chrono::seconds(60), false);
    }
    simulation->crankUntil(
        [&]() {
            for (auto const& app : nodes)
            {
                if (appliedTxs(app) - appliedBefore < submitted)
                {
                    return false;
                }
            }
            return true;
        },
        std::chrono::seconds(120), false);
    auto const counts = countTxFlood(nodes);
    INFO(counts.toString());
    REQUIRE(counts.mAdvertised == 0);
    REQUIRE(counts.mDemanded == 0);
    REQUIRE(counts.mLocalFlooded == 0);
    REQUIRE(counts.mDirectSent == 3 * static_cast<int64_t>(submitted));
    REQUIRE(counts.mDirectRecv == counts.mDirectSent);
    for (auto const& app : nodes)
    {
        REQUIRE(appliedTxs(app) - appliedBefore == submitted);
    }
}

TEST_CASE("transactions are included while a leader is down",
          "[herder][tx-flood-targets][simulation]")
{
    // 3 validators, threshold 2, one never started. With leader terms the
    // node that closed a ledger leads the next one too, so the dead node
    // only leads round 1 when a term ends (every fifth ledger, or after a
    // ledger that applied nothing), but it is the round 2 leader of about
    // every other slot. A target that is not a connected, identified peer
    // makes the live nodes flood, and the round moves the targets on.
    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    auto simulation =
        std::make_shared<Simulation>(networkID, acceleratedConfig);
    std::vector<SecretKey> keys;
    for (int i = 0; i < 3; i++)
    {
        keys.push_back(
            SecretKey::fromSeed(sha256("NODE_SEED_" + std::to_string(i))));
    }
    SCPQuorumSet qSet;
    qSet.threshold = 2;
    std::vector<NodeID> allNodes;
    for (auto const& k : keys)
    {
        qSet.validators.push_back(k.getPublicKey());
        allNodes.push_back(k.getPublicKey());
    }
    simulation->addNode(keys[0], qSet);
    simulation->addNode(keys[1], qSet);
    simulation->addPendingConnection(keys[0].getPublicKey(),
                                     keys[1].getPublicKey());
    simulation->startAllNodes();
    auto nodes = simulation->getNodes();
    simulation->crankUntil(
        [&]() { return allClosed(nodes, 4); },
        std::chrono::seconds(300), false);

    auto const appliedBefore = appliedTxs(nodes[0]);
    int submitted = 0;
    // Whether a node sent its overlay a target list at round 2 or later.
    auto sawLaterRound = [&]() {
        for (auto const& app : nodes)
        {
            for (auto const& update :
                 herderOf(app).mTxFloodTargetHistoryForTesting)
            {
                if (update.targets && update.round >= 2 &&
                    update.round != UINT32_MAX)
                {
                    return true;
                }
            }
        }
        return false;
    };
    // 10 ledgers with transactions. Then ledgers without any, until the
    // dead node led a round 1: an empty ledger ends its leader's term, and
    // the dead node is the next slot's first leader half of the time.
    for (int batch = 0; batch < 60 && (batch < 10 || !sawLaterRound());
         ++batch)
    {
        for (size_t n = 0; batch < 10 && n < nodes.size(); ++n)
        {
            auto source = getGenesisAccount(*nodes[n], submitted);
            auto dest = getGenesisAccount(*nodes[n], NUM_ACCOUNTS - 1);
            auto tx = source.tx({payment(dest.getPublicKey(), 1000)});
            REQUIRE(nodes[n]->getHerder().recvTransaction(tx, false) ==
                    TxSubmitStatus::TX_STATUS_PENDING);
            ++submitted;
        }
        auto const next =
            nodes[0]->getLedgerManager().getLastClosedLedgerNum() + 1;
        simulation->crankUntil([&]() { return allClosed(nodes, next); },
                               std::chrono::seconds(120), false);
    }
    simulation->crankUntil(
        [&]() {
            return appliedTxs(nodes[0]) - appliedBefore >= submitted &&
                   appliedTxs(nodes[1]) - appliedBefore >= submitted;
        },
        std::chrono::seconds(300), false);
    REQUIRE(appliedTxs(nodes[0]) - appliedBefore == submitted);
    REQUIRE(appliedTxs(nodes[1]) - appliedBefore == submitted);

    // Every target list a node sent to its overlay is what the schedule
    // gives for the state the node was in then: see isScheduledTargetList.
    // Some lists were sent at round 2 or later.
    int checked = 0;
    int laterRounds = 0;
    TermPredictions predictions;
    for (auto const& app : nodes)
    {
        for (auto const& update :
             herderOf(app).mTxFloodTargetHistoryForTesting)
        {
            if (!update.targets)
            {
                continue;
            }
            INFO(fmt::format("slot {}, round {}, {} targets", update.slot,
                             update.round, update.targets->size()));
            REQUIRE(isScheduledTargetList(update, allNodes));
            ++checked;
            if (update.round >= 2 && update.round != UINT32_MAX)
            {
                ++laterRounds;
            }
        }
        auto const p = checkTermPredictions(
            herderOf(app).mTxFloodTargetHistoryForTesting, allNodes);
        predictions.mRight += p.mRight;
        predictions.mWrong += p.mWrong;
        predictions.mWrongWithoutForfeit += p.mWrongWithoutForfeit;
    }
    INFO(fmt::format("target lists checked {}, at round 2 or later {}; a "
                     "new term's first leader was in the list one ledger "
                     "ahead {} times, not {} times ({} without a forfeit)",
                     checked, laterRounds, predictions.mRight,
                     predictions.mWrong, predictions.mWrongWithoutForfeit));
    // An update is only sent when the targets change, so there are fewer
    // than one per node and ledger.
    REQUIRE(checked >= 10);
    REQUIRE(laterRounds > 0);
    REQUIRE(predictions.mWrongWithoutForfeit == 0);
    auto const counts = countTxFlood(nodes);
    INFO(counts.toString());
    // Both paths were used: direct while both targets were alive, the flood
    // while the dead node was one of them.
    REQUIRE(counts.mLocalFlooded > 0);
}

TEST_CASE("no transaction is included twice in overlay-only mode",
          "[herder][tx-flood-targets][simulation]")
{
    // Overlay-only mode (the mode of the network-level load tests) closes
    // ledgers without applying them, so nothing rejects a transaction that
    // a leader includes a second time: every copy that reaches a mempool
    // after its transaction externalized shows up as a duplicate.
    auto simulation = Topologies::core(
        4, 0.75, sha256(getTestConfig().NETWORK_PASSPHRASE), [](int i) {
            auto cfg = acceleratedConfig(i);
            cfg.ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING = true;
            cfg.GENESIS_TEST_ACCOUNT_COUNT = 2000;
            return cfg;
        });
    simulation->startAllNodes();
    auto nodes = simulation->getNodes();
    simulation->crankUntil(
        [&]() {
            if (!allClosed(nodes, 4))
            {
                return false;
            }
            for (auto const& app : nodes)
            {
                if (identifiedPeers(app) != 3 ||
                    !herderOf(app).getTxFloodTargetsForTesting())
                {
                    return false;
                }
            }
            return true;
        },
        std::chrono::seconds(120), false);
    for (auto const& app : nodes)
    {
        app->setRunInOverlayOnlyMode(true);
    }

    uint32_t const accountsPerNode = 400;
    uint32_t const txsPerNode = 300;
    auto completedRuns = [](Application& app) {
        return app.getMetrics()
            .NewMeter({"loadgen", "run", "complete"}, "run")
            .count();
    };
    std::vector<uint64_t> before;
    for (size_t i = 0; i < nodes.size(); ++i)
    {
        before.emplace_back(completedRuns(*nodes[i]));
        nodes[i]->getLoadGenerator().generateLoad(GeneratedLoadConfig::txLoad(
            LoadGenMode::PAY, accountsPerNode, txsPerNode, /* txRate */ 10,
            /* offset */ static_cast<uint32_t>(i) * accountsPerNode));
    }

    // Every ledger node 0 closes: its transactions, by full hash.
    std::map<Hash, uint32_t> firstIn;
    std::vector<std::string> twice;
    size_t included = 0;
    uint32_t ledgers = 0;
    uint32_t missed = 0;
    uint32_t lastSeq = nodes[0]->getLedgerManager().getLastClosedLedgerNum();
    auto record = [&]() {
        auto const& lcl = nodes[0]->getLedgerManager().getLastClosedLedgerHeader();
        if (lcl.header.ledgerSeq == lastSeq)
        {
            return;
        }
        missed += lcl.header.ledgerSeq - lastSeq - 1;
        lastSeq = lcl.header.ledgerSeq;
        auto res = herderOf(nodes[0]).getTxSet(lcl.header.scpValue.txSetHash);
        auto const* txSet = std::get_if<TxSetXDRFrameConstPtr>(&res);
        if (!txSet || !*txSet)
        {
            return;
        }
        ++ledgers;
        for (auto const& hash : (*txSet)->getTransactionFullHashes())
        {
            ++included;
            auto [it, isNew] = firstIn.emplace(hash, lastSeq);
            if (!isNew)
            {
                twice.emplace_back(fmt::format(
                    "{} in ledger {} (slot % 5 = {}) and again in {} (slot % 5 "
                    "= {})",
                    hexAbbrev(hash), it->second, it->second % 5, lastSeq,
                    lastSeq % 5));
            }
        }
    };
    auto loadDone = [&]() {
        for (size_t i = 0; i < nodes.size(); ++i)
        {
            if (completedRuns(*nodes[i]) != before[i] + 1)
            {
                return false;
            }
        }
        return true;
    };
    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(150);
    while (!loadDone() && std::chrono::steady_clock::now() < deadline)
    {
        simulation->crankForAtMost(std::chrono::milliseconds(20), false);
        record();
    }
    bool const completed = loadDone();
    // A few more ledgers: a late copy is included by whoever leads next.
    auto const last = nodes[0]->getLedgerManager().getLastClosedLedgerNum() + 12;
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!allClosed(nodes, last) &&
           std::chrono::steady_clock::now() < deadline)
    {
        simulation->crankForAtMost(std::chrono::milliseconds(20), false);
        record();
    }
    std::string loadgen;
    for (auto const& app : nodes)
    {
        auto& m = app->getMetrics();
        loadgen += fmt::format(
            " [attempted {} rejected {} complete {} failed {}]",
            m.NewMeter({"loadgen", "txn", "attempted"}, "txn").count(),
            m.NewMeter({"loadgen", "txn", "rejected"}, "txn").count(),
            m.NewMeter({"loadgen", "run", "complete"}, "run").count(),
            m.NewMeter({"loadgen", "run", "failed"}, "run").count());
    }

    std::string list;
    for (size_t i = 0; i < twice.size() && i < 40; ++i)
    {
        list += "\n  " + twice[i];
    }
    INFO(fmt::format("{} ledgers with transactions ({} not seen), {} "
                     "inclusions of {} distinct transactions, {} submitted; "
                     "loadgen per node:{}; included twice: {}{}",
                     ledgers, missed, included, firstIn.size(),
                     nodes.size() * txsPerNode, loadgen, twice.size(), list));
    REQUIRE(completed);
    REQUIRE(missed == 0);
    REQUIRE(firstIn.size() == nodes.size() * txsPerNode);
    REQUIRE(twice.empty());
}

TEST_CASE("experiment options: legacy adverts and batch task count",
          "[herder][tx-flood-targets]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.TX_FLOOD_LEGACY_ADVERTS = true;
    cfg.BATCH_EXECUTOR_TASKS = 7;
    auto app = createTestApplication(clock, cfg);
    // Without the option a single node's target list is itself (see the
    // last section of the next test); with it there is no list at all, and
    // the overlay floods with adverts as before.
    REQUIRE(!herderOf(app).computeTxFloodTargets());
    // Seven validation tasks whatever the machine's physical core count.
    REQUIRE(app->getBatchExecutor().preferredTaskCount() == 7);
}

TEST_CASE("a node checks a transaction before it sends it on",
          "[herder][tx-flood-targets]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    auto app = createTestApplication(clock, cfg);
    auto& herder = herderOf(app);
    auto& lm = app->getLedgerManager();
    auto root = app->getRoot();
    auto const balance = lm.getLastMinBalance(2) + 10'000'000;
    auto a = root->create("a", balance);
    auto b = root->create("b", balance);
    auto c = root->create("c", balance);
    auto submit = [&](TransactionFrameBasePtr const& tx) {
        return herder.recvTransaction(tx, false);
    };
    auto& invalid =
        app->getMetrics().NewMeter({"herder", "tx-admission", "invalid"}, "tx");
    auto& busy = app->getMetrics().NewMeter(
        {"herder", "tx-admission", "account-busy"}, "tx");

    SECTION("a valid transaction is accepted, once per source account")
    {
        auto tx1 = a.tx({payment(b, 100)});
        REQUIRE(submit(tx1) == TxSubmitStatus::TX_STATUS_PENDING);
        REQUIRE(submit(tx1) == TxSubmitStatus::TX_STATUS_DUPLICATE);
        // Another transaction of the same account, valid on its own against
        // the last closed ledger (same sequence number): only one of the
        // two can apply.
        auto tx2 = a.tx({payment(c, 100)}, tx1->getSeqNum());
        REQUIRE(tx2->getFullHash() != tx1->getFullHash());
        REQUIRE(submit(tx2) == TxSubmitStatus::TX_STATUS_TRY_AGAIN_LATER);
        REQUIRE(busy.count() == 1);
        // Other accounts are not affected.
        REQUIRE(submit(b.tx({payment(a, 100)})) ==
                TxSubmitStatus::TX_STATUS_PENDING);
        REQUIRE(invalid.count() == 0);

        SECTION("until it is externalized")
        {
            closeLedger(*app, {tx1});
            auto tx3 = a.tx({payment(c, 100)});
            REQUIRE(tx3->getSeqNum() == tx1->getSeqNum() + 1);
            REQUIRE(submit(tx3) == TxSubmitStatus::TX_STATUS_PENDING);
        }
        SECTION("or another transaction of the account externalizes")
        {
            // tx2 was refused here, but reached the ledger through another
            // node: tx1 can no longer apply, and the account is free.
            closeLedger(*app, {tx2});
            auto tx3 = a.tx({payment(c, 100)}, tx2->getSeqNum() + 1);
            REQUIRE(submit(tx3) == TxSubmitStatus::TX_STATUS_PENDING);
        }
        SECTION("or the sequence number is used up some other way")
        {
            // Another account's transaction bumps a's sequence number past
            // tx1's. No transaction of a externalizes, but tx1 is dead.
            auto bump = c.tx({a.op(bumpSequence(tx1->getSeqNum() + 5))});
            bump->addSignature(a.getSecretKey());
            closeLedger(*app, {bump});
            REQUIRE(a.getLastSequenceNumber() == tx1->getSeqNum() + 5);
            auto tx3 = a.tx({payment(c, 100)});
            REQUIRE(submit(tx3) == TxSubmitStatus::TX_STATUS_PENDING);
            REQUIRE(busy.count() == 1);
        }
        SECTION("an unrelated ledger does not free the account")
        {
            closeLedger(*app, {c.tx({payment(a, 100)})});
            REQUIRE(submit(tx2) == TxSubmitStatus::TX_STATUS_TRY_AGAIN_LATER);
        }
        SECTION("or it times out")
        {
            clock.setCurrentVirtualTime(
                clock.now() + HerderImpl::LOCAL_TX_PENDING_TIMEOUT -
                std::chrono::seconds(1));
            REQUIRE(submit(tx2) == TxSubmitStatus::TX_STATUS_TRY_AGAIN_LATER);
            clock.setCurrentVirtualTime(clock.now() + std::chrono::seconds(1));
            REQUIRE(submit(tx2) == TxSubmitStatus::TX_STATUS_PENDING);
        }
    }

    SECTION("an invalid transaction is refused")
    {
        auto expectInvalid = [&](TransactionFrameBasePtr const& tx) {
            auto const count = invalid.count();
            REQUIRE(submit(tx) == TxSubmitStatus::TX_STATUS_ERROR);
            REQUIRE(invalid.count() == count + 1);
        };
        SECTION("bad signature")
        {
            auto tx = a.tx({payment(b, 100)});
            tx->getMutableEnvelope().v1().signatures.clear();
            tx->addSignature(b.getSecretKey());
            expectInvalid(tx);
        }
        SECTION("sequence number already used")
        {
            expectInvalid(a.tx({payment(b, 100)}, a.getLastSequenceNumber()));
        }
        SECTION("sequence number in the future")
        {
            expectInvalid(
                a.tx({payment(b, 100)}, a.getLastSequenceNumber() + 2));
        }
        SECTION("fee too low")
        {
            auto tx = transactionFromOperations(
                *app, a.getSecretKey(), a.nextSequenceNumber(),
                {payment(b, 100)}, 1);
            expectInvalid(tx);
        }
        SECTION("no such account")
        {
            auto tx = transactionFromOperations(
                *app, getAccount("ghost"), 1, {payment(b, 100)});
            expectInvalid(tx);
        }
        // A refused transaction does not block its account.
        REQUIRE(
            submit(a.tx({payment(b, 100)}, a.getLastSequenceNumber() + 1)) ==
            TxSubmitStatus::TX_STATUS_PENDING);
    }

    SECTION("pregenerated load skips the validity check")
    {
        auto tx = a.tx({payment(b, 100)});
        tx->getMutableEnvelope().v1().signatures.clear();
        REQUIRE(herder.recvTransaction(tx, false, /*isLoadgenTx=*/true) ==
                TxSubmitStatus::TX_STATUS_PENDING);
        REQUIRE(invalid.count() == 0);
    }

    SECTION("overlay-only mode does not limit an account to one transaction")
    {
        app->setRunInOverlayOnlyMode(true);
        auto tx1 = a.tx({payment(b, 100)});
        auto tx2 = a.tx({payment(c, 100)});
        REQUIRE(submit(tx1) == TxSubmitStatus::TX_STATUS_PENDING);
        // The sequence-number check is off in this mode too (checkValid).
        REQUIRE(submit(tx2) == TxSubmitStatus::TX_STATUS_PENDING);
        // The validity check still runs.
        auto tx3 = b.tx({payment(a, 100)});
        tx3->getMutableEnvelope().v1().signatures.clear();
        REQUIRE(submit(tx3) == TxSubmitStatus::TX_STATUS_ERROR);
        app->setRunInOverlayOnlyMode(false);
    }

    SECTION("the targets of a single node, and of one that lost sync")
    {
        // A node that is its own quorum leads every round: it is the only
        // target, so nothing is sent anywhere.
        auto targets = herder.computeTxFloodTargets();
        REQUIRE(targets);
        REQUIRE(*targets ==
                std::vector<NodeID>{app->getConfig().NODE_SEED.getPublicKey()});
        // Without the network's latest ledger the node does not know who
        // leads: everyone.
        herder.lostSync();
        REQUIRE(!herder.computeTxFloodTargets());
        REQUIRE(!herder.getTxFloodTargetsForTesting());
    }
}
