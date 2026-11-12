// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SHA.h"
#include "crypto/SecretKey.h"
#include "herder/LeaderSchedule.h"
#include "test/Catch2.h"
#include "util/Math.h"
#include "util/XDROperators.h"
#include <algorithm>
#include <fmt/format.h>
#include <functional>
#include <map>
#include <set>

using namespace stellar;

namespace
{
std::vector<NodeID>
makeNodes(size_t n)
{
    std::vector<NodeID> nodes;
    for (size_t i = 0; i < n; i++)
    {
        nodes.emplace_back(
            SecretKey::fromSeed(sha256(fmt::format("SCHEDULE_NODE_{}", i)))
                .getPublicKey());
    }
    return nodes;
}
}

TEST_CASE("leader schedule is a deterministic permutation",
          "[herder][leader-schedule]")
{
    auto nodes = makeNodes(30);
    Hash seed = sha256("ledger header 41");

    auto schedule = computeLeaderSchedule(seed, nodes);

    SECTION("it is a permutation of the node set")
    {
        REQUIRE(schedule.size() == nodes.size());
        REQUIRE(std::set<NodeID>(schedule.begin(), schedule.end()) ==
                std::set<NodeID>(nodes.begin(), nodes.end()));
    }
    SECTION("it does not depend on the order nodes are listed in")
    {
        // every node enumerates its quorum in its own order
        for (int i = 0; i < 20; i++)
        {
            auto shuffled = nodes;
            stellar::shuffle(shuffled.begin(), shuffled.end(),
                             getGlobalRandomEngine());
            REQUIRE(computeLeaderSchedule(seed, shuffled) == schedule);
        }
    }
    SECTION("duplicates are ignored")
    {
        auto dup = nodes;
        dup.insert(dup.end(), nodes.begin(), nodes.begin() + 7);
        REQUIRE(computeLeaderSchedule(seed, dup) == schedule);
    }
    SECTION("the seed changes the order")
    {
        auto other = computeLeaderSchedule(sha256("ledger header 42"), nodes);
        REQUIRE(std::set<NodeID>(other.begin(), other.end()) ==
                std::set<NodeID>(nodes.begin(), nodes.end()));
        REQUIRE(other != schedule);
    }
    SECTION("the node set changes the order only by the nodes involved")
    {
        // removing a node leaves the relative order of the others alone
        auto fewer = nodes;
        NodeID removed = fewer.back();
        fewer.pop_back();
        auto expected = schedule;
        expected.erase(std::find(expected.begin(), expected.end(), removed));
        REQUIRE(computeLeaderSchedule(seed, fewer) == expected);
    }
    SECTION("degenerate sets")
    {
        REQUIRE(computeLeaderSchedule(seed, {}).empty());
        REQUIRE(computeLeaderSchedule(seed, {nodes[3]}) ==
                std::vector<NodeID>{nodes[3]});
    }
}

TEST_CASE("leader schedule is fair across seeds", "[herder][leader-schedule]")
{
    // Each node should lead round 1 about 1/n of the time, and the node
    // leading round 2 should be independent of the one leading round 1.
    size_t const n = 10;
    size_t const slots = 20000;
    auto nodes = makeNodes(n);
    std::map<NodeID, size_t> first;
    std::map<std::pair<NodeID, NodeID>, size_t> firstTwo;
    for (size_t i = 0; i < slots; i++)
    {
        auto schedule =
            computeLeaderSchedule(sha256(fmt::format("header {}", i)), nodes);
        first[schedule[0]]++;
        firstTwo[{schedule[0], schedule[1]}]++;
    }
    REQUIRE(first.size() == n);
    double const expected = static_cast<double>(slots) / n;
    for (auto const& f : first)
    {
        // 2000 expected, standard deviation ~42: 10% is over 4 sigma
        REQUIRE(f.second > expected * 0.9);
        REQUIRE(f.second < expected * 1.1);
    }
    // all ordered pairs of distinct nodes show up
    REQUIRE(firstTwo.size() == n * (n - 1));
    double const expectedPair = static_cast<double>(slots) / (n * (n - 1));
    for (auto const& f : firstTwo)
    {
        // 222 expected, standard deviation ~15
        REQUIRE(f.second > expectedPair * 0.6);
        REQUIRE(f.second < expectedPair * 1.4);
    }
}

