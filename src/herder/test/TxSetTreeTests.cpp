// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// Network-level tests of the spanning tree for the leader's tx set
// (EXPERIMENTAL_TX_SET_TREE): the leader writes its set to its children in a
// tree that every node derives from the nodes of the leader schedule and the
// slot's seed, and every node passes the set on to its own children; or, in
// "coded" mode, the leader cuts the set into erasure-coded pieces and every
// node gets its pieces from as many different senders. The logic is tested
// in the overlay (overlay/src/flood/txset_tree.rs, txset_coded.rs,
// overlay/src/libp2p_overlay/txset_tree_tests.rs, txset_coded_tests.rs);
// these tests run it under consensus, with Core telling every overlay the
// trees of the next slots.

#include "crypto/SHA.h"
#include "herder/Herder.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/RustOverlayManager.h"
#include "simulation/Simulation.h"
#include "simulation/Topologies.h"
#include "test/Catch2.h"
#include "test/test.h"
#include "util/MetricsRegistry.h"
#include <fmt/format.h>

using namespace stellar;

namespace
{
int const NODES = 7;

// Network totals of the dissemination counters.
struct TreeCounts
{
    // Copies of a set written by its leader (tagged or not), by its leader
    // with a header, and by relays.
    int64_t mPushSent{0};
    int64_t mRootSent{0};
    int64_t mRelayed{0};
    // Tagged frames received, refused for relaying, received twice.
    int64_t mReceived{0};
    int64_t mRefused{0};
    int64_t mDuplicate{0};
    // Requests put off because the tree should bring the set, and those
    // that had to be sent after the wait.
    int64_t mDeferred{0};
    int64_t mFallback{0};
    // Frames whose delay since the leader's send was recorded.
    int64_t mDelayCount{0};
    // Core: sets delivered by the overlay, and requests repeated after the
    // one-second retry interval.
    int64_t mCoreFetches{0};
    int64_t mCoreRetries{0};
    // Relays not made by nodes that are set up to withhold them.
    int64_t mWithheld{0};
    // Coded mode: pieces written by leaders and by relays, valid pieces
    // received, pieces refused and pieces found wrong; sets rebuilt, with a
    // parity piece, and rebuilds that gave no tx set.
    int64_t mPiecesSent{0};
    int64_t mPiecesRelayed{0};
    int64_t mPiecesReceived{0};
    int64_t mPiecesRefused{0};
    int64_t mPiecesInvalid{0};
    int64_t mSetsRebuilt{0};
    int64_t mSetsRebuiltWithParity{0};
    int64_t mSetsFailed{0};
    int64_t mSetsInconsistent{0};

