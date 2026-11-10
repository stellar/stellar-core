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