namespace
{
// A closed ledger's header as far as the schedule reads it: `proposer` signed
// its value, and it applied transactions or not.
LedgerHeader
closedLedger(uint32_t seq, NodeID const& proposer, bool appliedTransactions,
             Hash const& previousLedgerHash)
{
    LedgerHeader header;
    header.ledgerSeq = seq;
    header.previousLedgerHash = previousLedgerHash;
    header.scpValue.ext.v(STELLAR_VALUE_SIGNED);
    header.scpValue.ext.lcValueSignature().nodeID = proposer;
    header.txSetResultHash = appliedTransactions
                                 ? sha256("some results")
                                 : xdrSha256(TransactionResultSet{});
    return header;
}
}

TEST_CASE("leader term outcome", "[herder][leader-schedule][leader-term]")
{
    auto nodes = makeNodes(7);
    NodeID const proposer = nodes[2];
    Hash const prev = sha256("ledger 100");
    // 101 + 1 is not a multiple of the term length
    REQUIRE(LEADER_TERM_LEDGERS == 5);
    auto header = closedLedger(101, proposer, true, prev);

    SECTION("the node whose value closed the ledger keeps leading")
    {
        for (auto const& outcome :
             {leaderTermOutcome(header, true), leaderTermOutcome(header, false),
              leaderTermOutcome(101, header.scpValue, true)})
        {
            REQUIRE(outcome.mIncumbent == proposer);
            REQUIRE(!outcome.mForfeited);
        }
    }
    SECTION("millisecond values name their proposer too")
    {
        header.scpValue.ext.v(STELLAR_VALUE_SIGNED_MS);
        header.scpValue.ext.signedMsValue().lcValueSignature.nodeID = nodes[4];
        REQUIRE(leaderTermOutcome(header, true).mIncumbent == nodes[4]);
    }
    SECTION("a term ends on every multiple of the term length")
    {
        for (uint32_t seq = 100; seq < 100 + 3 * LEADER_TERM_LEDGERS; ++seq)
        {
            header.ledgerSeq = seq;
            bool const nextStartsTerm = (seq + 1) % LEADER_TERM_LEDGERS == 0;
            auto const outcome = leaderTermOutcome(header, true);
            REQUIRE(outcome.mIncumbent.has_value() == !nextStartsTerm);
            // the end of a term is not a forfeit
            REQUIRE(!outcome.mForfeited);
        }
    }
    SECTION("a ledger that applied nothing forfeits the term")
    {
        // an invalid ("skipped") transaction set, or an empty one
        auto skipped = closedLedger(101, proposer, false, prev);
        for (auto const& outcome :
             {leaderTermOutcome(skipped, true),
              leaderTermOutcome(101, skipped.scpValue, false)})
        {
            REQUIRE(!outcome.mIncumbent);
            REQUIRE(outcome.mForfeited == proposer);
        }
        // unless ledgers never record results (overlay-only mode)
        REQUIRE(leaderTermOutcome(skipped, false).mIncumbent == proposer);
        REQUIRE(!leaderTermOutcome(skipped, false).mForfeited);
        // also at the end of a term: the new term's order is seeded, and
        // must not start with the node whose ledger applied nothing
        skipped.ledgerSeq = 104;
        REQUIRE(!leaderTermOutcome(skipped, true).mIncumbent);
        REQUIRE(leaderTermOutcome(skipped, true).mForfeited == proposer);
    }
    SECTION("a consensus-level empty-tx-set value forfeits the term")
    {
        header.scpValue.ext.v(STELLAR_VALUE_EMPTY_TX_SET);
        header.scpValue.ext.proposedValue().lcValueSignature.nodeID = proposer;
        for (bool haveResults : {true, false})
        {
            auto const outcome = leaderTermOutcome(header, haveResults);
            REQUIRE(!outcome.mIncumbent);
            REQUIRE(outcome.mForfeited == proposer);
        }
        header.scpValue.ext.v(STELLAR_VALUE_EMPTY_TX_SET_MS);
        header.scpValue.ext.proposedMsValue().lcValueSignature.nodeID =
            proposer;
        REQUIRE(leaderTermOutcome(header, false).mForfeited == proposer);
    }
    SECTION("an unsigned value names nobody")
    {
        header.scpValue.ext.v(STELLAR_VALUE_BASIC);
        REQUIRE(!leaderTermOutcome(header, true).mIncumbent);
        REQUIRE(!leaderTermOutcome(header, true).mForfeited);
    }
}

