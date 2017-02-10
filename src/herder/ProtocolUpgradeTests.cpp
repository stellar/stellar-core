// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/Herder.h"
#include "herder/LedgerCloseData.h"
#include "lib/catch.hpp"
#include "overlay/LoopbackPeer.h"
#include "simulation/Simulation.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Timer.h"
#include "util/optional.h"

using namespace stellar;

struct LedgerUpgradeNode
{
    int ledgerProtocolVersion;
    optional<std::tm> preferredUpdateDatetime;
    std::vector<int> quorumIndexes;
    int quorumTheshold;
};

struct LedgerUpgradeSimulationResult
{
    std::tm time;
    std::vector<int> expectedLedgerProtocolVersions;
};

struct LedgerUpgradeSimulation
{
    std::vector<LedgerUpgradeNode> nodes;
    std::vector<LedgerUpgradeSimulationResult> results;
    std::vector<int> expectedLedgerProtocolVersions;
};

void
simulateLedgerUpgrade(const LedgerUpgradeSimulation& upgradeSimulation)
{
    auto const& nodes = upgradeSimulation.nodes;

    auto start = getTestDate(1, 7, 2014);
    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    auto simulation =
        std::make_shared<Simulation>(Simulation::OVER_LOOPBACK, networkID);
    simulation->getClock().setCurrentTime(VirtualClock::from_time_t(start));

    auto keys = std::vector<SecretKey>{};
    auto configs = std::vector<Config>{};
    for (auto i = 0; i < nodes.size(); i++)
    {
        keys.push_back(
            SecretKey::fromSeed(sha256("NODE_SEED_" + std::to_string(i))));
        configs.push_back(simulation->newConfig());
        configs.back().LEDGER_PROTOCOL_VERSION = nodes[i].ledgerProtocolVersion;
        configs.back().PREFERRED_UPGRADE_DATETIME =
            nodes[i].preferredUpdateDatetime;
    }

    for (auto i = 0; i < nodes.size(); i++)
    {
        auto qSet = SCPQuorumSet{};
        qSet.threshold = nodes[i].quorumTheshold;
        for (auto j : nodes[i].quorumIndexes)
            qSet.validators.push_back(keys[j].getPublicKey());
        simulation->addNode(keys[i], qSet, simulation->getClock(), &configs[i]);
    }

    for (auto i = 0; i < nodes.size(); i++)
        for (auto j = i + 1; j < nodes.size(); j++)
            simulation->addPendingConnection(keys[i].getPublicKey(),
                                             keys[j].getPublicKey());

    simulation->startAllNodes();

    for (auto const& result : upgradeSimulation.results)
    {
        simulation->crankUntil(VirtualClock::tmToPoint(result.time), false);

        for (auto i = 0; i < nodes.size(); i++)
        {
            auto const& node = simulation->getNode(keys[i].getPublicKey());
            REQUIRE(node->getLedgerManager()
                        .getCurrentLedgerHeader()
                        .ledgerVersion ==
                    result.expectedLedgerProtocolVersions[i]);
        }
    }

    simulation->crankForAtLeast(std::chrono::seconds{20}, true);

    for (auto i = 0; i < nodes.size(); i++)
    {
        auto const& node = simulation->getNode(keys[i].getPublicKey());
        REQUIRE(
            node->getLedgerManager().getCurrentLedgerHeader().ledgerVersion ==
            upgradeSimulation.expectedLedgerProtocolVersions[i]);
    }
}

TEST_CASE("0 nodes vote for upgrading ledger - keep old version",
          "[herder][upgrade]")
{
    simulateLedgerUpgrade(
        {{{0, {}, {0, 1}, 2}, {0, {}, {0, 1}, 2}}, {}, {0, 0}});
}

TEST_CASE("1 of 2 nodes vote for upgrading ledger - keep old version",
          "[herder][upgrade]")
{
    simulateLedgerUpgrade(
        {{{0, {}, {0, 1}, 2}, {1, {}, {0, 1}, 2}}, {}, {0, 0}});
}