    std::string
    toString() const
    {
        return fmt::format(
            "push sent {} (tagged {}), relayed {}, received {}, refused {}, "
            "duplicate {}, deferred {}, fallback {}, delays {}; core fetches "
            "{} retries {}; withheld {}; pieces sent {} relayed {} received "
            "{} refused {} invalid {}; sets rebuilt {} (with parity {}) "
            "failed {} inconsistent {}",
            mPushSent, mRootSent, mRelayed, mReceived, mRefused, mDuplicate,
            mDeferred, mFallback, mDelayCount, mCoreFetches, mCoreRetries,
            mWithheld, mPiecesSent, mPiecesRelayed, mPiecesReceived,
            mPiecesRefused, mPiecesInvalid, mSetsRebuilt,
            mSetsRebuiltWithParity, mSetsFailed, mSetsInconsistent);
    }
};

TreeCounts
countTree(std::vector<Application::pointer> const& nodes)
{
    TreeCounts res;
    for (auto const& app : nodes)
    {
        app->getOverlayManager().syncOverlayMetrics();
        auto& metrics = app->getMetrics();
        auto tree = [&](char const* name, char const* unit = "txset") {
            return static_cast<int64_t>(
                metrics.NewMeter({"overlay", "txset-tree", name}, unit)
                    .count());
        };
        res.mPushSent += static_cast<int64_t>(
            metrics.NewMeter({"overlay", "txset-push", "sent"}, "txset")
                .count());
        res.mRootSent += tree("root-sent");
        res.mRelayed += tree("relayed");
        res.mReceived += tree("received");
        res.mRefused += tree("refused");
        res.mDuplicate += tree("duplicate");
        res.mDeferred += tree("fetch-deferred");
        res.mFallback += tree("fetch-fallback");
        res.mDelayCount += tree("delay-count");
        res.mWithheld += tree("withheld");
        auto coded = [&](char const* name, char const* unit) {
            return static_cast<int64_t>(
                metrics.NewMeter({"overlay", "txset-coded", name}, unit)
                    .count());
        };
        res.mPiecesSent += coded("pieces-sent", "piece");
        res.mPiecesRelayed += coded("pieces-relayed", "piece");
        res.mPiecesReceived += coded("pieces-received", "piece");
        res.mPiecesRefused += coded("pieces-refused", "piece");
        res.mPiecesInvalid += coded("pieces-invalid", "piece");
        res.mSetsRebuilt += coded("sets-rebuilt", "txset");
        res.mSetsRebuiltWithParity +=
            coded("sets-rebuilt-with-parity", "txset");
        res.mSetsFailed += coded("sets-failed", "txset");
        res.mSetsInconsistent += coded("sets-inconsistent", "txset");
        res.mCoreFetches += static_cast<int64_t>(
            metrics.NewTimer({"scp", "fetch", "txset"}).count());
        res.mCoreRetries += static_cast<int64_t>(
            metrics.NewCounter({"scp", "fetch", "txset-retry"}).count());
    }
    return res;
}

// A 7-node network with a 5-of-7 quorum, every node connected to every other
// one. With a fanout of 2 its trees have two levels: the leader, two relays,
// four leaves.
Simulation::pointer
makeNetwork(std::function<void(Config&, int)> const& adjust)
{
    return Topologies::core(
        NODES, 0.7, sha256(getTestConfig().NETWORK_PASSPHRASE), [&](int i) {
            auto cfg = getTestConfig(i);
            cfg.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = true;
            cfg.EXPERIMENTAL_TX_SET_TREE_FANOUT = 2;
            adjust(cfg, i);
            return cfg;
        });
}

// Starts the network, cranks until every node closed `lastLedger` and
// returns how many ledgers that were (from the first node's ledger at the
// start).
int64_t
closeLedgers(Simulation::pointer const& simulation, uint32_t lastLedger)
{
    simulation->startAllNodes();
    auto nodes = simulation->getNodes();
    auto const first = nodes[0]->getLedgerManager().getLastClosedLedgerNum();
    simulation->crankUntil(
        [&]() { return simulation->haveAllExternalized(lastLedger, 3); },
        std::chrono::seconds(300), false);
    REQUIRE(simulation->haveAllExternalized(lastLedger, 3));
    return static_cast<int64_t>(lastLedger) - first;
}
}

TEST_CASE("tx set tree mode is read from the config", "[herder][txset-tree]")
{
    Config cfg;
    REQUIRE(cfg.EXPERIMENTAL_TX_SET_TREE == "off");
    REQUIRE(cfg.EXPERIMENTAL_TX_SET_TREE_FANOUT == 0);
    REQUIRE(cfg.txSetTreeMode() == 0);
    cfg.EXPERIMENTAL_TX_SET_TREE = "star";
    REQUIRE(cfg.txSetTreeMode() == 1);
    cfg.EXPERIMENTAL_TX_SET_TREE = "hash";
    REQUIRE(cfg.txSetTreeMode() == 2);
    cfg.EXPERIMENTAL_TX_SET_TREE = "rtt";
    REQUIRE(cfg.txSetTreeMode() == 3);
    cfg.EXPERIMENTAL_TX_SET_TREE = "coded";
    REQUIRE(cfg.txSetTreeMode() == 4);
    REQUIRE(cfg.EXPERIMENTAL_TX_SET_TREE_WITHHOLDERS_FOR_TESTING == 0);
}

