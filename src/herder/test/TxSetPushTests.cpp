// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// Network-level tests of the leader's tx-set push: the leader of a round
// sends the tx set it is about to propose to every peer as soon as the set
// is built, instead of waiting for each follower to request it. Followers
// keep a pushed set in their overlay and hand it to Core only when an SCP
// statement names its hash; a follower that did not get it (or got another
// set) requests the proposed one as before.

#include "crypto/SHA.h"
#include "herder/Herder.h"
#include "herder/HerderImpl.h"
#include "herder/TxSetFrame.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "overlay/RustOverlayManager.h"
#include "simulation/Simulation.h"
#include "simulation/Topologies.h"
#include "test/Catch2.h"
#include "test/test.h"
#include "util/Logging.h"
#include "util/MetricsRegistry.h"
#include "util/ProtocolVersion.h"
#include <fmt/format.h>
#include <map>

using namespace stellar;

namespace
{
Config
acceleratedConfig(int configNum)
{
    auto cfg = getTestConfig(configNum);
    cfg.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = true;
    return cfg;
}

// Network totals of the tx-set dissemination counters.
struct TxSetCounts
{
    // Leader side: sets written to a peer without a request, and peer
    // requests not answered because the set was already pushed to that peer.
    int64_t mPushSent{0};
    int64_t mRequestSkipped{0};
    // Follower side: sets that arrived unrequested, Core requests answered
    // at once from such a set, sets that had to be awaited from the network,
    // and unrequested sets dropped by the limits.
    int64_t mPushReceived{0};
    int64_t mPushHit{0};
    int64_t mNetworkFetches{0};
    int64_t mPushDropped{0};
    // Core: sets delivered by the overlay, and requests repeated after the
    // one-second retry interval.
    int64_t mCoreFetches{0};
    int64_t mCoreRetries{0};

    std::string
    toString() const
    {
        return fmt::format(
            "push sent {} received {} hit {} dropped {}; request skipped {}; "
            "network fetches {}; core fetches {} retries {}",
            mPushSent, mPushReceived, mPushHit, mPushDropped, mRequestSkipped,
            mNetworkFetches, mCoreFetches, mCoreRetries);
    }
};

TxSetCounts
countTxSetDissemination(std::vector<Application::pointer> const& nodes)
{
    TxSetCounts res;
    for (auto const& app : nodes)
    {
        app->getOverlayManager().syncOverlayMetrics();
        auto& metrics = app->getMetrics();
        auto push = [&](char const* name) {
            return static_cast<int64_t>(
                metrics.NewMeter({"overlay", "txset-push", name}, "txset")
                    .count());
        };
        res.mPushSent += push("sent");
        res.mRequestSkipped += push("request-skipped");
        res.mPushReceived += push("received");
        res.mPushHit += push("hit");
        res.mPushDropped += push("dropped") + push("evicted");
        res.mNetworkFetches += static_cast<int64_t>(
            metrics.NewTimer({"overlay", "fetch", "txset"}).count());
        res.mCoreFetches += static_cast<int64_t>(
            metrics.NewTimer({"scp", "fetch", "txset"}).count());
        res.mCoreRetries += static_cast<int64_t>(
            metrics.NewCounter({"scp", "fetch", "txset-retry"}).count());
    }
    return res;
}

// Cranks until `app` closed `lastLedger` and returns the tx-set hash of
// every ledger it closed on the way, keyed by ledger sequence, with the hash
// of the ledger before it.
std::map<uint32_t, std::pair<Hash, Hash>>
crankAndRecordTxSets(Simulation::pointer const& simulation,
                     Application::pointer const& app, uint32_t lastLedger)
{
    std::map<uint32_t, std::pair<Hash, Hash>> res;
    auto& lm = app->getLedgerManager();
    auto const deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(300);
    uint32_t seen = lm.getLastClosedLedgerNum();
    while (seen < lastLedger)
    {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        simulation->crankForAtMost(std::chrono::milliseconds(20), false);
        auto const& lcl = lm.getLastClosedLedgerHeader();
        if (lcl.header.ledgerSeq > seen)
        {
            REQUIRE(lcl.header.ledgerSeq == seen + 1);
            seen = lcl.header.ledgerSeq;
            res[seen] = {lcl.header.scpValue.txSetHash,
                         lcl.header.previousLedgerHash};
        }
    }
    return res;
}

// A 4-node network with a 3-of-4 quorum, every node connected to every
// other one.
Simulation::pointer
makeNetwork(std::function<Config(int)> const& makeConfig)
{
    return Topologies::core(4, 0.75, sha256(getTestConfig().NETWORK_PASSPHRASE),
                            makeConfig);
}
}