TEST_CASE("next leader schedule", "[herder][leader-schedule][leader-term]")
{
    auto nodes = makeNodes(9);
    NodeID const proposer = nodes[5];
    Hash const prev = sha256("ledger 100");
    auto header = closedLedger(101, proposer, true, prev);
    auto const seeded = computeLeaderSchedule(prev, nodes);
    auto others = seeded;
    others.erase(std::find(others.begin(), others.end(), proposer));

    SECTION("the incumbent leads round 1, the seeded order follows")
    {
        auto schedule = computeNextLeaderSchedule(header, nodes, true);
        REQUIRE(schedule.size() == nodes.size());
        REQUIRE(schedule[0] == proposer);
        REQUIRE(std::vector<NodeID>(schedule.begin() + 1, schedule.end()) ==
                others);
    }
    SECTION("the order is seeded by the hash of the ledger before")
    {
        // Known one ledger early: nothing else of the closed ledger's
        // header moves the order.
        auto fresh = closedLedger(104, proposer, true, prev);
        REQUIRE(computeNextLeaderSchedule(fresh, nodes, true) == seeded);
        fresh.txSetResultHash = sha256("other results");
        fresh.scpValue.txSetHash = sha256("other set");
        fresh.bucketListHash = sha256("other state");
        REQUIRE(computeNextLeaderSchedule(fresh, nodes, true) == seeded);
        fresh.previousLedgerHash = sha256("another ledger 103");
        REQUIRE(computeNextLeaderSchedule(fresh, nodes, true) ==
                computeLeaderSchedule(fresh.previousLedgerHash, nodes));
    }
    SECTION("a leader that forfeited its term goes last")
    {
        // whatever the seeded order says, including when it would have put
        // that node first
        for (auto const& forfeiter : nodes)
        {
            auto skipped = closedLedger(101, forfeiter, false, prev);
            auto schedule = computeNextLeaderSchedule(skipped, nodes, true);
            REQUIRE(schedule.size() == nodes.size());
            REQUIRE(schedule.back() == forfeiter);
            REQUIRE(schedule.front() != forfeiter);
            auto rest = seeded;
            rest.erase(std::find(rest.begin(), rest.end(), forfeiter));
            REQUIRE(std::vector<NodeID>(schedule.begin(), schedule.end() - 1) ==
                    rest);
        }
    }
    SECTION("a new term takes the seeded order as it is")
    {
        auto last = closedLedger(104, proposer, true, prev);
        REQUIRE(computeNextLeaderSchedule(last, nodes, true) == seeded);
    }
    SECTION("unless the last ledger of the old term forfeited")
    {
        for (auto const& forfeiter : nodes)
        {
            auto last = closedLedger(104, forfeiter, false, prev);
            auto schedule = computeNextLeaderSchedule(last, nodes, true);
            REQUIRE(schedule.back() == forfeiter);
            REQUIRE(schedule.front() != forfeiter);
        }
    }
    SECTION("an incumbent or forfeiter outside the node set is ignored")
    {
        auto fewer = nodes;
        fewer.erase(std::find(fewer.begin(), fewer.end(), proposer));
        REQUIRE(computeNextLeaderSchedule(header, fewer, true) ==
                computeLeaderSchedule(prev, fewer));
        auto skipped = closedLedger(101, proposer, false, prev);
        REQUIRE(computeNextLeaderSchedule(skipped, fewer, true) ==
                computeLeaderSchedule(prev, fewer));
    }
    SECTION("every node computes the same schedule")
    {
        for (bool applied : {true, false})
        {
            auto closed = closedLedger(101, proposer, applied, prev);
            auto schedule = computeNextLeaderSchedule(closed, nodes, true);
            for (int i = 0; i < 10; i++)
            {
                auto shuffled = nodes;
                stellar::shuffle(shuffled.begin(), shuffled.end(),
                                 getGlobalRandomEngine());
                REQUIRE(computeNextLeaderSchedule(closed, shuffled, true) ==
                        schedule);
            }
        }
    }
    SECTION("the prediction made during the ballot matches the schedule")
    {
        // While ledger 101 is in its ballot, its header does not exist yet:
        // the seed (the hash of ledger 100) and the value do.
        REQUIRE(
            computeLeaderSchedule(
                prev, nodes, leaderTermOutcome(101, header.scpValue, true)) ==
            computeNextLeaderSchedule(header, nodes, true));
        auto skipped = closedLedger(101, proposer, false, prev);
        REQUIRE(
            computeLeaderSchedule(
                prev, nodes, leaderTermOutcome(101, skipped.scpValue, false)) ==
            computeNextLeaderSchedule(skipped, nodes, true));
    }
}