TEST_CASE("the tx set of a leader travels down the hash tree",
          "[herder][txset-tree][simulation]")
{
    auto simulation = makeNetwork(
        [](Config& cfg, int) { cfg.EXPERIMENTAL_TX_SET_TREE = "hash"; });
    uint32_t const lastLedger = 16;
    auto const ledgers = closeLedgers(simulation, lastLedger);
    REQUIRE(ledgers >= 12);

    auto counts = countTree(simulation->getNodes());
    INFO(counts.toString());
    // Sets were relayed, by nodes that their parent in the tree sent them
    // to: nothing was refused.
    REQUIRE(counts.mRelayed > 0);
    REQUIRE(counts.mRefused == 0);
    // Once the nodes know each other's validator identity and the trees,
    // a leader writes 2 copies of its set and its 2 children write 2 each;
    // before that (the first ledgers) it writes to all 6 peers. So the
    // leaders wrote far fewer copies than 6 per ledger, and most of the
    // copies came through relays.
    REQUIRE(counts.mPushSent <= 4 * ledgers);
    REQUIRE(counts.mRelayed >= 2 * (ledgers - 6));
    // Every node still got every set from its overlay without the
    // one-second retry.
    REQUIRE(counts.mCoreFetches >= (NODES - 1) * (ledgers - 2));
    REQUIRE(counts.mCoreRetries == 0);
    // The delay since the leader's send was recorded for the tagged frames.
    REQUIRE(counts.mDelayCount == counts.mReceived);
    REQUIRE(counts.mReceived > 0);
}

TEST_CASE("star mode pushes to every peer with a header",
          "[herder][txset-tree][simulation]")
{
    auto simulation = makeNetwork(
        [](Config& cfg, int) { cfg.EXPERIMENTAL_TX_SET_TREE = "star"; });
    uint32_t const lastLedger = 12;
    auto const ledgers = closeLedgers(simulation, lastLedger);
    REQUIRE(ledgers >= 8);

    auto counts = countTree(simulation->getNodes());
    INFO(counts.toString());
    // Every leader wrote its set to its 6 peers; nothing was relayed, and
    // nobody waited before asking.
    REQUIRE(counts.mPushSent >= (NODES - 1) * (ledgers - 2));
    REQUIRE(counts.mRootSent > 0);
    REQUIRE(counts.mRelayed == 0);
    REQUIRE(counts.mRefused == 0);
    REQUIRE(counts.mDeferred == 0);
    REQUIRE(counts.mDelayCount == counts.mReceived);
    REQUIRE(counts.mReceived > 0);
    REQUIRE(counts.mCoreRetries == 0);
}

TEST_CASE("without the setting nothing is tagged or relayed",
          "[herder][txset-tree][simulation]")
{
    auto simulation = makeNetwork([](Config&, int) {});
    uint32_t const lastLedger = 10;
    auto const ledgers = closeLedgers(simulation, lastLedger);

    auto counts = countTree(simulation->getNodes());
    INFO(counts.toString());
    REQUIRE(counts.mPushSent >= (NODES - 1) * (ledgers - 2));
    REQUIRE(counts.mRootSent == 0);
    REQUIRE(counts.mReceived == 0);
    REQUIRE(counts.mRelayed == 0);
    REQUIRE(counts.mDeferred == 0);
    REQUIRE(counts.mCoreRetries == 0);
}

TEST_CASE("the leader lays the tree out by round-trip times",
          "[herder][txset-tree][simulation]")
{
    auto simulation = makeNetwork(
        [](Config& cfg, int) { cfg.EXPERIMENTAL_TX_SET_TREE = "rtt"; });
    uint32_t const lastLedger = 16;
    auto const ledgers = closeLedgers(simulation, lastLedger);
    REQUIRE(ledgers >= 12);

    auto counts = countTree(simulation->getNodes());
    INFO(counts.toString());
    // On loopback every peer is at the same distance, so the order is
    // arbitrary; what matters here is that every node follows the order
    // the leader sends along.
    REQUIRE(counts.mRelayed >= 2 * (ledgers - 6));
    REQUIRE(counts.mRefused == 0);
    REQUIRE(counts.mPushSent <= 4 * ledgers);
    REQUIRE(counts.mCoreRetries == 0);
}

TEST_CASE(
    "a node that does not relay costs its subtree a wait and not a ledger",
    "[herder][txset-tree][simulation]")
{
    // Node 0 neither relays nor is told about trees. When it is a relay of
    // the leader's tree, its two children do not get the set through the
    // tree: they ask for it after the wait, well before Core's one-second
    // retry, and the network closes its ledgers.
    auto simulation = makeNetwork([](Config& cfg, int i) {
        cfg.EXPERIMENTAL_TX_SET_TREE = i == 0 ? "off" : "hash";
    });
    uint32_t const lastLedger = 30;
    auto const ledgers = closeLedgers(simulation, lastLedger);
    REQUIRE(ledgers >= 24);

    auto counts = countTree(simulation->getNodes());
    INFO(counts.toString());
    REQUIRE(counts.mRelayed > 0);
    // Node 0 is one of the two relays in about a third of the trees it does
    // not lead: over this many ledgers some subtree had to ask.
    REQUIRE(counts.mFallback > 0);
    REQUIRE(counts.mCoreRetries == 0);
}

