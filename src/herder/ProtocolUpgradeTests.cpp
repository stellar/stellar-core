// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/Herder.h"
#include "herder/LedgerCloseData.h"
#include "lib/catch.hpp"
#include "simulation/Simulation.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Timer.h"
#include "util/optional.h"

using namespace stellar;

struct LedgerUpdateQuorum
{
    std::vector<int> quorumIndexes;
    int quorumTheshold;
};

struct LedgerUpgradeNode
{
    int ledgerProtocolVersion;
    optional<std::tm> preferredUpdateDatetime;
    LedgerUpdateQuorum quorum;
};

struct LedgerUpgradeCheck
{
    std::tm time;
    std::vector<uint32_t> expectedLedgerProtocolVersions;
};

optional<std::tm>
upgradeAt(int minute, int second)
{
    return make_optional<std::tm>(
        getTestDateTime(1, 7, 2014, 0, minute, second));
}

std::tm
checkAt(int minute, int second)
{
    return getTestDateTime(1, 7, 2014, 0, minute, second);
}

void
simulateLedgerUpgrade(std::vector<LedgerUpgradeNode> const& nodes,
                      std::vector<LedgerUpgradeCheck> const& checks)
{
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
        qSet.threshold = nodes[i].quorum.quorumTheshold;
        for (auto j : nodes[i].quorum.quorumIndexes)
            qSet.validators.push_back(keys[j].getPublicKey());
        simulation->addNode(keys[i], qSet, simulation->getClock(), &configs[i]);
    }

    for (auto i = 0; i < nodes.size(); i++)
        for (auto j = i + 1; j < nodes.size(); j++)
            simulation->addPendingConnection(keys[i].getPublicKey(),
                                             keys[j].getPublicKey());

    simulation->startAllNodes();

    for (auto const& result : checks)
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
}

TEST_CASE("0 nodes vote for upgrading ledger - keep old version",
          "[herder][upgrade]")
{
    auto quorum = LedgerUpdateQuorum{{0, 1}, 2};
    auto nodes =
        std::vector<LedgerUpgradeNode>{{0, {}, quorum}, {0, {}, quorum}};
    auto checks = std::vector<LedgerUpgradeCheck>{{checkAt(0, 30), {0, 0}}};
    simulateLedgerUpgrade(nodes, checks);
}

TEST_CASE("1 of 2 nodes vote for upgrading ledger - keep old version",
          "[herder][upgrade]")
{
    auto quorum = LedgerUpdateQuorum{{0, 1}, 2};
    auto nodes =
        std::vector<LedgerUpgradeNode>{{0, {}, quorum}, {1, {}, quorum}};
    auto checks = std::vector<LedgerUpgradeCheck>{{checkAt(0, 30), {0, 0}}};
    simulateLedgerUpgrade(nodes, checks);
}

TEST_CASE("2 of 2 nodes vote for upgrading ledger - upgrade",
          "[herder][upgrade]")
{
    auto quorum = LedgerUpdateQuorum{{0, 1}, 2};
    auto nodes =
        std::vector<LedgerUpgradeNode>{{1, {}, quorum}, {1, {}, quorum}};
    auto checks = std::vector<LedgerUpgradeCheck>{{checkAt(0, 30), {1, 1}}};
    simulateLedgerUpgrade(nodes, checks);
}

TEST_CASE("1 of 3 nodes vote for upgrading ledger - keep old version",
          "[herder][upgrade]")
{
    auto quorum = LedgerUpdateQuorum{{0, 1, 2}, 2};
    auto nodes = std::vector<LedgerUpgradeNode>{
        {1, {}, quorum}, {0, {}, quorum}, {0, {}, quorum}};
    auto checks = std::vector<LedgerUpgradeCheck>{{checkAt(0, 30), {0, 0, 0}}};
    simulateLedgerUpgrade(nodes, checks);
}

TEST_CASE("2 of 3 nodes vote for upgrading ledger - upgrade, one node desynced",
          "[herder][upgrade]")
{
    auto quorum = LedgerUpdateQuorum{{0, 1, 2}, 2};
    auto nodes = std::vector<LedgerUpgradeNode>{
        {1, {}, quorum}, {1, {}, quorum}, {0, {}, quorum}};
    auto checks = std::vector<LedgerUpgradeCheck>{{checkAt(0, 30), {1, 1, 0}}};
    simulateLedgerUpgrade(nodes, checks);
}

TEST_CASE("2 of 2 nodes vote for upgrade at some time - upgrade at this time",
          "[herder][upgrade]")
{
    auto quorum = LedgerUpdateQuorum{{0, 1}, 2};
    auto nodes = std::vector<LedgerUpgradeNode>{{1, upgradeAt(1, 0), quorum},
                                                {1, upgradeAt(1, 0), quorum}};
    auto checks = std::vector<LedgerUpgradeCheck>{{checkAt(0, 30), {0, 0}},
                                                  {checkAt(1, 30), {1, 1}}};
    simulateLedgerUpgrade(nodes, checks);
}

TEST_CASE(
    "2 of 2 nodes vote for upgrade at some time - upgrade at earlier time",
    "[herder][upgrade]")
{
    auto quorum = LedgerUpdateQuorum{{0, 1}, 2};
    auto nodes = std::vector<LedgerUpgradeNode>{{1, upgradeAt(0, 30), quorum},
                                                {1, upgradeAt(1, 0), quorum}};
    auto checks = std::vector<LedgerUpgradeCheck>{{checkAt(0, 25), {0, 0}},
                                                  {checkAt(0, 45), {1, 1}}};
    simulateLedgerUpgrade(nodes, checks);
}

TEST_CASE("3 of 3 nodes vote for upgrade at some time; 2 on earlier time - "
          "upgrade at earlier time",
          "[herder][upgrade]")
{
    auto quorum = LedgerUpdateQuorum{{0, 1, 2}, 2};
    auto nodes = std::vector<LedgerUpgradeNode>{{1, upgradeAt(0, 30), quorum},
                                                {1, upgradeAt(0, 30), quorum},
                                                {1, upgradeAt(1, 0), quorum}};
    auto checks = std::vector<LedgerUpgradeCheck>{{checkAt(0, 25), {0, 0, 0}},
                                                  {checkAt(0, 45), {1, 1, 1}}};
    simulateLedgerUpgrade(nodes, checks);
}

TEST_CASE("3 of 3 nodes vote for upgrade at some time; 1 on earlier time - "
          "upgrade at earlier time",
          "[herder][upgrade]")
{
    auto quorum = LedgerUpdateQuorum{{0, 1, 2}, 2};
    auto nodes = std::vector<LedgerUpgradeNode>{{1, upgradeAt(0, 30), quorum},
                                                {1, upgradeAt(1, 0), quorum},
                                                {1, upgradeAt(1, 0), quorum}};
    auto checks = std::vector<LedgerUpgradeCheck>{{checkAt(0, 25), {0, 0, 0}},
                                                  {checkAt(0, 45), {1, 1, 1}}};
    simulateLedgerUpgrade(nodes, checks);
}