TEST_CASE("leader pushes its tx set to followers",
          "[herder][txset-push][simulation]")
{
    auto simulation = makeNetwork(acceleratedConfig);
    simulation->startAllNodes();
    auto nodes = simulation->getNodes();

    uint32_t const lastLedger = 14;
    auto closed = crankAndRecordTxSets(simulation, nodes[0], lastLedger);
    simulation->crankUntil(
        [&]() { return simulation->haveAllExternalized(lastLedger, 3); },
        std::chrono::seconds(60), false);
    REQUIRE(closed.size() >= 10);

    auto counts = countTxSetDissemination(nodes);
    INFO(counts.toString());

    // Every closed ledger had a leader that pushed its set to its 3 peers
    // (a few ledgers may still be in flight when the counters are read).
    auto const ledgers = static_cast<int64_t>(closed.size());
    REQUIRE(counts.mPushSent >= 3 * (ledgers - 2));
    // A follower either already held the pushed set when Core asked for it
    // (a hit), or its request crossed the push and was completed by it. On
    // a loopback network the push normally wins.
    REQUIRE(counts.mPushHit > 0);
    REQUIRE(counts.mPushHit + counts.mNetworkFetches >= 3 * (ledgers - 2));
    // Every follower got every set from its overlay ...
    REQUIRE(counts.mCoreFetches >= 3 * (ledgers - 2));
    // ... and none had to fall back to the one-second retry: a request that
    // the leader skipped (because it had pushed the set to that peer) was
    // always completed by the pushed copy.
    REQUIRE(counts.mCoreRetries == 0);
    REQUIRE(counts.mPushDropped == 0);
}

TEST_CASE("consensus uses the request path when the tx set push is lost",
          "[herder][txset-push][simulation]")
{
    auto simulation = makeNetwork(acceleratedConfig);
    auto nodes = simulation->getNodes();
    // No node's push leaves Core, as if every push were lost at its source.
    // (A push that is sent and then lost at the receiver is covered by the
    // overlay's own tests: test_dropped_push_is_recovered_by_a_request.)
    for (auto const& app : nodes)
    {
        app->getOverlayManager().setDropTxSetPushForTesting(true);
    }
    simulation->startAllNodes();

    uint32_t const lastLedger = 12;
    auto closed = crankAndRecordTxSets(simulation, nodes[0], lastLedger);
    simulation->crankUntil(
        [&]() { return simulation->haveAllExternalized(lastLedger, 3); },
        std::chrono::seconds(60), false);
    REQUIRE(closed.size() >= 8);

    auto counts = countTxSetDissemination(nodes);
    INFO(counts.toString());
    auto const ledgers = static_cast<int64_t>(closed.size());

    // Nothing was pushed, so nothing arrived unrequested and no request was
    // skipped: every follower asked the network for every set, as on the
    // base, and got it without waiting for the retry.
    REQUIRE(counts.mPushSent == 0);
    REQUIRE(counts.mPushReceived == 0);
    REQUIRE(counts.mPushHit == 0);
    REQUIRE(counts.mRequestSkipped == 0);
    REQUIRE(counts.mNetworkFetches >= 3 * (ledgers - 2));
    REQUIRE(counts.mCoreFetches >= 3 * (ledgers - 2));
    REQUIRE(counts.mCoreRetries == 0);
}