TEST_CASE("leader terms over a chain of ledgers",
          "[herder][leader-schedule][leader-term]")
{
    // Every ledger is closed by its round 1 leader, except where
    // `roundOneFails` says the leader timed out and round 2's leader closed
    // it, or `appliesNothing` says its set was skipped.
    size_t const n = 10;
    auto nodes = makeNodes(n);
    auto run = [&](std::function<bool(uint32_t)> roundOneFails,
                   std::function<bool(uint32_t)> appliesNothing) {
        std::map<uint32_t, NodeID> leaders;
        std::map<uint32_t, NodeID> proposers;
        LedgerHeader lcl =
            closedLedger(1, nodes[0], false, sha256("genesis parent"));
        for (uint32_t seq = 2; seq < 400; ++seq)
        {
            auto schedule = computeNextLeaderSchedule(lcl, nodes, true);
            REQUIRE(schedule.size() == n);
            leaders[seq] = schedule[0];
            auto const& proposer =
                roundOneFails(seq) ? schedule[1] : schedule[0];
            proposers[seq] = proposer;
            lcl = closedLedger(seq, proposer, !appliesNothing(seq),
                               xdrSha256(lcl));
        }
        return std::make_pair(leaders, proposers);
    };
    auto never = [](uint32_t) { return false; };

    SECTION("a healthy leader leads a whole term, and no longer")
    {
        auto [leaders, proposers] = run(never, never);
        std::set<NodeID> seen;
        for (uint32_t seq = 5; seq + LEADER_TERM_LEDGERS < 400;
             seq += LEADER_TERM_LEDGERS)
        {
            for (uint32_t i = 1; i < LEADER_TERM_LEDGERS; ++i)
            {
                REQUIRE(leaders[seq + i] == leaders[seq]);
            }
            seen.insert(leaders[seq]);
        }
        // terms are handed out by the seeded order: everyone gets some
        REQUIRE(seen.size() == n);
    }
    SECTION("a timeout hands the rest of the term to the node that took over")
    {
        // round 1 fails in the second ledger of every term
        auto fails = [](uint32_t seq) {
            return seq % LEADER_TERM_LEDGERS == 1;
        };
        auto [leaders, proposers] = run(fails, never);
        for (uint32_t seq = 10; seq + LEADER_TERM_LEDGERS < 400;
             seq += LEADER_TERM_LEDGERS)
        {
            // the term's first leader led two ledgers, and lost the second
            REQUIRE(leaders[seq + 1] == leaders[seq]);
            REQUIRE(proposers[seq + 1] != leaders[seq]);
            // from the next ledger on, the node that closed it leads
            for (uint32_t i = 2; i < LEADER_TERM_LEDGERS; ++i)
            {
                REQUIRE(leaders[seq + i] == proposers[seq + 1]);
                REQUIRE(leaders[seq + i] != leaders[seq]);
            }
        }
    }
    SECTION("a ledger that applied nothing rotates the leader at once")
    {
        auto skipped = [](uint32_t seq) {
            return seq % LEADER_TERM_LEDGERS == 2;
        };
        auto [leaders, proposers] = run(never, skipped);
        for (uint32_t seq = 10; seq + LEADER_TERM_LEDGERS < 400;
             seq += LEADER_TERM_LEDGERS)
        {
            REQUIRE(leaders[seq + 1] == leaders[seq]);
            REQUIRE(leaders[seq + 2] == leaders[seq]);
            // The ledger after the skipped one is never led by the node
            // whose set was skipped, and the node that takes over keeps the
            // rest of the term.
            REQUIRE(leaders[seq + 3] != leaders[seq]);
            REQUIRE(leaders[seq + 4] == leaders[seq + 3]);
        }
    }
}
