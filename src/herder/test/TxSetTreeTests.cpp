// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// Network-level tests of the spanning tree for the leader's tx set
// (EXPERIMENTAL_TX_SET_TREE): the leader writes its set to its children in a
// tree that every node derives from the nodes of the leader schedule and the
// slot's seed, and every node passes the set on to its own children. The
// tree's logic is tested in the overlay (overlay/src/flood/txset_tree.rs,
// overlay/src/libp2p_overlay/txset_tree_tests.rs); these tests run it under
// consensus, with Core telling every overlay the trees of the next slots.

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

    std::string
    toString() const
    {
        return fmt::format(
            "push sent {} (tagged {}), relayed {}, received {}, refused {}, "
            "duplicate {}, deferred {}, fallback {}, delays {}; core fetches "
            "{} retries {}",
            mPushSent, mRootSent, mRelayed, mReceived, mRefused, mDuplicate,
            mDeferred, mFallback, mDelayCount, mCoreFetches, mCoreRetries);
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

TEST_CASE("a node that does not relay costs its subtree a wait and not a ledger",
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