TEST_CASE("the tx set of a leader travels as coded pieces",
          "[herder][txset-tree][txset-coded][simulation]")
{
    // With 6 other nodes a set is cut into 6 pieces of which 3 rebuild it;
    // the leader writes one piece to each node, and each passes its piece
    // on to the 5 others.
    auto simulation = makeNetwork(
        [](Config& cfg, int) { cfg.EXPERIMENTAL_TX_SET_TREE = "coded"; });
    uint32_t const lastLedger = 16;
    auto const ledgers = closeLedgers(simulation, lastLedger);
    REQUIRE(ledgers >= 12);

    auto counts = countTree(simulation->getNodes());
    INFO(counts.toString());
    // Once the nodes know each other's validator identity and the layouts,
    // a leader writes 6 pieces per set and no whole copy; before that (the
    // first ledgers) it writes the set to all 6 peers.
    REQUIRE(counts.mPiecesSent >= 6 * (ledgers - 6));
    REQUIRE(counts.mPiecesRelayed >= 5 * 6 * (ledgers - 6));
    REQUIRE(counts.mPushSent <= 4 * ledgers);
    // Every node rebuilt those sets from pieces, and no piece was wrong.
    REQUIRE(counts.mSetsRebuilt >= (NODES - 1) * (ledgers - 6));
    REQUIRE(counts.mPiecesInvalid == 0);
    REQUIRE(counts.mSetsFailed == 0);
    REQUIRE(counts.mSetsInconsistent == 0);
    REQUIRE(counts.mWithheld == 0);
    // Every node still got every set from its overlay without the
    // one-second retry, and the delay was recorded for every rebuilt set.
    REQUIRE(counts.mCoreFetches >= (NODES - 1) * (ledgers - 2));
    REQUIRE(counts.mCoreRetries == 0);
    REQUIRE(counts.mDelayCount == counts.mReceived);
    REQUIRE(counts.mReceived >= counts.mSetsRebuilt);
}

TEST_CASE("relays that withhold cost a tree a wait and coded pieces none",
          "[herder][txset-tree][txset-coded][simulation]")
{
    // The validator with the smallest node ID passes nothing on.
    //  - In the hash tree (two relays, two children each) the children of
    //    that relay ask for the set after their wait, in every tree in which
    //    it is a relay.
    //  - In coded mode a node then holds at least 5 of its 6 pieces and
    //    needs 3: it rebuilds the set, mostly with a parity piece, and
    //    nobody has to ask.
    auto run = [](std::string const& mode) {
        auto simulation = makeNetwork([&](Config& cfg, int) {
            cfg.EXPERIMENTAL_TX_SET_TREE = mode;
            cfg.EXPERIMENTAL_TX_SET_TREE_WITHHOLDERS_FOR_TESTING = 1;
        });
        uint32_t const lastLedger = 30;
        auto const ledgers = closeLedgers(simulation, lastLedger);
        REQUIRE(ledgers >= 24);
        auto counts = countTree(simulation->getNodes());
        // The network closes its ledgers either way, without Core's
        // one-second retry.
        REQUIRE(counts.mCoreRetries == 0);
        REQUIRE(counts.mWithheld > 0);
        return counts;
    };
    auto const tree = run("hash");
    auto const coded = run("coded");
    INFO("hash: " << tree.toString());
    INFO("coded: " << coded.toString());
    REQUIRE(tree.mFallback > 0);
    REQUIRE(coded.mSetsRebuiltWithParity > 0);
    REQUIRE(coded.mPiecesInvalid == 0);
    REQUIRE(coded.mSetsFailed == 0);
    // Coded: far fewer sets asked for than in the tree (none, unless two
    // writes to one node failed for one set, which the first ledgers'
    // stream set-up can do).
    REQUIRE(coded.mFallback * 4 <= tree.mFallback);
}
