// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/Herder.h"
#include "herder/LedgerCloseData.h"
#include "herder/Upgrades.h"
#include "lib/catch.hpp"
#include "simulation/Simulation.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Timer.h"
#include "util/optional.h"
#include <xdrpp/marshal.h>

using namespace stellar;

struct LedgerUpgradeableData
{
    uint32_t ledgerVersion;
    uint32_t baseFee;
    uint32_t maxTxSetSize;
};

struct LedgerUpgradeNode
{
    LedgerUpgradeableData starting;
    VirtualClock::time_point preferredUpgradeDatetime;
};

struct LedgerUpgradeCheck
{
    VirtualClock::time_point time;
    std::vector<LedgerUpgradeableData> expected;
};

void
simulateUpgrade(std::vector<LedgerUpgradeNode> const& nodes,
                std::vector<LedgerUpgradeCheck> const& checks)
{
    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    auto simulation =
        std::make_shared<Simulation>(Simulation::OVER_LOOPBACK, networkID);
    simulation->setCurrentTime(genesis(0, 0));

    auto keys = std::vector<SecretKey>{};
    auto configs = std::vector<Config>{};
    for (auto i = 0; i < nodes.size(); i++)
    {
        keys.push_back(
            SecretKey::fromSeed(sha256("NODE_SEED_" + std::to_string(i))));
        configs.push_back(simulation->newConfig());
        configs.back().LEDGER_PROTOCOL_VERSION =
            nodes[i].starting.ledgerVersion;
        configs.back().DESIRED_BASE_FEE = nodes[i].starting.baseFee;
        configs.back().DESIRED_MAX_TX_PER_LEDGER =
            nodes[i].starting.maxTxSetSize;
        configs.back().PREFERRED_UPGRADE_DATETIME =
            nodes[i].preferredUpgradeDatetime;
    }

    auto qSet = SCPQuorumSet{};
    qSet.threshold = 2;
    qSet.validators.push_back(keys[0].getPublicKey());
    qSet.validators.push_back(keys[1].getPublicKey());
    qSet.validators.push_back(keys[2].getPublicKey());

    for (auto i = 0; i < nodes.size(); i++)
    {
        simulation->addNode(keys[i], qSet, &configs[i]);
    }

    for (auto i = 0; i < nodes.size(); i++)
    {
        for (auto j = i + 1; j < nodes.size(); j++)
        {
            simulation->addPendingConnection(keys[i].getPublicKey(),
                                             keys[j].getPublicKey());
        }
    }

    simulation->startAllNodes();

    for (auto const& result : checks)
    {
        simulation->crankUntil(result.time, false);

        for (auto i = 0; i < nodes.size(); i++)
        {
            auto const& node = simulation->getNode(keys[i].getPublicKey());
            REQUIRE(node->getLedgerManager()
                        .getCurrentLedgerHeader()
                        .ledgerVersion == result.expected[i].ledgerVersion);
            REQUIRE(node->getLedgerManager().getCurrentLedgerHeader().baseFee ==
                    result.expected[i].baseFee);
            REQUIRE(node->getLedgerManager()
                        .getCurrentLedgerHeader()
                        .maxTxSetSize == result.expected[i].maxTxSetSize);
        }
    }
}

LedgerUpgrade
makeProtocolVersionUpgrade(int version)
{
    auto result = LedgerUpgrade{LEDGER_UPGRADE_VERSION};
    result.newLedgerVersion() = version;
    return result;
}

LedgerUpgrade
makeBaseFeeUpgrade(int baseFee)
{
    auto result = LedgerUpgrade{LEDGER_UPGRADE_BASE_FEE};
    result.newBaseFee() = baseFee;
    return result;
}

LedgerUpgrade
makeTxCountUpgrade(int txCount)
{
    auto result = LedgerUpgrade{LEDGER_UPGRADE_MAX_TX_SET_SIZE};
    result.newMaxTxSetSize() = txCount;
    return result;
}

UpgradeType
toUpgradeType(LedgerUpgrade const& upgrade)
{
    auto v = xdr::xdr_to_opaque(upgrade);
    auto result = UpgradeType{v.begin(), v.end()};
    return result;
}

