// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/Herder.h"
#include "herder/LedgerCloseData.h"
#include "herder/Upgrades.h"
#include "history/HistoryTestsUtils.h"
#include "lib/catch.hpp"
#include "simulation/Simulation.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/StatusManager.h"
#include "util/Timer.h"
#include "util/optional.h"
#include <xdrpp/marshal.h>

using namespace stellar;

struct LedgerUpgradeableData
{
    LedgerUpgradeableData()
    {
    }
    LedgerUpgradeableData(uint32_t v, uint32_t f, uint32_t txs, uint32_t r)
        : ledgerVersion(v), baseFee(f), maxTxSetSize(txs), baseReserve(r)
    {
    }
    uint32_t ledgerVersion{0};
    uint32_t baseFee{0};
    uint32_t maxTxSetSize{0};
    uint32_t baseReserve{0};
};

struct LedgerUpgradeNode
{
    LedgerUpgradeableData desiredUpgrades;
    VirtualClock::time_point preferredUpgradeDatetime;
};

struct LedgerUpgradeCheck
{
    VirtualClock::time_point time;
    std::vector<LedgerUpgradeableData> expected;
};

void
simulateUpgrade(std::vector<LedgerUpgradeNode> const& nodes,
                std::vector<LedgerUpgradeCheck> const& checks,
                bool checkUpgradeStatus = false)
{
    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    historytestutils::TmpDirHistoryConfigurator configurator{};
    auto simulation =
        std::make_shared<Simulation>(Simulation::OVER_LOOPBACK, networkID);
    simulation->setCurrentTime(genesis(0, 0));

    // configure nodes
    auto keys = std::vector<SecretKey>{};
    auto configs = std::vector<Config>{};
    for (size_t i = 0; i < nodes.size(); i++)
    {
        keys.push_back(
            SecretKey::fromSeed(sha256("NODE_SEED_" + std::to_string(i))));
        configs.push_back(simulation->newConfig());
        // disable upgrade from config
        configs.back().TESTING_UPGRADE_DATETIME = VirtualClock::time_point();
        configs.back().USE_CONFIG_FOR_GENESIS = false;
        // first node can write to history, all can read
        configurator.configure(configs.back(), i == 0);
    }

    // first two only depend on each other
    // this allows to test for v-blocking properties
    // on the 3rd node
    auto qSet = SCPQuorumSet{};
    qSet.threshold = 2;
    qSet.validators.push_back(keys[0].getPublicKey());
    qSet.validators.push_back(keys[1].getPublicKey());
    qSet.validators.push_back(keys[2].getPublicKey());

    auto setUpgrade = [](optional<uint32>& o, uint32 v) {
        o = make_optional<uint32>(v);
    };
    // create nodes
    for (size_t i = 0; i < nodes.size(); i++)
    {
        auto app = simulation->addNode(keys[i], qSet, &configs[i]);

        auto& upgradeTime = nodes[i].preferredUpgradeDatetime;

        if (upgradeTime.time_since_epoch().count() != 0)
        {
            auto& du = nodes[i].desiredUpgrades;
            Upgrades::UpgradeParameters upgrades;
            setUpgrade(upgrades.mBaseFee, du.baseFee);
            setUpgrade(upgrades.mBaseReserve, du.baseReserve);
            setUpgrade(upgrades.mMaxTxSize, du.maxTxSetSize);
            setUpgrade(upgrades.mProtocolVersion, du.ledgerVersion);
            upgrades.mUpgradeTime = upgradeTime;
            app->getHerder().setUpgrades(upgrades);
        }
    }

    HistoryManager::initializeHistoryArchive(
        *simulation->getNode(keys[0].getPublicKey()), "test");

    for (size_t i = 0; i < nodes.size(); i++)
    {
        for (size_t j = i + 1; j < nodes.size(); j++)
        {
            simulation->addPendingConnection(keys[i].getPublicKey(),
                                             keys[j].getPublicKey());
        }
    }

    simulation->startAllNodes();

    auto statesMatch = [&](std::vector<LedgerUpgradeableData> const& state) {
        for (size_t i = 0; i < nodes.size(); i++)
        {
            auto const& node = simulation->getNode(keys[i].getPublicKey());
            REQUIRE(node->getLedgerManager()
                        .getCurrentLedgerHeader()
                        .ledgerVersion == state[i].ledgerVersion);
            REQUIRE(node->getLedgerManager().getCurrentLedgerHeader().baseFee ==
                    state[i].baseFee);
            REQUIRE(node->getLedgerManager()
                        .getCurrentLedgerHeader()
                        .maxTxSetSize == state[i].maxTxSetSize);
            REQUIRE(
                node->getLedgerManager().getCurrentLedgerHeader().baseReserve ==
                state[i].baseReserve);
        }
    };

    for (auto const& result : checks)
    {
        simulation->crankUntil(result.time, false);
        statesMatch(result.expected);
    }

    auto allSynced = [&]() {
        return std::all_of(
            std::begin(keys), std::end(keys), [&](SecretKey const& key) {
                auto const& node = simulation->getNode(key.getPublicKey());
                return node->getLedgerManager().getState() ==
                       LedgerManager::LM_SYNCED_STATE;
            });
    };

    // all nodes are synced as there was no disagreement about upgrades
    REQUIRE(allSynced());

    if (checkUpgradeStatus)
    {
        // at least one node should show message thats it has some
        // pending upgrades
        REQUIRE(std::any_of(
            std::begin(keys), std::end(keys), [&](SecretKey const& key) {
                auto const& node = simulation->getNode(key.getPublicKey());
                return !node->getStatusManager()
                            .getStatusMessage(StatusCategory::REQUIRES_UPGRADES)
                            .empty();
            }));
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

LedgerUpgrade
makeBaseReserveUpgrade(int baseReserve)
{
    auto result = LedgerUpgrade{LEDGER_UPGRADE_BASE_RESERVE};
    result.newBaseReserve() = baseReserve;
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
                 bool shouldListAny)
{
    auto cfg = getTestConfig();
    cfg.LEDGER_PROTOCOL_VERSION = 10;
    cfg.TESTING_UPGRADE_DESIRED_FEE = 100;
    cfg.TESTING_UPGRADE_MAX_TX_PER_LEDGER = 50;
    cfg.TESTING_UPGRADE_RESERVE = 100000000;
    cfg.TESTING_UPGRADE_DATETIME = preferredUpgradeDatetime;

    auto header = LedgerHeader{};
    header.ledgerVersion = cfg.LEDGER_PROTOCOL_VERSION;
    header.baseFee = cfg.TESTING_UPGRADE_DESIRED_FEE;
    header.baseReserve = cfg.TESTING_UPGRADE_RESERVE;
    header.maxTxSetSize = cfg.TESTING_UPGRADE_MAX_TX_PER_LEDGER;
    header.scpValue.closeTime = VirtualClock::to_time_t(genesis(0, 0));

    auto protocolVersionUpgrade =
        makeProtocolVersionUpgrade(cfg.LEDGER_PROTOCOL_VERSION);
    auto baseFeeUpgrade = makeBaseFeeUpgrade(cfg.TESTING_UPGRADE_DESIRED_FEE);
    auto txCountUpgrade =
        makeTxCountUpgrade(cfg.TESTING_UPGRADE_MAX_TX_PER_LEDGER);
    auto baseReserveUpgrade =
        makeBaseReserveUpgrade(cfg.TESTING_UPGRADE_RESERVE);

    SECTION("protocol version upgrade needed")
    {
        header.ledgerVersion--;
        auto upgrades = Upgrades{cfg}.createUpgradesFor(header);
        auto expected = shouldListAny
                            ? std::vector<LedgerUpgrade>{protocolVersionUpgrade}
                            : std::vector<LedgerUpgrade>{};
        REQUIRE(upgrades == expected);
    }

    SECTION("base fee upgrade needed")
    {
        header.baseFee /= 2;
        auto upgrades = Upgrades{cfg}.createUpgradesFor(header);
        auto expected = shouldListAny
                            ? std::vector<LedgerUpgrade>{baseFeeUpgrade}
                            : std::vector<LedgerUpgrade>{};
        REQUIRE(upgrades == expected);
    }

    SECTION("tx count upgrade needed")
    {
        header.maxTxSetSize /= 2;
        auto upgrades = Upgrades{cfg}.createUpgradesFor(header);
        auto expected = shouldListAny
                            ? std::vector<LedgerUpgrade>{txCountUpgrade}
                            : std::vector<LedgerUpgrade>{};
        REQUIRE(upgrades == expected);
    }

    SECTION("base reserve upgrade needed")
    {
        header.baseReserve /= 2;
        auto upgrades = Upgrades{cfg}.createUpgradesFor(header);
        auto expected = shouldListAny
                            ? std::vector<LedgerUpgrade>{baseReserveUpgrade}
                            : std::vector<LedgerUpgrade>{};
        REQUIRE(upgrades == expected);
    }

    SECTION("all upgrades needed")
    {
        header.ledgerVersion--;
        header.baseFee /= 2;
        header.maxTxSetSize /= 2;
        header.baseReserve /= 2;
        auto upgrades = Upgrades{cfg}.createUpgradesFor(header);
        auto expected =
            shouldListAny
                ? std::vector<LedgerUpgrade>{protocolVersionUpgrade,
                                             baseFeeUpgrade, txCountUpgrade,
                                             baseReserveUpgrade}
                : std::vector<LedgerUpgrade>{};
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
                     bool canBeValid)
{
    auto cfg = getTestConfig();
    cfg.LEDGER_PROTOCOL_VERSION = 10;
    cfg.TESTING_UPGRADE_DESIRED_FEE = 100;
    cfg.TESTING_UPGRADE_MAX_TX_PER_LEDGER = 50;
    cfg.TESTING_UPGRADE_RESERVE = 100000000;
    cfg.TESTING_UPGRADE_DATETIME = preferredUpgradeDatetime;

    auto checkTime = VirtualClock::to_time_t(genesis(0, 0));
    auto ledgerUpgradeType = LedgerUpgradeType{};

    auto checkWith = [&](bool nomination) {
        SECTION("invalid upgrade data")
        {
            REQUIRE(!Upgrades{cfg}.isValid(checkTime, UpgradeType{},
                                           ledgerUpgradeType, nomination, cfg));
        }

        SECTION("version")
        {
            if (nomination)
            {
                REQUIRE(canBeValid ==
                        Upgrades{cfg}.isValid(
                            checkTime,
                            toUpgradeType(makeProtocolVersionUpgrade(10)),
                            ledgerUpgradeType, nomination, cfg));
            }
            else
            {
                REQUIRE(Upgrades{cfg}.isValid(
                    checkTime, toUpgradeType(makeProtocolVersionUpgrade(10)),
                    ledgerUpgradeType, nomination, cfg));
            }
            REQUIRE(!Upgrades{cfg}.isValid(
                checkTime, toUpgradeType(makeProtocolVersionUpgrade(9)),
                ledgerUpgradeType, nomination, cfg));
            REQUIRE(!Upgrades{cfg}.isValid(
                checkTime, toUpgradeType(makeProtocolVersionUpgrade(11)),
                ledgerUpgradeType, nomination, cfg));
        }

        SECTION("base fee")
        {
            if (nomination)
            {
                REQUIRE(canBeValid ==
                        Upgrades{cfg}.isValid(
                            checkTime, toUpgradeType(makeBaseFeeUpgrade(100)),
                            ledgerUpgradeType, nomination, cfg));
                REQUIRE(!Upgrades{cfg}.isValid(
                    checkTime, toUpgradeType(makeBaseFeeUpgrade(99)),
                    ledgerUpgradeType, nomination, cfg));
                REQUIRE(!Upgrades{cfg}.isValid(
                    checkTime, toUpgradeType(makeBaseFeeUpgrade(101)),
                    ledgerUpgradeType, nomination, cfg));
            }
            else
            {
                REQUIRE(Upgrades{cfg}.isValid(
                    checkTime, toUpgradeType(makeBaseFeeUpgrade(100)),
                    ledgerUpgradeType, nomination, cfg));
                REQUIRE(Upgrades{cfg}.isValid(
                    checkTime, toUpgradeType(makeBaseFeeUpgrade(99)),
                    ledgerUpgradeType, nomination, cfg));
                REQUIRE(Upgrades{cfg}.isValid(
                    checkTime, toUpgradeType(makeBaseFeeUpgrade(101)),
                    ledgerUpgradeType, nomination, cfg));
            }
            REQUIRE(!Upgrades{cfg}.isValid(checkTime,
                                           toUpgradeType(makeBaseFeeUpgrade(0)),
                                           ledgerUpgradeType, nomination, cfg));
        }

        SECTION("tx count")
        {
            if (nomination)
            {
                REQUIRE(canBeValid == Upgrades{cfg}.isValid(
                                          checkTime,
                                          toUpgradeType(makeTxCountUpgrade(50)),
                                          ledgerUpgradeType, nomination, cfg));
                REQUIRE(!Upgrades{cfg}.isValid(
                    checkTime, toUpgradeType(makeTxCountUpgrade(49)),
                    ledgerUpgradeType, nomination, cfg));
                REQUIRE(!Upgrades{cfg}.isValid(
                    checkTime, toUpgradeType(makeTxCountUpgrade(51)),
                    ledgerUpgradeType, nomination, cfg));
            }
            else
            {
                REQUIRE(Upgrades{cfg}.isValid(
                    checkTime, toUpgradeType(makeTxCountUpgrade(50)),
                    ledgerUpgradeType, nomination, cfg));
                REQUIRE(Upgrades{cfg}.isValid(
                    checkTime, toUpgradeType(makeTxCountUpgrade(49)),
                    ledgerUpgradeType, nomination, cfg));
                REQUIRE(Upgrades{cfg}.isValid(
                    checkTime, toUpgradeType(makeTxCountUpgrade(51)),
                    ledgerUpgradeType, nomination, cfg));
            }
            REQUIRE(!Upgrades{cfg}.isValid(checkTime,
                                           toUpgradeType(makeTxCountUpgrade(0)),
                                           ledgerUpgradeType, nomination, cfg));
        }

        SECTION("reserve")
        {
            if (nomination)
            {
                REQUIRE(canBeValid ==
                        Upgrades{cfg}.isValid(
                            checkTime,
                            toUpgradeType(makeBaseReserveUpgrade(100000000)),
                            ledgerUpgradeType, nomination, cfg));
                REQUIRE(!Upgrades{cfg}.isValid(
                    checkTime, toUpgradeType(makeBaseReserveUpgrade(99999999)),
                    ledgerUpgradeType, nomination, cfg));
                REQUIRE(!Upgrades{cfg}.isValid(
                    checkTime, toUpgradeType(makeBaseReserveUpgrade(100000001)),
                    ledgerUpgradeType, nomination, cfg));
            }
            else
            {
                REQUIRE(Upgrades{cfg}.isValid(
                    checkTime, toUpgradeType(makeBaseReserveUpgrade(100000000)),
                    ledgerUpgradeType, nomination, cfg));
                REQUIRE(Upgrades{cfg}.isValid(
                    checkTime, toUpgradeType(makeBaseReserveUpgrade(99999999)),
                    ledgerUpgradeType, nomination, cfg));
                REQUIRE(Upgrades{cfg}.isValid(
                    checkTime, toUpgradeType(makeBaseReserveUpgrade(100000001)),
                    ledgerUpgradeType, nomination, cfg));
            }
            REQUIRE(!Upgrades{cfg}.isValid(
                checkTime, toUpgradeType(makeBaseReserveUpgrade(0)),
                ledgerUpgradeType, nomination, cfg));
        }
    };
    checkWith(true);
    checkWith(false);
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

TEST_CASE("Ledger Manager applies upgrades properly", "[upgrades]")
{
    VirtualClock clock;
    auto cfg = getTestConfig(0);
    cfg.USE_CONFIG_FOR_GENESIS = false;
    auto app = Application::create(clock, cfg);
    app->start();

    auto const& lcl = app->getLedgerManager().getLastClosedLedgerHeader();
    auto const& lastHash = lcl.hash;
    auto txSet = std::make_shared<TxSetFrame>(lastHash);

    REQUIRE(lcl.header.ledgerVersion == LedgerManager::GENESIS_LEDGER_VERSION);
    REQUIRE(lcl.header.baseFee == LedgerManager::GENESIS_LEDGER_BASE_FEE);
    REQUIRE(lcl.header.maxTxSetSize ==
            LedgerManager::GENESIS_LEDGER_MAX_TX_SIZE);
    REQUIRE(lcl.header.baseReserve ==
            LedgerManager::GENESIS_LEDGER_BASE_RESERVE);

    auto executeUpgrades = [&](xdr::xvector<UpgradeType, 6> const& upgrades) {
        StellarValue sv{txSet->getContentsHash(), 2, upgrades, 0};
        LedgerCloseData ledgerData(lcl.header.ledgerSeq + 1, txSet, sv);
        app->getLedgerManager().closeLedger(ledgerData);
        return app->getLedgerManager().getLastClosedLedgerHeader();
    };
    auto executeUpgrade = [&](LedgerUpgrade const& upgrade) {
        auto upgrades = xdr::xvector<UpgradeType, 6>{};
        upgrades.push_back(toUpgradeType(upgrade));
        return executeUpgrades(upgrades);
    };

    SECTION("ledger version")
    {
        REQUIRE(executeUpgrade(
                    makeProtocolVersionUpgrade(cfg.LEDGER_PROTOCOL_VERSION))
                    .header.ledgerVersion == cfg.LEDGER_PROTOCOL_VERSION);
    }

    SECTION("base fee")
    {
        REQUIRE(executeUpgrade(makeBaseFeeUpgrade(1000)).header.baseFee ==
                1000);
    }

    SECTION("max tx")
    {
        REQUIRE(executeUpgrade(makeTxCountUpgrade(1300)).header.maxTxSetSize ==
                1300);
    }

    SECTION("base reserve")
    {
        REQUIRE(
            executeUpgrade(makeBaseReserveUpgrade(1000)).header.baseReserve ==
            1000);
    }

    SECTION("all")
    {
        auto header =
            executeUpgrades({toUpgradeType(makeProtocolVersionUpgrade(
                                 cfg.LEDGER_PROTOCOL_VERSION)),
                             toUpgradeType(makeBaseFeeUpgrade(1000)),
                             toUpgradeType(makeTxCountUpgrade(1300)),
                             toUpgradeType(makeBaseReserveUpgrade(1000))})
                .header;
        REQUIRE(header.ledgerVersion == cfg.LEDGER_PROTOCOL_VERSION);
        REQUIRE(header.baseFee == 1000);
        REQUIRE(header.maxTxSetSize == 1300);
        REQUIRE(header.baseReserve == 1000);
    }
}

TEST_CASE("simulate upgrades", "[herder][upgrades]")
{
    // no upgrade is done
    auto noUpgrade =
        LedgerUpgradeableData(LedgerManager::GENESIS_LEDGER_VERSION,
                              LedgerManager::GENESIS_LEDGER_BASE_FEE,
                              LedgerManager::GENESIS_LEDGER_MAX_TX_SIZE,
                              LedgerManager::GENESIS_LEDGER_BASE_RESERVE);
    // all values are upgraded
    auto upgrade =
        LedgerUpgradeableData(Config::CURRENT_LEDGER_PROTOCOL_VERSION,
                              LedgerManager::GENESIS_LEDGER_BASE_FEE + 1,
                              LedgerManager::GENESIS_LEDGER_MAX_TX_SIZE + 1,
                              LedgerManager::GENESIS_LEDGER_BASE_RESERVE + 1);

    SECTION("0 of 3 vote - dont upgrade")
    {
        auto nodes = std::vector<LedgerUpgradeNode>{{}, {}, {}};
        auto checks = std::vector<LedgerUpgradeCheck>{
            {genesis(0, 10), {noUpgrade, noUpgrade, noUpgrade}}};
        simulateUpgrade(nodes, checks);
    }

    SECTION("1 of 3 vote, dont upgrade")
    {
        auto nodes =
            std::vector<LedgerUpgradeNode>{{upgrade, genesis(0, 0)}, {}, {}};
        auto checks = std::vector<LedgerUpgradeCheck>{
            {genesis(0, 10), {noUpgrade, noUpgrade, noUpgrade}}};
        simulateUpgrade(nodes, checks, true);
    }

    SECTION("2 of 3 vote (v-blocking) - 3 upgrade")
    {
        auto nodes = std::vector<LedgerUpgradeNode>{
            {upgrade, genesis(0, 0)}, {upgrade, genesis(0, 0)}, {}};
        auto checks = std::vector<LedgerUpgradeCheck>{
            {genesis(0, 10), {upgrade, upgrade, upgrade}}};
        simulateUpgrade(nodes, checks);
    }

    SECTION("3 of 3 vote - upgrade")
    {
        auto nodes = std::vector<LedgerUpgradeNode>{{upgrade, genesis(0, 15)},
                                                    {upgrade, genesis(0, 15)},
                                                    {upgrade, genesis(0, 15)}};
        auto checks = std::vector<LedgerUpgradeCheck>{
            {genesis(0, 10), {noUpgrade, noUpgrade, noUpgrade}},
            {genesis(0, 21), {upgrade, upgrade, upgrade}}};
        simulateUpgrade(nodes, checks);
    }

    SECTION("3 votes for bogus fee - all 3 upgrade but ignore bad fee")
    {
        auto upgradeBadFee = upgrade;
        upgradeBadFee.baseFee = 0;
        auto expectedResult = upgradeBadFee;
        expectedResult.baseFee = LedgerManager::GENESIS_LEDGER_BASE_FEE;
        auto nodes =
            std::vector<LedgerUpgradeNode>{{upgradeBadFee, genesis(0, 0)},
                                           {upgradeBadFee, genesis(0, 0)},
                                           {upgradeBadFee, genesis(0, 0)}};
        auto checks = std::vector<LedgerUpgradeCheck>{
            {genesis(0, 10), {expectedResult, expectedResult, expectedResult}}};
        simulateUpgrade(nodes, checks, true);
    }

    SECTION("1 of 3 vote early - 2 upgrade late")
    {
        auto nodes = std::vector<LedgerUpgradeNode>{{upgrade, genesis(0, 10)},
                                                    {upgrade, genesis(0, 30)},
                                                    {upgrade, genesis(0, 30)}};
        auto checks = std::vector<LedgerUpgradeCheck>{
            {genesis(0, 20), {noUpgrade, noUpgrade, noUpgrade}},
            {genesis(0, 36), {upgrade, upgrade, upgrade}}};
        simulateUpgrade(nodes, checks);
    }

    SECTION("2 of 3 vote early (v-blocking) - 3 upgrade anyways")
    {
        auto nodes = std::vector<LedgerUpgradeNode>{{upgrade, genesis(0, 10)},
                                                    {upgrade, genesis(0, 10)},
                                                    {upgrade, genesis(0, 30)}};
        auto checks = std::vector<LedgerUpgradeCheck>{
            {genesis(0, 9), {noUpgrade, noUpgrade, noUpgrade}},
            {genesis(0, 20), {upgrade, upgrade, upgrade}}};
        simulateUpgrade(nodes, checks);
    }
}
