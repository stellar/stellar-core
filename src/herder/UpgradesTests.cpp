// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/Upgrades.h"
#include "herder/Herder.h"
#include "herder/LedgerCloseData.h"
#include "lib/catch.hpp"
#include "simulation/Simulation.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Timer.h"
#include "util/optional.h"
#include <xdrpp/marshal.h>

using namespace stellar;

struct LedgerUpgradeQuorum
{
    std::vector<int> quorumIndexes;
    int quorumTheshold;
};

struct LedgerUpgradeNode
{
    int ledgerProtocolVersion;
    VirtualClock::time_point preferredUpgradeDatetime;
    LedgerUpgradeQuorum quorum;
};

struct LedgerUpgradeCheck
{
    VirtualClock::time_point time;
    std::vector<uint32_t> expectedLedgerProtocolVersions;
};

VirtualClock::time_point
at(int minute, int second)
{
    return VirtualClock::tmToPoint(
        getTestDateTime(1, 7, 2014, 0, minute, second));
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
            nodes[i].preferredUpgradeDatetime;
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
        simulation->crankUntil(result.time, false);

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

LedgerUpgrade
makeProtocolVersionUpgrade(int version)
{
    auto result = LedgerUpgrade{};
    result.type(LEDGER_UPGRADE_VERSION);
    result.newLedgerVersion() = version;
    return result;
}

LedgerUpgrade
makeBaseFeeUpgrade(int baseFee)
{
    auto result = LedgerUpgrade{};
    result.type(LEDGER_UPGRADE_BASE_FEE);
    result.newBaseFee() = baseFee;
    return result;
}

LedgerUpgrade
makeTxCountUpgrade(int txCount)
{
    auto result = LedgerUpgrade{};
    result.type(LEDGER_UPGRADE_MAX_TX_SET_SIZE);
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

TEST_CASE("list upgrades when no time set for upgrade", "[upgrades]")
{
    auto cfg = getTestConfig();
    cfg.LEDGER_PROTOCOL_VERSION = 10;
    cfg.DESIRED_BASE_FEE = 100;
    cfg.DESIRED_MAX_TX_PER_LEDGER = 50;

    auto header = LedgerHeader{};
    header.ledgerVersion = cfg.LEDGER_PROTOCOL_VERSION;
    header.baseFee = cfg.DESIRED_BASE_FEE;
    header.maxTxSetSize = cfg.DESIRED_MAX_TX_PER_LEDGER;
    header.scpValue.closeTime = VirtualClock::to_time_t(at(0, 0));

    auto protocolVersionUpgrade =
        makeProtocolVersionUpgrade(cfg.LEDGER_PROTOCOL_VERSION);
    auto baseFeeUpgrade = makeBaseFeeUpgrade(cfg.DESIRED_BASE_FEE);
    auto txCountUpgrade = makeTxCountUpgrade(cfg.DESIRED_MAX_TX_PER_LEDGER);

    SECTION("no upgrade needed")
    {
        auto upgrades = Upgrades{cfg}.upgradesFor(header);
        auto expected = std::vector<LedgerUpgrade>{};
        REQUIRE(upgrades == expected);
    }

    SECTION("protocol version upgrade needed")
    {
        header.ledgerVersion--;
        auto upgrades = Upgrades{cfg}.upgradesFor(header);
        auto expected = std::vector<LedgerUpgrade>{protocolVersionUpgrade};
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
        auto expected = std::vector<LedgerUpgrade>{
            protocolVersionUpgrade, baseFeeUpgrade, txCountUpgrade};
        REQUIRE(upgrades == expected);
    }
}

TEST_CASE("list upgrades just before upgrade time", "[upgrades]")
{
    auto cfg = getTestConfig();
    cfg.LEDGER_PROTOCOL_VERSION = 10;
    cfg.DESIRED_BASE_FEE = 100;
    cfg.DESIRED_MAX_TX_PER_LEDGER = 50;
    cfg.PREFERRED_UPGRADE_DATETIME = at(0, 1);

    auto header = LedgerHeader{};
    header.ledgerVersion = cfg.LEDGER_PROTOCOL_VERSION;
    header.baseFee = cfg.DESIRED_BASE_FEE;
    header.maxTxSetSize = cfg.DESIRED_MAX_TX_PER_LEDGER;
    header.scpValue.closeTime = VirtualClock::to_time_t(at(0, 0));

    auto baseFeeUpgrade = makeBaseFeeUpgrade(cfg.DESIRED_BASE_FEE);
    auto txCountUpgrade = makeTxCountUpgrade(cfg.DESIRED_MAX_TX_PER_LEDGER);

    SECTION("protocol version upgrade needed")
    {
        header.ledgerVersion--;
        auto upgrades = Upgrades{cfg}.upgradesFor(header);
        auto expected = std::vector<LedgerUpgrade>{};
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
            std::vector<LedgerUpgrade>{baseFeeUpgrade, txCountUpgrade};
        REQUIRE(upgrades == expected);
    }
}

TEST_CASE("list upgrades at upgrade time", "[upgrades]")
{
    auto cfg = getTestConfig();
    cfg.LEDGER_PROTOCOL_VERSION = 10;
    cfg.DESIRED_BASE_FEE = 100;
    cfg.DESIRED_MAX_TX_PER_LEDGER = 50;
    cfg.PREFERRED_UPGRADE_DATETIME = at(0, 0);

    auto header = LedgerHeader{};
    header.ledgerVersion = cfg.LEDGER_PROTOCOL_VERSION;
    header.baseFee = cfg.DESIRED_BASE_FEE;
    header.maxTxSetSize = cfg.DESIRED_MAX_TX_PER_LEDGER;
    header.scpValue.closeTime = VirtualClock::to_time_t(at(0, 0));

    auto protocolVersionUpgrade =
        makeProtocolVersionUpgrade(cfg.LEDGER_PROTOCOL_VERSION);
    auto baseFeeUpgrade = makeBaseFeeUpgrade(cfg.DESIRED_BASE_FEE);
    auto txCountUpgrade = makeTxCountUpgrade(cfg.DESIRED_MAX_TX_PER_LEDGER);

    SECTION("protocol version upgrade needed")
    {
        header.ledgerVersion--;
        auto upgrades = Upgrades{cfg}.upgradesFor(header);
        auto expected = std::vector<LedgerUpgrade>{protocolVersionUpgrade};
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
        auto expected = std::vector<LedgerUpgrade>{
            protocolVersionUpgrade, baseFeeUpgrade, txCountUpgrade};
        REQUIRE(upgrades == expected);
    }
}

TEST_CASE("validate upgrades when no time set for upgrade", "[upgrades]")
{
    auto cfg = getTestConfig();
    cfg.LEDGER_PROTOCOL_VERSION = 10;
    cfg.DESIRED_BASE_FEE = 100;
    cfg.DESIRED_MAX_TX_PER_LEDGER = 50;

    auto checkTime = VirtualClock::to_time_t(at(0, 0));
    auto ledgerUpgradeType = LedgerUpgradeType{};

    SECTION("invalid upgrade data")
    {
        REQUIRE(!Upgrades{cfg}.isValid(checkTime, UpgradeType{},
                                       ledgerUpgradeType, false));
        REQUIRE(!Upgrades{cfg}.isValid(checkTime, UpgradeType{},
                                       ledgerUpgradeType, true));
    }

    SECTION("valid version")
    {
        REQUIRE(Upgrades{cfg}.isValid(
            checkTime, toUpgradeType(makeProtocolVersionUpgrade(10)),
            ledgerUpgradeType, false));
        REQUIRE(Upgrades{cfg}.isValid(
            checkTime, toUpgradeType(makeProtocolVersionUpgrade(10)),
            ledgerUpgradeType, true));
    }

    SECTION("invalid version")
    {
        REQUIRE(!Upgrades{cfg}.isValid(
            checkTime, toUpgradeType(makeProtocolVersionUpgrade(9)),
            ledgerUpgradeType, false));
        REQUIRE(!Upgrades{cfg}.isValid(
            checkTime, toUpgradeType(makeProtocolVersionUpgrade(9)),
            ledgerUpgradeType, true));
        REQUIRE(!Upgrades{cfg}.isValid(
            checkTime, toUpgradeType(makeProtocolVersionUpgrade(11)),
            ledgerUpgradeType, false));
        REQUIRE(!Upgrades{cfg}.isValid(
            checkTime, toUpgradeType(makeProtocolVersionUpgrade(11)),
            ledgerUpgradeType, true));
    }

    SECTION("valid fee")
    {
        REQUIRE(Upgrades{cfg}.isValid(checkTime,
                                      toUpgradeType(makeBaseFeeUpgrade(50)),
                                      ledgerUpgradeType, false));
        REQUIRE(Upgrades{cfg}.isValid(checkTime,
                                      toUpgradeType(makeBaseFeeUpgrade(50)),
                                      ledgerUpgradeType, true));
        REQUIRE(Upgrades{cfg}.isValid(checkTime,
                                      toUpgradeType(makeBaseFeeUpgrade(200)),
                                      ledgerUpgradeType, false));
        REQUIRE(Upgrades{cfg}.isValid(checkTime,
                                      toUpgradeType(makeBaseFeeUpgrade(200)),
                                      ledgerUpgradeType, true));
    }

    SECTION("too small fee")
    {
        REQUIRE(!Upgrades{cfg}.isValid(checkTime,
                                       toUpgradeType(makeBaseFeeUpgrade(49)),
                                       ledgerUpgradeType, false));
        REQUIRE(!Upgrades{cfg}.isValid(checkTime,
                                       toUpgradeType(makeBaseFeeUpgrade(49)),
                                       ledgerUpgradeType, true));
    }

    SECTION("too big fee")
    {
        REQUIRE(!Upgrades{cfg}.isValid(checkTime,
                                       toUpgradeType(makeBaseFeeUpgrade(201)),
                                       ledgerUpgradeType, false));
        REQUIRE(!Upgrades{cfg}.isValid(checkTime,
                                       toUpgradeType(makeBaseFeeUpgrade(201)),
                                       ledgerUpgradeType, true));
    }

    SECTION("valid tx count")
    {
        REQUIRE(Upgrades{cfg}.isValid(checkTime,
                                      toUpgradeType(makeTxCountUpgrade(35)),
                                      ledgerUpgradeType, false));
        REQUIRE(Upgrades{cfg}.isValid(checkTime,
                                      toUpgradeType(makeTxCountUpgrade(35)),
                                      ledgerUpgradeType, true));
        REQUIRE(Upgrades{cfg}.isValid(checkTime,
                                      toUpgradeType(makeTxCountUpgrade(65)),
                                      ledgerUpgradeType, false));
        REQUIRE(Upgrades{cfg}.isValid(checkTime,
                                      toUpgradeType(makeTxCountUpgrade(65)),
                                      ledgerUpgradeType, true));
    }

    SECTION("too small tx count")
    {
        REQUIRE(!Upgrades{cfg}.isValid(checkTime,
                                       toUpgradeType(makeTxCountUpgrade(34)),
                                       ledgerUpgradeType, false));
        REQUIRE(!Upgrades{cfg}.isValid(checkTime,
                                       toUpgradeType(makeTxCountUpgrade(34)),
                                       ledgerUpgradeType, true));
    }

    SECTION("too big tx count")
    {
        REQUIRE(!Upgrades{cfg}.isValid(checkTime,
                                       toUpgradeType(makeTxCountUpgrade(66)),
                                       ledgerUpgradeType, false));
        REQUIRE(!Upgrades{cfg}.isValid(checkTime,
                                       toUpgradeType(makeTxCountUpgrade(66)),
                                       ledgerUpgradeType, true));
    }
}

TEST_CASE("validate upgrades just before upgrade time", "[upgrades]")
{
    auto cfg = getTestConfig();
    cfg.LEDGER_PROTOCOL_VERSION = 10;
    cfg.DESIRED_BASE_FEE = 100;
    cfg.DESIRED_MAX_TX_PER_LEDGER = 50;
    cfg.PREFERRED_UPGRADE_DATETIME = at(0, 1);

    auto checkTime = VirtualClock::to_time_t(at(0, 0));
    auto ledgerUpgradeType = LedgerUpgradeType{};

    SECTION("invalid upgrade data")
    {
        REQUIRE(!Upgrades{cfg}.isValid(checkTime, UpgradeType{},
                                       ledgerUpgradeType, false));
        REQUIRE(!Upgrades{cfg}.isValid(checkTime, UpgradeType{},
                                       ledgerUpgradeType, true));
    }

    SECTION("valid version")
    {
        REQUIRE(!Upgrades{cfg}.isValid(
            checkTime, toUpgradeType(makeProtocolVersionUpgrade(10)),
            ledgerUpgradeType, false));
        REQUIRE(Upgrades{cfg}.isValid(
            checkTime, toUpgradeType(makeProtocolVersionUpgrade(10)),
            ledgerUpgradeType, true));
    }

    SECTION("invalid version")
    {
        REQUIRE(!Upgrades{cfg}.isValid(
            checkTime, toUpgradeType(makeProtocolVersionUpgrade(9)),
            ledgerUpgradeType, false));
        REQUIRE(!Upgrades{cfg}.isValid(
            checkTime, toUpgradeType(makeProtocolVersionUpgrade(9)),
            ledgerUpgradeType, true));
        REQUIRE(!Upgrades{cfg}.isValid(
            checkTime, toUpgradeType(makeProtocolVersionUpgrade(11)),
            ledgerUpgradeType, false));
        REQUIRE(!Upgrades{cfg}.isValid(
            checkTime, toUpgradeType(makeProtocolVersionUpgrade(11)),
            ledgerUpgradeType, true));
    }

    SECTION("valid fee")
    {
        REQUIRE(Upgrades{cfg}.isValid(checkTime,
                                      toUpgradeType(makeBaseFeeUpgrade(50)),
                                      ledgerUpgradeType, false));
        REQUIRE(Upgrades{cfg}.isValid(checkTime,
                                      toUpgradeType(makeBaseFeeUpgrade(50)),
                                      ledgerUpgradeType, true));
        REQUIRE(Upgrades{cfg}.isValid(checkTime,
                                      toUpgradeType(makeBaseFeeUpgrade(200)),
                                      ledgerUpgradeType, false));
        REQUIRE(Upgrades{cfg}.isValid(checkTime,
                                      toUpgradeType(makeBaseFeeUpgrade(200)),
                                      ledgerUpgradeType, true));
    }

    SECTION("too small fee")
    {
        REQUIRE(!Upgrades{cfg}.isValid(checkTime,
                                       toUpgradeType(makeBaseFeeUpgrade(49)),
                                       ledgerUpgradeType, false));
        REQUIRE(!Upgrades{cfg}.isValid(checkTime,
                                       toUpgradeType(makeBaseFeeUpgrade(49)),
                                       ledgerUpgradeType, true));
    }

    SECTION("too big fee")
    {
        REQUIRE(!Upgrades{cfg}.isValid(checkTime,
                                       toUpgradeType(makeBaseFeeUpgrade(201)),
                                       ledgerUpgradeType, false));
        REQUIRE(!Upgrades{cfg}.isValid(checkTime,
                                       toUpgradeType(makeBaseFeeUpgrade(201)),
                                       ledgerUpgradeType, true));
    }

    SECTION("valid tx count")
    {
        REQUIRE(Upgrades{cfg}.isValid(checkTime,
                                      toUpgradeType(makeTxCountUpgrade(35)),
                                      ledgerUpgradeType, false));
        REQUIRE(Upgrades{cfg}.isValid(checkTime,
                                      toUpgradeType(makeTxCountUpgrade(35)),
                                      ledgerUpgradeType, true));
        REQUIRE(Upgrades{cfg}.isValid(checkTime,
                                      toUpgradeType(makeTxCountUpgrade(65)),
                                      ledgerUpgradeType, false));
        REQUIRE(Upgrades{cfg}.isValid(checkTime,
                                      toUpgradeType(makeTxCountUpgrade(65)),
                                      ledgerUpgradeType, true));
    }

    SECTION("too small tx count")
    {
        REQUIRE(!Upgrades{cfg}.isValid(checkTime,
                                       toUpgradeType(makeTxCountUpgrade(34)),
                                       ledgerUpgradeType, false));
        REQUIRE(!Upgrades{cfg}.isValid(checkTime,
                                       toUpgradeType(makeTxCountUpgrade(34)),
                                       ledgerUpgradeType, true));
    }

    SECTION("too big tx count")
    {
        REQUIRE(!Upgrades{cfg}.isValid(checkTime,
                                       toUpgradeType(makeTxCountUpgrade(66)),
                                       ledgerUpgradeType, false));
        REQUIRE(!Upgrades{cfg}.isValid(checkTime,
                                       toUpgradeType(makeTxCountUpgrade(66)),
                                       ledgerUpgradeType, true));
    }
}

TEST_CASE("validate upgrades at upgrade time", "[upgrades]")
{
    auto cfg = getTestConfig();
    cfg.LEDGER_PROTOCOL_VERSION = 10;
    cfg.DESIRED_BASE_FEE = 100;
    cfg.DESIRED_MAX_TX_PER_LEDGER = 50;
    cfg.PREFERRED_UPGRADE_DATETIME = at(0, 0);

    auto checkTime = VirtualClock::to_time_t(at(0, 0));
    auto ledgerUpgradeType = LedgerUpgradeType{};

    SECTION("invalid upgrade data")
    {
        REQUIRE(!Upgrades{cfg}.isValid(checkTime, UpgradeType{},
                                       ledgerUpgradeType, false));
        REQUIRE(!Upgrades{cfg}.isValid(checkTime, UpgradeType{},
                                       ledgerUpgradeType, true));
    }

    SECTION("valid version")
    {
        REQUIRE(Upgrades{cfg}.isValid(
            checkTime, toUpgradeType(makeProtocolVersionUpgrade(10)),
            ledgerUpgradeType, false));
        REQUIRE(Upgrades{cfg}.isValid(
            checkTime, toUpgradeType(makeProtocolVersionUpgrade(10)),
            ledgerUpgradeType, true));
    }

    SECTION("invalid version")
    {
        REQUIRE(!Upgrades{cfg}.isValid(
            checkTime, toUpgradeType(makeProtocolVersionUpgrade(9)),
            ledgerUpgradeType, false));
        REQUIRE(!Upgrades{cfg}.isValid(
            checkTime, toUpgradeType(makeProtocolVersionUpgrade(9)),
            ledgerUpgradeType, true));
        REQUIRE(!Upgrades{cfg}.isValid(
            checkTime, toUpgradeType(makeProtocolVersionUpgrade(11)),
            ledgerUpgradeType, false));
        REQUIRE(!Upgrades{cfg}.isValid(
            checkTime, toUpgradeType(makeProtocolVersionUpgrade(11)),
            ledgerUpgradeType, true));
    }

    SECTION("valid fee")
    {
        REQUIRE(Upgrades{cfg}.isValid(checkTime,
                                      toUpgradeType(makeBaseFeeUpgrade(50)),
                                      ledgerUpgradeType, false));
        REQUIRE(Upgrades{cfg}.isValid(checkTime,
                                      toUpgradeType(makeBaseFeeUpgrade(50)),
                                      ledgerUpgradeType, true));
        REQUIRE(Upgrades{cfg}.isValid(checkTime,
                                      toUpgradeType(makeBaseFeeUpgrade(200)),
                                      ledgerUpgradeType, false));
        REQUIRE(Upgrades{cfg}.isValid(checkTime,
                                      toUpgradeType(makeBaseFeeUpgrade(200)),
                                      ledgerUpgradeType, true));
    }

    SECTION("too small fee")
    {
        REQUIRE(!Upgrades{cfg}.isValid(checkTime,
                                       toUpgradeType(makeBaseFeeUpgrade(49)),
                                       ledgerUpgradeType, false));
        REQUIRE(!Upgrades{cfg}.isValid(checkTime,
                                       toUpgradeType(makeBaseFeeUpgrade(49)),
                                       ledgerUpgradeType, true));
    }

    SECTION("too big fee")
    {
        REQUIRE(!Upgrades{cfg}.isValid(checkTime,
                                       toUpgradeType(makeBaseFeeUpgrade(201)),
                                       ledgerUpgradeType, false));
        REQUIRE(!Upgrades{cfg}.isValid(checkTime,
                                       toUpgradeType(makeBaseFeeUpgrade(201)),
                                       ledgerUpgradeType, true));
    }

    SECTION("valid tx count")
    {
        REQUIRE(Upgrades{cfg}.isValid(checkTime,
                                      toUpgradeType(makeTxCountUpgrade(35)),
                                      ledgerUpgradeType, false));
        REQUIRE(Upgrades{cfg}.isValid(checkTime,
                                      toUpgradeType(makeTxCountUpgrade(35)),
                                      ledgerUpgradeType, true));
        REQUIRE(Upgrades{cfg}.isValid(checkTime,
                                      toUpgradeType(makeTxCountUpgrade(65)),
                                      ledgerUpgradeType, false));
        REQUIRE(Upgrades{cfg}.isValid(checkTime,
                                      toUpgradeType(makeTxCountUpgrade(65)),
                                      ledgerUpgradeType, true));
    }

    SECTION("too small tx count")
    {
        REQUIRE(!Upgrades{cfg}.isValid(checkTime,
                                       toUpgradeType(makeTxCountUpgrade(34)),
                                       ledgerUpgradeType, false));
        REQUIRE(!Upgrades{cfg}.isValid(checkTime,
                                       toUpgradeType(makeTxCountUpgrade(34)),
                                       ledgerUpgradeType, true));
    }

    SECTION("too big tx count")
    {
        REQUIRE(!Upgrades{cfg}.isValid(checkTime,
                                       toUpgradeType(makeTxCountUpgrade(66)),
                                       ledgerUpgradeType, false));
        REQUIRE(!Upgrades{cfg}.isValid(checkTime,
                                       toUpgradeType(makeTxCountUpgrade(66)),
                                       ledgerUpgradeType, true));
    }
}

TEST_CASE("0 nodes vote for upgrading ledger - keep old version",
          "[herder][upgrades]")
{
    auto quorum = LedgerUpgradeQuorum{{0, 1}, 2};
    auto nodes =
        std::vector<LedgerUpgradeNode>{{0, {}, quorum}, {0, {}, quorum}};
    auto checks = std::vector<LedgerUpgradeCheck>{{at(0, 30), {0, 0}}};
    simulateLedgerUpgrade(nodes, checks);
}

TEST_CASE("1 of 2 nodes vote for upgrading ledger - keep old version",
          "[herder][upgrades]")
{
    auto quorum = LedgerUpgradeQuorum{{0, 1}, 2};
    auto nodes =
        std::vector<LedgerUpgradeNode>{{0, {}, quorum}, {1, {}, quorum}};
    auto checks = std::vector<LedgerUpgradeCheck>{{at(0, 30), {0, 0}}};
    simulateLedgerUpgrade(nodes, checks);
}

TEST_CASE("2 of 2 nodes vote for upgrading ledger - upgrade",
          "[herder][upgrades]")
{
    auto quorum = LedgerUpgradeQuorum{{0, 1}, 2};
    auto nodes =
        std::vector<LedgerUpgradeNode>{{1, {}, quorum}, {1, {}, quorum}};
    auto checks = std::vector<LedgerUpgradeCheck>{{at(0, 30), {1, 1}}};
    simulateLedgerUpgrade(nodes, checks);
}

TEST_CASE("1 of 3 nodes vote for upgrading ledger - keep old version",
          "[herder][upgrades]")
{
    auto quorum = LedgerUpgradeQuorum{{0, 1, 2}, 2};
    auto nodes = std::vector<LedgerUpgradeNode>{
        {1, {}, quorum}, {0, {}, quorum}, {0, {}, quorum}};
    auto checks = std::vector<LedgerUpgradeCheck>{{at(0, 30), {0, 0, 0}}};
    simulateLedgerUpgrade(nodes, checks);
}

TEST_CASE("2 of 3 nodes vote for upgrading ledger - upgrade, one node desynced",
          "[herder][upgrades]")
{
    auto quorum = LedgerUpgradeQuorum{{0, 1, 2}, 2};
    auto nodes = std::vector<LedgerUpgradeNode>{
        {1, {}, quorum}, {1, {}, quorum}, {0, {}, quorum}};
    auto checks = std::vector<LedgerUpgradeCheck>{{at(0, 30), {1, 1, 0}}};
    simulateLedgerUpgrade(nodes, checks);
}

TEST_CASE("2 of 2 nodes vote for upgrade at some time - upgrade at this time",
          "[herder][upgrades]")
{
    auto quorum = LedgerUpgradeQuorum{{0, 1}, 2};
    auto nodes = std::vector<LedgerUpgradeNode>{{1, at(1, 0), quorum},
                                                {1, at(1, 0), quorum}};
    auto checks = std::vector<LedgerUpgradeCheck>{{at(0, 30), {0, 0}},
                                                  {at(1, 30), {1, 1}}};
    simulateLedgerUpgrade(nodes, checks);
}

TEST_CASE("2 of 2 nodes vote for upgrade at some time - upgrade at later time",
          "[herder][upgrades]")
{
    auto quorum = LedgerUpgradeQuorum{{0, 1}, 2};
    auto nodes = std::vector<LedgerUpgradeNode>{{1, at(0, 30), quorum},
                                                {1, at(1, 0), quorum}};
    auto checks = std::vector<LedgerUpgradeCheck>{{at(0, 45), {0, 0}},
                                                  {at(1, 15), {1, 1}}};
    simulateLedgerUpgrade(nodes, checks);
}

TEST_CASE("3 of 3 nodes vote for upgrade at some time; 2 on earlier time - "
          "upgrade at earlier time",
          "[herder][upgrades]")
{
    auto quorum = LedgerUpgradeQuorum{{0, 1, 2}, 2};
    auto nodes = std::vector<LedgerUpgradeNode>{
        {1, at(0, 30), quorum}, {1, at(0, 30), quorum}, {1, at(1, 0), quorum}};
    auto checks = std::vector<LedgerUpgradeCheck>{
        {at(0, 15), {0, 0, 0}}, {at(0, 45), {1, 1, 1}}, {at(1, 15), {1, 1, 1}}};
    simulateLedgerUpgrade(nodes, checks);
}

TEST_CASE("3 of 3 nodes vote for upgrade at some time; 1 on earlier time - "
          "upgrade at later time",
          "[herder][upgrades]")
{
    auto quorum = LedgerUpgradeQuorum{{0, 1, 2}, 2};
    auto nodes = std::vector<LedgerUpgradeNode>{
        {1, at(0, 30), quorum}, {1, at(1, 0), quorum}, {1, at(1, 0), quorum}};
    auto checks = std::vector<LedgerUpgradeCheck>{{at(0, 45), {0, 0, 0}},
                                                  {at(1, 15), {1, 1, 1}}};
    simulateLedgerUpgrade(nodes, checks);
}