void
testListUpgrades(VirtualClock::time_point preferredUpgradeDatetime,
                 bool shouldListProtocol)
{
    auto cfg = getTestConfig();
    cfg.LEDGER_PROTOCOL_VERSION = 10;
    cfg.DESIRED_BASE_FEE = 100;
    cfg.DESIRED_MAX_TX_PER_LEDGER = 50;
    cfg.PREFERRED_UPGRADE_DATETIME = preferredUpgradeDatetime;

    auto header = LedgerHeader{};
    header.ledgerVersion = cfg.LEDGER_PROTOCOL_VERSION;
    header.baseFee = cfg.DESIRED_BASE_FEE;
    header.maxTxSetSize = cfg.DESIRED_MAX_TX_PER_LEDGER;
    header.scpValue.closeTime = VirtualClock::to_time_t(genesis(0, 0));

    auto protocolVersionUpgrade =
        makeProtocolVersionUpgrade(cfg.LEDGER_PROTOCOL_VERSION);
    auto baseFeeUpgrade = makeBaseFeeUpgrade(cfg.DESIRED_BASE_FEE);
    auto txCountUpgrade = makeTxCountUpgrade(cfg.DESIRED_MAX_TX_PER_LEDGER);

    SECTION("protocol version upgrade needed")
    {
        header.ledgerVersion--;
        auto upgrades = Upgrades{cfg}.upgradesFor(header);
        auto expected = shouldListProtocol
                            ? std::vector<LedgerUpgrade>{protocolVersionUpgrade}
                            : std::vector<LedgerUpgrade>{};
        REQUIRE(upgrades == expected);
    }

    SECTION("base fee upgrade needed")
    {
        header.baseFee /= 2;
        auto upgrades = Upgrades{cfg}.upgradesFor(header);
        auto expected = std::vector<LedgerUpgrade>{baseFeeUpgrade};
        REQUIRE(upgrades == expected);
    }

    SECTION("tx count upgrade needed")
    {
        header.maxTxSetSize /= 2;
        auto upgrades = Upgrades{cfg}.upgradesFor(header);
        auto expected = std::vector<LedgerUpgrade>{txCountUpgrade};
        REQUIRE(upgrades == expected);
    }

    SECTION("all upgrades needed")
    {
        header.ledgerVersion--;
        header.baseFee /= 2;
        header.maxTxSetSize /= 2;
        auto upgrades = Upgrades{cfg}.upgradesFor(header);
        auto expected =
            shouldListProtocol
                ? std::vector<LedgerUpgrade>{protocolVersionUpgrade,
                                             baseFeeUpgrade, txCountUpgrade}
                : std::vector<LedgerUpgrade>{baseFeeUpgrade, txCountUpgrade};
        REQUIRE(upgrades == expected);
    }
}

TEST_CASE("list upgrades when no time set for upgrade", "[upgrades]")
{
    testListUpgrades({}, true);
}

TEST_CASE("list upgrades just before upgrade time", "[upgrades]")
{
    testListUpgrades(genesis(0, 1), false);
}

TEST_CASE("list upgrades at upgrade time", "[upgrades]")
{
    testListUpgrades(genesis(0, 0), true);
}

void
testValidateUpgrades(VirtualClock::time_point preferredUpgradeDatetime,
                     bool shouldValidateProtocolUpgrade)
{
    auto cfg = getTestConfig();
    cfg.LEDGER_PROTOCOL_VERSION = 10;
    cfg.DESIRED_BASE_FEE = 100;
    cfg.DESIRED_MAX_TX_PER_LEDGER = 50;
    cfg.PREFERRED_UPGRADE_DATETIME = preferredUpgradeDatetime;

    auto checkTime = VirtualClock::to_time_t(genesis(0, 0));
    auto ledgerUpgradeType = LedgerUpgradeType{};

    SECTION("invalid upgrade data")
    {
        REQUIRE(!Upgrades{cfg}.isValid(checkTime, UpgradeType{},
                                       ledgerUpgradeType));
    }

    SECTION("version")
    {
        REQUIRE(shouldValidateProtocolUpgrade ==
                Upgrades{cfg}.isValid(
                    checkTime, toUpgradeType(makeProtocolVersionUpgrade(10)),
                    ledgerUpgradeType));
        REQUIRE(!Upgrades{cfg}.isValid(
            checkTime, toUpgradeType(makeProtocolVersionUpgrade(9)),
            ledgerUpgradeType));
        REQUIRE(!Upgrades{cfg}.isValid(
            checkTime, toUpgradeType(makeProtocolVersionUpgrade(11)),
            ledgerUpgradeType));
    }

    SECTION("base fee")
    {
        REQUIRE(Upgrades{cfg}.isValid(checkTime,
                                      toUpgradeType(makeBaseFeeUpgrade(100)),
                                      ledgerUpgradeType));
        REQUIRE(!Upgrades{cfg}.isValid(checkTime,
                                       toUpgradeType(makeBaseFeeUpgrade(99)),
                                       ledgerUpgradeType));
        REQUIRE(!Upgrades{cfg}.isValid(checkTime,
                                       toUpgradeType(makeBaseFeeUpgrade(101)),
                                       ledgerUpgradeType));
    }

    SECTION("tx count")
    {
        REQUIRE(Upgrades{cfg}.isValid(checkTime,
                                      toUpgradeType(makeTxCountUpgrade(50)),
                                      ledgerUpgradeType));
        REQUIRE(!Upgrades{cfg}.isValid(checkTime,
                                       toUpgradeType(makeTxCountUpgrade(49)),
                                       ledgerUpgradeType));
        REQUIRE(!Upgrades{cfg}.isValid(checkTime,
                                       toUpgradeType(makeTxCountUpgrade(51)),
                                       ledgerUpgradeType));
    }
}