TEST_CASE("2 of 2 nodes vote for upgrading ledger - upgrade",
          "[herder][upgrade]")
{
    simulateLedgerUpgrade(
        {{{1, {}, {0, 1}, 2}, {1, {}, {0, 1}, 2}}, {}, {1, 1}});
}

TEST_CASE("1 of 3 nodes vote for upgrading ledger - keep old version",
          "[herder][upgrade]")
{
    simulateLedgerUpgrade(
        {{{1, {}, {0, 1, 2}, 2}, {0, {}, {0, 1, 2}, 2}, {0, {}, {0, 1, 2}, 2}},
         {},
         {0, 0, 0}});
}

TEST_CASE("2 of 3 nodes vote for upgrading ledger - upgrade, one node desynced",
          "[herder][upgrade]")
{
    simulateLedgerUpgrade(
        {{{1, {}, {0, 1, 2}, 2}, {1, {}, {0, 1, 2}, 2}, {0, {}, {0, 1, 2}, 2}},
         {},
         {1, 1, 0}});
}

TEST_CASE("2 of 2 nodes vote for upgrade at some time - upgrade at this time",
          "[herder][upgrade]")
{
    simulateLedgerUpgrade(
        {{{1,
           make_optional<std::tm>(getTestDateTime(1, 7, 2014, 0, 1, 0)),
           {0, 1},
           2},
          {1,
           make_optional<std::tm>(getTestDateTime(1, 7, 2014, 0, 1, 0)),
           {0, 1},
           2}},
         {{getTestDateTime(1, 7, 2014, 0, 0, 30), {0, 0}},
          {getTestDateTime(1, 7, 2014, 0, 1, 30), {1, 1}}},
         {1, 1}});
}

TEST_CASE(
    "2 of 2 nodes vote for upgrade at some time - upgrade at earlier time",
    "[herder][upgrade]")
{
    simulateLedgerUpgrade(
        {{{1,
           make_optional<std::tm>(getTestDateTime(1, 7, 2014, 0, 0, 30)),
           {0, 1},
           2},
          {1,
           make_optional<std::tm>(getTestDateTime(1, 7, 2014, 0, 1, 0)),
           {0, 1},
           2}},
         {{getTestDateTime(1, 7, 2014, 0, 0, 25), {0, 0}},
          {getTestDateTime(1, 7, 2014, 0, 0, 45), {1, 1}}},
         {1, 1}});
}

TEST_CASE("3 of 3 nodes vote for upgrade at some time; 2 on earlier time - "
          "upgrade at earlier time",
          "[herder][upgrade]")
{
    simulateLedgerUpgrade(
        {{{1,
           make_optional<std::tm>(getTestDateTime(1, 7, 2014, 0, 0, 30)),
           {0, 1},
           2},
          {1,
           make_optional<std::tm>(getTestDateTime(1, 7, 2014, 0, 0, 30)),
           {0, 1},
           2},
          {1,
           make_optional<std::tm>(getTestDateTime(1, 7, 2014, 0, 1, 0)),
           {0, 1},
           2}},
         {{getTestDateTime(1, 7, 2014, 0, 0, 25), {0, 0, 0}},
          {getTestDateTime(1, 7, 2014, 0, 0, 45), {1, 1, 1}}},
         {1, 1, 1}});
}

TEST_CASE("3 of 3 nodes vote for upgrade at some time; 1 on earlier time - "
          "upgrade at earlier time",
          "[herder][upgrade]")
{
    simulateLedgerUpgrade(
        {{{1,
           make_optional<std::tm>(getTestDateTime(1, 7, 2014, 0, 0, 30)),
           {0, 1},
           2},
          {1,
           make_optional<std::tm>(getTestDateTime(1, 7, 2014, 0, 1, 0)),
           {0, 1},
           2},
          {1,
           make_optional<std::tm>(getTestDateTime(1, 7, 2014, 0, 1, 0)),
           {0, 1},
           2}},
         {{getTestDateTime(1, 7, 2014, 0, 0, 25), {0, 0, 0}},
          {getTestDateTime(1, 7, 2014, 0, 0, 45), {1, 1, 1}}},
         {1, 1, 1}});
}