TEST_CASE("some nodes lose the tx set push and still follow",
          "[herder][txset-push][simulation]")
{
    auto simulation = makeNetwork(acceleratedConfig);
    auto nodes = simulation->getNodes();
    // Nodes 0 and 1 push when they lead; the pushes of nodes 2 and 3 are
    // lost. The network sees both paths on alternating ledgers.
    nodes[2]->getOverlayManager().setDropTxSetPushForTesting(true);
    nodes[3]->getOverlayManager().setDropTxSetPushForTesting(true);
    simulation->startAllNodes();

    uint32_t const lastLedger = 20;
    auto closed = crankAndRecordTxSets(simulation, nodes[0], lastLedger);
    simulation->crankUntil(
        [&]() { return simulation->haveAllExternalized(lastLedger, 3); },
        std::chrono::seconds(60), false);

    auto counts = countTxSetDissemination(nodes);
    INFO(counts.toString());
    REQUIRE(closed.size() >= 16);
    REQUIRE(counts.mCoreRetries == 0);
    REQUIRE(counts.mCoreFetches >=
            3 * (static_cast<int64_t>(closed.size()) - 2));
}

// Every leader pushes the set it built (A) but proposes another one (B): a
// push whose content does not match the hash in the leader's SCP statement.
// Followers must not use A. They request B, validate it and externalize it.
//
// B is a tx set without transactions that is not encoded like
// `TxSetXDRFrame::makeEmpty` (its parallel Soroban phase carries a base
// fee). It is valid before CANONICAL_EMPTY_SOROBAN_PHASE_PROTOCOL_VERSION,
// so the network runs on the protocol before that gate. A, built from an
// empty mempool, is the canonical empty set.
TEST_CASE("pushed tx set that does not match the proposal is not used",
          "[herder][txset-push][simulation]")
{
    auto const protocolVersion =
        static_cast<uint32_t>(CANONICAL_EMPTY_SOROBAN_PHASE_PROTOCOL_VERSION) -
        1;
    auto makeConfig = [&](int i) {
        auto cfg = acceleratedConfig(i);
        cfg.LEDGER_PROTOCOL_VERSION = protocolVersion;
        cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION = protocolVersion;
        return cfg;
    };
    auto makeProposedTxSet = [](Hash const& prevHash) {
        GeneralizedTransactionSet xdr(1);
        xdr.v1TxSet().previousLedgerHash = prevHash;
        xdr.v1TxSet().phases.emplace_back(0);
        xdr.v1TxSet()
            .phases.emplace_back(1)
            .parallelTxsComponent()
            .baseFee.activate() = 12345;
        return TxSetXDRFrame::makeFromWire(xdr);
    };

    auto simulation = makeNetwork(makeConfig);
    auto nodes = simulation->getNodes();
    for (auto const& app : nodes)
    {
        static_cast<HerderImpl&>(app->getHerder()).mProposedTxSetOverride =
            [&](LedgerHeaderHistoryEntry const& lcl) {
                return makeProposedTxSet(lcl.hash);
            };
    }
    simulation->startAllNodes();

    uint32_t const lastLedger = 12;
    auto closed = crankAndRecordTxSets(simulation, nodes[1], lastLedger);
    simulation->crankUntil(
        [&]() { return simulation->haveAllExternalized(lastLedger, 3); },
        std::chrono::seconds(60), false);
    REQUIRE(closed.size() >= 8);

    // Every ledger externalized the proposed set B, never the pushed set A.
    for (auto const& [seq, hashes] : closed)
    {
        auto const& [txSetHash, prevHash] = hashes;
        CAPTURE(seq);
        REQUIRE(txSetHash == makeProposedTxSet(prevHash)->getContentsHash());
    }

    auto counts = countTxSetDissemination(nodes);
    INFO(counts.toString());
    auto const ledgers = static_cast<int64_t>(closed.size());
    // The built sets were pushed and arrived ...
    REQUIRE(counts.mPushSent >= 3 * (ledgers - 2));
    REQUIRE(counts.mPushReceived > 0);
    // ... but no Core request was ever answered from one: the proposed
    // hash is not the hash of anything that was pushed. Every follower
    // fetched the proposed set through the request path instead, and the
    // leader answered (it never pushed that set, so it skipped no request).
    REQUIRE(counts.mPushHit == 0);
    REQUIRE(counts.mRequestSkipped == 0);
    REQUIRE(counts.mNetworkFetches >= 3 * (ledgers - 2));
    REQUIRE(counts.mCoreRetries == 0);
}