TEST_CASE("validate upgrades when no time set for upgrade", "[upgrades]")
{
    testValidateUpgrades({}, true);
}

TEST_CASE("validate upgrades just before upgrade time", "[upgrades]")
{
    testValidateUpgrades(genesis(0, 1), false);
}

TEST_CASE("validate upgrades at upgrade time", "[upgrades]")
{
    testValidateUpgrades(genesis(0, 0), true);
}

TEST_CASE("simulate upgrades", "[herder][upgrades]")
{
    // no upgrade is done
    auto noUpgrade =
        LedgerUpgradeableData{LedgerManager::GENESIS_LEDGER_VERSION,
                              LedgerManager::GENESIS_LEDGER_BASE_FEE,
                              LedgerManager::GENESIS_LEDGER_MAX_TX_SIZE};
    // only fee and tx size upgrades are allowed as they are in limit
    // but ledger version is not upgraded
    auto partialUpgrade =
        LedgerUpgradeableData{LedgerManager::GENESIS_LEDGER_VERSION,
                              LedgerManager::GENESIS_LEDGER_BASE_FEE + 1,
                              LedgerManager::GENESIS_LEDGER_MAX_TX_SIZE + 1};
    // all values are upgraded by small values
    auto upgrade =
        LedgerUpgradeableData{LedgerManager::GENESIS_LEDGER_VERSION + 1,
                              LedgerManager::GENESIS_LEDGER_BASE_FEE + 1,
                              LedgerManager::GENESIS_LEDGER_MAX_TX_SIZE + 1};
    // all values are upgraded by big values
    auto bigUpgrade =
        LedgerUpgradeableData{LedgerManager::GENESIS_LEDGER_VERSION + 1,
                              LedgerManager::GENESIS_LEDGER_BASE_FEE * 4,
                              LedgerManager::GENESIS_LEDGER_MAX_TX_SIZE * 4};

    SECTION("0 of 3 vote - dont upgrade")
    {
        auto nodes = std::vector<LedgerUpgradeNode>{
            {noUpgrade, {}}, {noUpgrade, {}}, {noUpgrade, {}}};
        auto checks = std::vector<LedgerUpgradeCheck>{
            {genesis(0, 30), {noUpgrade, noUpgrade, noUpgrade}}};
        simulateUpgrade(nodes, checks);
    }

    SECTION("1 of 3 vote, big upgrade - dont upgrade")
    {
        auto nodes = std::vector<LedgerUpgradeNode>{
            {bigUpgrade, {}}, {noUpgrade, {}}, {noUpgrade, {}}};
        auto checks = std::vector<LedgerUpgradeCheck>{
            {genesis(0, 30), {noUpgrade, noUpgrade, noUpgrade}}};
        simulateUpgrade(nodes, checks);
    }

    SECTION("2 of 3 vote - 2 upgrade, 1 dont")
    {
        auto nodes = std::vector<LedgerUpgradeNode>{
            {upgrade, {}}, {upgrade, {}}, {noUpgrade, {}}};
        auto checks = std::vector<LedgerUpgradeCheck>{
            {genesis(0, 30), {upgrade, upgrade, noUpgrade}}};
        simulateUpgrade(nodes, checks);
    }

    SECTION("3 of 3 vote - upgrade")
    {
        auto nodes = std::vector<LedgerUpgradeNode>{{upgrade, genesis(1, 0)},
                                                    {upgrade, genesis(1, 0)},
                                                    {upgrade, genesis(1, 0)}};
        auto checks = std::vector<LedgerUpgradeCheck>{
            {genesis(0, 30), {partialUpgrade, partialUpgrade, partialUpgrade}},
            {genesis(1, 30), {upgrade, upgrade, upgrade}}};
        simulateUpgrade(nodes, checks);
    }

    SECTION("1 of 3 vote early - 3 upgrade late")
    {
        auto nodes = std::vector<LedgerUpgradeNode>{{upgrade, genesis(0, 30)},
                                                    {upgrade, genesis(1, 0)},
                                                    {upgrade, genesis(1, 0)}};
        auto checks = std::vector<LedgerUpgradeCheck>{
            {genesis(0, 45), {partialUpgrade, partialUpgrade, partialUpgrade}},
            {genesis(1, 15), {upgrade, upgrade, upgrade}}};
        simulateUpgrade(nodes, checks);
    }

    SECTION("2 of 3 vote early - 2 upgrade early, 1 upgrade partially")
    {
        auto nodes = std::vector<LedgerUpgradeNode>{{upgrade, genesis(0, 30)},
                                                    {upgrade, genesis(0, 30)},
                                                    {upgrade, genesis(1, 0)}};
        auto checks = std::vector<LedgerUpgradeCheck>{
            {genesis(0, 15), {partialUpgrade, partialUpgrade, partialUpgrade}},
            {genesis(0, 45), {upgrade, upgrade, partialUpgrade}}};
        simulateUpgrade(nodes, checks);
    }
}
