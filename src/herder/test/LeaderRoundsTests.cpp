// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// Network-level tests of consensus without nomination: the leader of each
// round comes from a deterministic schedule derived from the last closed
// ledger header (see herder/LeaderSchedule.h).

#include "crypto/SHA.h"
#include "herder/Herder.h"
#include "herder/HerderImpl.h"
#include "herder/LeaderSchedule.h"
#include "ledger/LedgerHeaderUtils.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "simulation/Simulation.h"
#include "simulation/Topologies.h"
#include "test/Catch2.h"
#include "test/test.h"
#include "util/Logging.h"
#include "util/MetricsRegistry.h"
#include "util/XDROperators.h"
#include <fmt/format.h>
#include <map>
#include <set>

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

struct ClosedLedger
{
    LedgerHeader mPrevious;
    NodeID mProposer;
    Hash mResultHash;
};

// Cranks until `app` closed `lastLedger`, recording for every ledger it
// closes on the way the header of the ledger before it (what its leader
// schedule is derived from) and the node that signed the externalized value
// (its proposer).
std::map<uint32_t, ClosedLedger>
crankAndRecord(Simulation::pointer const& simulation,
               Application::pointer const& app, uint32_t lastLedger,
               std::chrono::seconds timeout)
{
    std::map<uint32_t, ClosedLedger> res;
    auto& lm = app->getLedgerManager();
    auto prev = lm.getLastClosedLedgerHeader();
    auto const deadline = std::chrono::steady_clock::now() + timeout;
    while (lm.getLastClosedLedgerNum() < lastLedger)
    {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        simulation->crankForAtMost(std::chrono::milliseconds(20), false);
        auto cur = lm.getLastClosedLedgerHeader();
        if (cur.header.ledgerSeq == prev.header.ledgerSeq + 1)
        {
            REQUIRE(cur.header.previousLedgerHash == prev.hash);
            res[cur.header.ledgerSeq] = ClosedLedger{
                prev.header, getLcValueSignature(cur.header.scpValue).nodeID,
                cur.header.txSetResultHash};
        }
        prev = cur;
    }
    return res;
}

int64_t
leaderTimeouts(Application::pointer const& app)
{
    return static_cast<int64_t>(
        app->getMetrics().NewHistogram({"scp", "timeout", "leader"}).sum());
}

int64_t
ballotTimeouts(Application::pointer const& app)
{
    return static_cast<int64_t>(
        app->getMetrics().NewHistogram({"scp", "timeout", "prepare"}).sum());
}
}

TEST_CASE("leader schedule drives consensus on a healthy network",
          "[herder][leader-schedule][simulation]")
{
    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    auto simulation = Topologies::core(4, 0.75, networkID, acceleratedConfig);
    simulation->startAllNodes();
    auto nodes = simulation->getNodes();
    auto nodeIDs = simulation->getNodeIDs();

    uint32_t const lastLedger = 16;
    auto closed = crankAndRecord(simulation, nodes[0], lastLedger,
                                 std::chrono::seconds(300));
    simulation->crankUntil(
        [&]() { return simulation->haveAllExternalized(lastLedger, 3); },
        std::chrono::seconds(60), false);

    int64_t timeouts = 0;
    for (auto const& n : nodes)
    {
        timeouts += leaderTimeouts(n) + ballotTimeouts(n);
    }

    // Every node computes the same schedule from the previous ledger's
    // header, and the value that externalizes is the one its round 1 leader
    // proposed (unless a round timed out somewhere, e.g. on a busy test
    // machine). These ledgers are empty, so no leader keeps its term: the
    // node that closed the previous ledger goes last, and the order seeded
    // by the ledger before it is otherwise kept.
    std::set<NodeID> proposers;
    int64_t notFirstLeader = 0;
    REQUIRE(closed.size() >= 10);
    for (auto const& c : closed)
    {
        auto schedule =
            computeNextLeaderSchedule(c.second.mPrevious, nodeIDs, true);
        auto expected = computeLeaderSchedule(
            c.second.mPrevious.previousLedgerHash, nodeIDs);
        if (c.second.mPrevious.scpValue.ext.v() != STELLAR_VALUE_BASIC)
        {
            auto const previousProposer =
                getLcValueSignature(c.second.mPrevious.scpValue).nodeID;
            auto it =
                std::find(expected.begin(), expected.end(), previousProposer);
            REQUIRE(it != expected.end());
            std::rotate(it, it + 1, expected.end());
            // so two ledgers in a row are never led by the same node here
            REQUIRE(schedule.front() != previousProposer);
        }
        REQUIRE(schedule == expected);
        REQUIRE(schedule.size() == 4);
        REQUIRE(std::find(schedule.begin(), schedule.end(),
                          c.second.mProposer) != schedule.end());
        if (c.second.mProposer != schedule[0])
        {
            ++notFirstLeader;
        }
        proposers.insert(c.second.mProposer);
    }
    INFO(fmt::format("ledgers {} not-first-leader {} timeouts {}",
                     closed.size(), notFirstLeader, timeouts));
    REQUIRE(notFirstLeader <= timeouts);
    // most ledgers close in round 1
    REQUIRE(notFirstLeader * 2 < static_cast<int64_t>(closed.size()));
    // leadership rotates
    REQUIRE(proposers.size() >= 2);

    // the nodes agree on the schedule of the next ledger
    auto const next = nodes[0]->getLedgerManager().getLastClosedLedgerNum() + 1;
    std::optional<std::vector<NodeID>> expected;
    for (auto const& n : nodes)
    {
        if (n->getLedgerManager().getLastClosedLedgerNum() + 1 != next)
        {
            continue;
        }
        auto& driver =
            static_cast<HerderImpl&>(n->getHerder()).getHerderSCPDriver();
        auto schedule = driver.getLeaderSchedule(next);
        REQUIRE(schedule.size() == 4);
        if (!expected)
        {
            expected = schedule;
        }
        REQUIRE(*expected == schedule);
        REQUIRE(driver.getLeaderSchedule(next + 1).empty());
        REQUIRE(driver.getLeaderSchedule(next - 1).empty());
    }
}

TEST_CASE("leader schedule fails over when a validator is down",
          "[herder][leader-schedule][simulation]")
{
    // 3 validators, threshold 2, one never started. Whenever the dead node
    // leads round 1 the others time out and the round 2 leader takes over.
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
    NodeID const dead = keys[2].getPublicKey();

    simulation->addNode(keys[0], qSet);
    simulation->addNode(keys[1], qSet);
    simulation->addPendingConnection(keys[0].getPublicKey(),
                                     keys[1].getPublicKey());
    simulation->startAllNodes();
    auto nodes = simulation->getNodes();

    uint32_t const lastLedger = 25;
    auto closed = crankAndRecord(simulation, nodes[0], lastLedger,
                                 std::chrono::seconds(600));
    simulation->crankUntil(
        [&]() { return simulation->haveAllExternalized(lastLedger, 3); },
        std::chrono::seconds(60), false);

    int64_t deadLedRound1 = 0;
    REQUIRE(closed.size() >= 20);
    for (auto const& c : closed)
    {
        // the schedule covers the whole quorum set, including the dead node
        auto schedule =
            computeNextLeaderSchedule(c.second.mPrevious, allNodes, true);
        REQUIRE(schedule.size() == 3);
        // every ledger closed anyway, with a live node's proposal
        REQUIRE(c.second.mProposer != dead);
        if (schedule[0] == dead)
        {
            ++deadLedRound1;
        }
    }
    INFO(fmt::format("ledgers {} led by the dead node {}", closed.size(),
                     deadLedRound1));
    // 1/3 of the ledgers on average: (2/3)^20 that it never happens
    REQUIRE(deadLedRound1 > 0);
    // and each of those cost the live nodes a leader timeout
    for (auto const& n : nodes)
    {
        REQUIRE(leaderTimeouts(n) >= deadLedRound1);
    }
}
