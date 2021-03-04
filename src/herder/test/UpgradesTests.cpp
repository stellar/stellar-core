// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketInputIterator.h"
#include "bucket/BucketTests.h"
#include "herder/Herder.h"
#include "herder/HerderImpl.h"
#include "herder/LedgerCloseData.h"
#include "herder/Upgrades.h"
#include "history/HistoryArchiveManager.h"
#include "history/test/HistoryTestsUtils.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "ledger/TrustLineWrapper.h"
#include "lib/catch.hpp"
#include "simulation/Simulation.h"
#include "test/TestMarket.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "transactions/SignatureUtils.h"
#include "transactions/SponsorshipUtils.h"
#include "transactions/TransactionUtils.h"
#include "util/StatusManager.h"
#include "util/Timer.h"
#include "util/optional.h"
#include <fmt/format.h>
#include <xdrpp/marshal.h>

using namespace stellar;
using namespace stellar::txtest;

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
    VirtualClock::system_time_point preferredUpgradeDatetime;
};

struct LedgerUpgradeCheck
{
    VirtualClock::system_time_point time;
    std::vector<LedgerUpgradeableData> expected;
};

namespace
{
void
simulateUpgrade(std::vector<LedgerUpgradeNode> const& nodes,
                std::vector<LedgerUpgradeCheck> const& checks,
                bool checkUpgradeStatus = false)
{
    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    historytestutils::TmpDirHistoryConfigurator configurator{};
    auto simulation =
        std::make_shared<Simulation>(Simulation::OVER_LOOPBACK, networkID);
    simulation->setCurrentVirtualTime(genesis(0, 0));

    // configure nodes
    auto keys = std::vector<SecretKey>{};
    auto configs = std::vector<Config>{};
    for (size_t i = 0; i < nodes.size(); i++)
    {
        keys.push_back(
            SecretKey::fromSeed(sha256("NODE_SEED_" + std::to_string(i))));
        configs.push_back(simulation->newConfig());
        // disable upgrade from config
        configs.back().TESTING_UPGRADE_DATETIME =
            VirtualClock::system_time_point();
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

    simulation->getNode(keys[0].getPublicKey())
        ->getHistoryArchiveManager()
        .initializeHistoryArchive("test");

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
                        .getLastClosedLedgerHeader()
                        .header.ledgerVersion == state[i].ledgerVersion);
            REQUIRE(node->getLedgerManager().getLastTxFee() ==
                    state[i].baseFee);
            REQUIRE(node->getLedgerManager().getLastMaxTxSetSize() ==
                    state[i].maxTxSetSize);
            REQUIRE(node->getLedgerManager().getLastReserve() ==
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

LedgerHeader
executeUpgrades(Application& app, xdr::xvector<UpgradeType, 6> const& upgrades)
{
    auto& lm = app.getLedgerManager();
    auto const& lcl = lm.getLastClosedLedgerHeader();
    auto txSet = std::make_shared<TxSetFrame>(lcl.hash);

    StellarValue sv = app.getHerder().makeStellarValue(
        txSet->getContentsHash(), 2, upgrades, app.getConfig().NODE_SEED);
    LedgerCloseData ledgerData(lcl.header.ledgerSeq + 1, txSet, sv);

    app.getLedgerManager().closeLedger(ledgerData);
    return lm.getLastClosedLedgerHeader().header;
};

LedgerHeader
executeUpgrade(Application& app, LedgerUpgrade const& lupgrade)
{
    return executeUpgrades(app, {toUpgradeType(lupgrade)});
};

void
testListUpgrades(VirtualClock::system_time_point preferredUpgradeDatetime,
                 bool shouldListAny)
{
    auto cfg = getTestConfig();
    cfg.LEDGER_PROTOCOL_VERSION = 10;
    cfg.TESTING_UPGRADE_DESIRED_FEE = 100;
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 50;
    cfg.TESTING_UPGRADE_RESERVE = 100000000;
    cfg.TESTING_UPGRADE_DATETIME = preferredUpgradeDatetime;

    auto header = LedgerHeader{};
    header.ledgerVersion = cfg.LEDGER_PROTOCOL_VERSION;
    header.baseFee = cfg.TESTING_UPGRADE_DESIRED_FEE;
    header.baseReserve = cfg.TESTING_UPGRADE_RESERVE;
    header.maxTxSetSize = cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE;
    header.scpValue.closeTime = VirtualClock::to_time_t(genesis(0, 0));

    auto protocolVersionUpgrade =
        makeProtocolVersionUpgrade(cfg.LEDGER_PROTOCOL_VERSION);
    auto baseFeeUpgrade = makeBaseFeeUpgrade(cfg.TESTING_UPGRADE_DESIRED_FEE);
    auto txCountUpgrade =
        makeTxCountUpgrade(cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE);
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

void
testValidateUpgrades(VirtualClock::system_time_point preferredUpgradeDatetime,
                     bool canBeValid)
{
    auto cfg = getTestConfig();
    cfg.LEDGER_PROTOCOL_VERSION = 10;
    cfg.TESTING_UPGRADE_DESIRED_FEE = 100;
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 50;
    cfg.TESTING_UPGRADE_RESERVE = 100000000;
    cfg.TESTING_UPGRADE_DATETIME = preferredUpgradeDatetime;

    auto checkTime = VirtualClock::to_time_t(genesis(0, 0));
    auto ledgerUpgradeType = LedgerUpgradeType{};

    // a ledgerheader used for base cases
    LedgerHeader baseLH;
    baseLH.ledgerVersion = 8;
    baseLH.scpValue.closeTime = checkTime;

    auto checkWith = [&](bool nomination) {
        SECTION("invalid upgrade data")
        {
            REQUIRE(!Upgrades{cfg}.isValid(UpgradeType{}, ledgerUpgradeType,
                                           nomination, cfg, baseLH));
        }

        SECTION("version")
        {
            if (nomination)
            {
                REQUIRE(canBeValid ==
                        Upgrades{cfg}.isValid(
                            toUpgradeType(makeProtocolVersionUpgrade(10)),
                            ledgerUpgradeType, nomination, cfg, baseLH));
            }
            else
            {
                REQUIRE(Upgrades{cfg}.isValid(
                    toUpgradeType(makeProtocolVersionUpgrade(10)),
                    ledgerUpgradeType, nomination, cfg, baseLH));
            }
            // 10 is queued, so this upgrade is only valid when not nominating
            bool v9Upgrade = Upgrades{cfg}.isValid(
                toUpgradeType(makeProtocolVersionUpgrade(9)), ledgerUpgradeType,
                nomination, cfg, baseLH);
            if (nomination)
            {
                REQUIRE(!v9Upgrade);
            }
            else
            {
                REQUIRE(v9Upgrade);
            }
            // rollback not allowed
            REQUIRE(!Upgrades{cfg}.isValid(
                toUpgradeType(makeProtocolVersionUpgrade(7)), ledgerUpgradeType,
                nomination, cfg, baseLH));
            // version is not supported
            REQUIRE(!Upgrades{cfg}.isValid(
                toUpgradeType(makeProtocolVersionUpgrade(11)),
                ledgerUpgradeType, nomination, cfg, baseLH));
        }

        SECTION("base fee")
        {
            if (nomination)
            {
                REQUIRE(canBeValid ==
                        Upgrades{cfg}.isValid(
                            toUpgradeType(makeBaseFeeUpgrade(100)),
                            ledgerUpgradeType, nomination, cfg, baseLH));
                REQUIRE(!Upgrades{cfg}.isValid(
                    toUpgradeType(makeBaseFeeUpgrade(99)), ledgerUpgradeType,
                    nomination, cfg, baseLH));
                REQUIRE(!Upgrades{cfg}.isValid(
                    toUpgradeType(makeBaseFeeUpgrade(101)), ledgerUpgradeType,
                    nomination, cfg, baseLH));
            }
            else
            {
                REQUIRE(Upgrades{cfg}.isValid(
                    toUpgradeType(makeBaseFeeUpgrade(100)), ledgerUpgradeType,
                    nomination, cfg, baseLH));
                REQUIRE(Upgrades{cfg}.isValid(
                    toUpgradeType(makeBaseFeeUpgrade(99)), ledgerUpgradeType,
                    nomination, cfg, baseLH));
                REQUIRE(Upgrades{cfg}.isValid(
                    toUpgradeType(makeBaseFeeUpgrade(101)), ledgerUpgradeType,
                    nomination, cfg, baseLH));
            }
            REQUIRE(!Upgrades{cfg}.isValid(toUpgradeType(makeBaseFeeUpgrade(0)),
                                           ledgerUpgradeType, nomination, cfg,
                                           baseLH));
        }

        SECTION("tx count")
        {
            if (nomination)
            {
                REQUIRE(canBeValid == Upgrades{cfg}.isValid(
                                          toUpgradeType(makeTxCountUpgrade(50)),
                                          ledgerUpgradeType, nomination, cfg,
                                          baseLH));
                REQUIRE(!Upgrades{cfg}.isValid(
                    toUpgradeType(makeTxCountUpgrade(49)), ledgerUpgradeType,
                    nomination, cfg, baseLH));
                REQUIRE(!Upgrades{cfg}.isValid(
                    toUpgradeType(makeTxCountUpgrade(51)), ledgerUpgradeType,
                    nomination, cfg, baseLH));
            }
            else
            {
                REQUIRE(Upgrades{cfg}.isValid(
                    toUpgradeType(makeTxCountUpgrade(50)), ledgerUpgradeType,
                    nomination, cfg, baseLH));
                REQUIRE(Upgrades{cfg}.isValid(
                    toUpgradeType(makeTxCountUpgrade(49)), ledgerUpgradeType,
                    nomination, cfg, baseLH));
                REQUIRE(Upgrades{cfg}.isValid(
                    toUpgradeType(makeTxCountUpgrade(51)), ledgerUpgradeType,
                    nomination, cfg, baseLH));
            }
            auto cfg0TxSize = cfg;
            cfg0TxSize.TESTING_UPGRADE_MAX_TX_SET_SIZE = 0;
            REQUIRE(canBeValid == Upgrades{cfg0TxSize}.isValid(
                                      toUpgradeType(makeTxCountUpgrade(0)),
                                      ledgerUpgradeType, nomination, cfg,
                                      baseLH));
        }

        SECTION("reserve")
        {
            if (nomination)
            {
                REQUIRE(canBeValid ==
                        Upgrades{cfg}.isValid(
                            toUpgradeType(makeBaseReserveUpgrade(100000000)),
                            ledgerUpgradeType, nomination, cfg, baseLH));
                REQUIRE(!Upgrades{cfg}.isValid(
                    toUpgradeType(makeBaseReserveUpgrade(99999999)),
                    ledgerUpgradeType, nomination, cfg, baseLH));
                REQUIRE(!Upgrades{cfg}.isValid(
                    toUpgradeType(makeBaseReserveUpgrade(100000001)),
                    ledgerUpgradeType, nomination, cfg, baseLH));
            }
            else
            {
                REQUIRE(Upgrades{cfg}.isValid(
                    toUpgradeType(makeBaseReserveUpgrade(100000000)),
                    ledgerUpgradeType, nomination, cfg, baseLH));
                REQUIRE(Upgrades{cfg}.isValid(
                    toUpgradeType(makeBaseReserveUpgrade(99999999)),
                    ledgerUpgradeType, nomination, cfg, baseLH));
                REQUIRE(Upgrades{cfg}.isValid(
                    toUpgradeType(makeBaseReserveUpgrade(100000001)),
                    ledgerUpgradeType, nomination, cfg, baseLH));
            }
            REQUIRE(!Upgrades{cfg}.isValid(
                toUpgradeType(makeBaseReserveUpgrade(0)), ledgerUpgradeType,
                nomination, cfg, baseLH));
        }
    };
    checkWith(true);
    checkWith(false);
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
    auto app = createTestApplication(clock, cfg);
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

    SECTION("ledger version")
    {
        REQUIRE(executeUpgrade(*app, makeProtocolVersionUpgrade(
                                         cfg.LEDGER_PROTOCOL_VERSION))
                    .ledgerVersion == cfg.LEDGER_PROTOCOL_VERSION);
    }

    SECTION("base fee")
    {
        REQUIRE(executeUpgrade(*app, makeBaseFeeUpgrade(1000)).baseFee == 1000);
    }

    SECTION("max tx")
    {
        REQUIRE(executeUpgrade(*app, makeTxCountUpgrade(1300)).maxTxSetSize ==
                1300);
    }

    SECTION("base reserve")
    {
        REQUIRE(
            executeUpgrade(*app, makeBaseReserveUpgrade(1000)).baseReserve ==
            1000);
    }

    SECTION("all")
    {
        auto header = executeUpgrades(
            *app, {toUpgradeType(
                       makeProtocolVersionUpgrade(cfg.LEDGER_PROTOCOL_VERSION)),
                   toUpgradeType(makeBaseFeeUpgrade(1000)),
                   toUpgradeType(makeTxCountUpgrade(1300)),
                   toUpgradeType(makeBaseReserveUpgrade(1000))});
        REQUIRE(header.ledgerVersion == cfg.LEDGER_PROTOCOL_VERSION);
        REQUIRE(header.baseFee == 1000);
        REQUIRE(header.maxTxSetSize == 1300);
        REQUIRE(header.baseReserve == 1000);
    }
}

TEST_CASE("upgrade to version 10", "[upgrades]")
{
    VirtualClock clock;
    auto cfg = getTestConfig(0);
    cfg.USE_CONFIG_FOR_GENESIS = false;

    auto app = createTestApplication(clock, cfg);
    app->start();
    executeUpgrade(*app, makeProtocolVersionUpgrade(9));

    auto& lm = app->getLedgerManager();
    auto txFee = lm.getLastTxFee();

    auto const& lcl = lm.getLastClosedLedgerHeader();
    auto txSet = std::make_shared<TxSetFrame>(lcl.hash);

    auto root = TestAccount::createRoot(*app);
    auto issuer = root.create("issuer", lm.getLastMinBalance(0) + 100 * txFee);
    auto native = txtest::makeNativeAsset();
    auto cur1 = issuer.asset("CUR1");
    auto cur2 = issuer.asset("CUR2");

    auto market = TestMarket{*app};

    auto executeUpgrade = [&] {
        REQUIRE(::executeUpgrade(*app, makeProtocolVersionUpgrade(10))
                    .ledgerVersion == 10);
    };

    auto getLiabilities = [&](TestAccount& acc) {
        Liabilities res;
        LedgerTxn ltx(app->getLedgerTxnRoot());
        auto account = stellar::loadAccount(ltx, acc.getPublicKey());
        res.selling = getSellingLiabilities(ltx.loadHeader(), account);
        res.buying = getBuyingLiabilities(ltx.loadHeader(), account);
        return res;
    };
    auto getAssetLiabilities = [&](TestAccount& acc, Asset const& asset) {
        Liabilities res;
        if (acc.hasTrustLine(asset))
        {
            LedgerTxn ltx(app->getLedgerTxnRoot());
            auto trust = stellar::loadTrustLine(ltx, acc.getPublicKey(), asset);
            res.selling = trust.getSellingLiabilities(ltx.loadHeader());
            res.buying = trust.getBuyingLiabilities(ltx.loadHeader());
        }
        return res;
    };

    auto createOffer = [&](TestAccount& acc, Asset const& selling,
                           Asset const& buying,
                           std::vector<TestMarketOffer>& offers,
                           OfferState const& afterUpgrade = OfferState::SAME) {
        OfferState state = {selling, buying, Price{2, 1}, 1000};
        auto offer = market.requireChangesWithOffer(
            {}, [&] { return market.addOffer(acc, state); });
        if (afterUpgrade == OfferState::SAME)
        {
            offers.push_back({offer.key, offer.state});
        }
        else
        {
            offers.push_back({offer.key, afterUpgrade});
        }
    };

    SECTION("one account, multiple offers, one asset pair")
    {
        SECTION("valid native")
        {
            auto a1 =
                root.create("A", lm.getLastMinBalance(5) + 2000 + 5 * txFee);
            a1.changeTrust(cur1, 6000);
            issuer.pay(a1, cur1, 2000);

            std::vector<TestMarketOffer> offers;
            createOffer(a1, native, cur1, offers);
            createOffer(a1, native, cur1, offers);
            createOffer(a1, cur1, native, offers);
            createOffer(a1, cur1, native, offers);

            market.requireChanges(offers, executeUpgrade);
            REQUIRE(getLiabilities(a1) == Liabilities{4000, 2000});
            REQUIRE(getAssetLiabilities(a1, cur1) == Liabilities{4000, 2000});
        }

        SECTION("invalid selling native")
        {
            auto a1 =
                root.create("A", lm.getLastMinBalance(5) + 1000 + 5 * txFee);
            a1.changeTrust(cur1, 6000);
            issuer.pay(a1, cur1, 2000);

            std::vector<TestMarketOffer> offers;
            createOffer(a1, native, cur1, offers, OfferState::DELETED);
            createOffer(a1, native, cur1, offers, OfferState::DELETED);
            createOffer(a1, cur1, native, offers);
            createOffer(a1, cur1, native, offers);

            market.requireChanges(offers, executeUpgrade);
            REQUIRE(getLiabilities(a1) == Liabilities{4000, 0});
            REQUIRE(getAssetLiabilities(a1, cur1) == Liabilities{0, 2000});
        }

        SECTION("invalid buying native")
        {
            auto createOfferQuantity =
                [&](TestAccount& acc, Asset const& selling, Asset const& buying,
                    int64_t quantity, std::vector<TestMarketOffer>& offers,
                    OfferState const& afterUpgrade = OfferState::SAME) {
                    OfferState state = {selling, buying, Price{2, 1}, quantity};
                    auto offer = market.requireChangesWithOffer(
                        {}, [&] { return market.addOffer(acc, state); });
                    if (afterUpgrade == OfferState::SAME)
                    {
                        offers.push_back({offer.key, offer.state});
                    }
                    else
                    {
                        offers.push_back({offer.key, afterUpgrade});
                    }
                };

            auto a1 =
                root.create("A", lm.getLastMinBalance(5) + 2000 + 5 * txFee);
            a1.changeTrust(cur1, INT64_MAX);
            issuer.pay(a1, cur1, INT64_MAX - 4000);

            std::vector<TestMarketOffer> offers;
            createOffer(a1, native, cur1, offers);
            createOffer(a1, native, cur1, offers);
            createOfferQuantity(a1, cur1, native, INT64_MAX / 4 - 2000, offers,
                                OfferState::DELETED);
            createOfferQuantity(a1, cur1, native, INT64_MAX / 4 - 2000, offers,
                                OfferState::DELETED);

            market.requireChanges(offers, executeUpgrade);
            REQUIRE(getLiabilities(a1) == Liabilities{0, 2000});
            REQUIRE(getAssetLiabilities(a1, cur1) == Liabilities{4000, 0});
        }

        SECTION("valid non-native")
        {
            auto a1 = root.create("A", lm.getLastMinBalance(6) + 6 * txFee);
            a1.changeTrust(cur1, 6000);
            a1.changeTrust(cur2, 6000);
            issuer.pay(a1, cur1, 2000);
            issuer.pay(a1, cur2, 2000);

            std::vector<TestMarketOffer> offers;
            createOffer(a1, cur1, cur2, offers);
            createOffer(a1, cur1, cur2, offers);
            createOffer(a1, cur2, cur1, offers);
            createOffer(a1, cur2, cur1, offers);

            market.requireChanges(offers, executeUpgrade);
            REQUIRE(getAssetLiabilities(a1, cur1) == Liabilities{4000, 2000});
            REQUIRE(getAssetLiabilities(a1, cur2) == Liabilities{4000, 2000});
        }

        SECTION("invalid non-native")
        {
            auto a1 = root.create("A", lm.getLastMinBalance(6) + 6 * txFee);
            a1.changeTrust(cur1, 6000);
            a1.changeTrust(cur2, 6000);
            issuer.pay(a1, cur1, 1000);
            issuer.pay(a1, cur2, 2000);

            std::vector<TestMarketOffer> offers;
            createOffer(a1, cur1, cur2, offers, OfferState::DELETED);
            createOffer(a1, cur1, cur2, offers, OfferState::DELETED);
            createOffer(a1, cur2, cur1, offers);
            createOffer(a1, cur2, cur1, offers);
            market.requireChanges(offers, executeUpgrade);

            REQUIRE(getAssetLiabilities(a1, cur1) == Liabilities{4000, 0});
            REQUIRE(getAssetLiabilities(a1, cur2) == Liabilities{0, 2000});
        }

        SECTION("valid non-native issued by account")
        {
            auto a1 = root.create("A", lm.getLastMinBalance(4) + 4 * txFee);
            auto issuedCur1 = a1.asset("CUR1");
            auto issuedCur2 = a1.asset("CUR2");

            std::vector<TestMarketOffer> offers;
            createOffer(a1, issuedCur1, issuedCur2, offers);
            createOffer(a1, issuedCur1, issuedCur2, offers);
            createOffer(a1, issuedCur2, issuedCur1, offers);
            createOffer(a1, issuedCur2, issuedCur1, offers);

            market.requireChanges(offers, executeUpgrade);
        }
    }

    SECTION("one account, multiple offers, multiple asset pairs")
    {
        SECTION("all valid")
        {
            auto a1 =
                root.create("A", lm.getLastMinBalance(14) + 4000 + 14 * txFee);
            a1.changeTrust(cur1, 12000);
            a1.changeTrust(cur2, 12000);
            issuer.pay(a1, cur1, 4000);
            issuer.pay(a1, cur2, 4000);

            std::vector<TestMarketOffer> offers;
            createOffer(a1, native, cur1, offers);
            createOffer(a1, native, cur1, offers);
            createOffer(a1, cur1, native, offers);
            createOffer(a1, cur1, native, offers);
            createOffer(a1, native, cur2, offers);
            createOffer(a1, native, cur2, offers);
            createOffer(a1, cur2, native, offers);
            createOffer(a1, cur2, native, offers);
            createOffer(a1, cur1, cur2, offers);
            createOffer(a1, cur1, cur2, offers);
            createOffer(a1, cur2, cur1, offers);
            createOffer(a1, cur2, cur1, offers);

            market.requireChanges(offers, executeUpgrade);
            REQUIRE(getLiabilities(a1) == Liabilities{8000, 4000});
            REQUIRE(getAssetLiabilities(a1, cur1) == Liabilities{8000, 4000});
            REQUIRE(getAssetLiabilities(a1, cur2) == Liabilities{8000, 4000});
        }

        SECTION("one invalid native")
        {
            auto a1 =
                root.create("A", lm.getLastMinBalance(14) + 2000 + 14 * txFee);
            a1.changeTrust(cur1, 12000);
            a1.changeTrust(cur2, 12000);
            issuer.pay(a1, cur1, 4000);
            issuer.pay(a1, cur2, 4000);

            std::vector<TestMarketOffer> offers;
            createOffer(a1, native, cur1, offers, OfferState::DELETED);
            createOffer(a1, native, cur1, offers, OfferState::DELETED);
            createOffer(a1, cur1, native, offers);
            createOffer(a1, cur1, native, offers);
            createOffer(a1, native, cur2, offers, OfferState::DELETED);
            createOffer(a1, native, cur2, offers, OfferState::DELETED);
            createOffer(a1, cur2, native, offers);
            createOffer(a1, cur2, native, offers);
            createOffer(a1, cur1, cur2, offers);
            createOffer(a1, cur1, cur2, offers);
            createOffer(a1, cur2, cur1, offers);
            createOffer(a1, cur2, cur1, offers);

            market.requireChanges(offers, executeUpgrade);
            REQUIRE(getLiabilities(a1) == Liabilities{8000, 0});
            REQUIRE(getAssetLiabilities(a1, cur1) == Liabilities{4000, 4000});
            REQUIRE(getAssetLiabilities(a1, cur2) == Liabilities{4000, 4000});
        }

        SECTION("one invalid non-native")
        {
            auto a1 =
                root.create("A", lm.getLastMinBalance(14) + 4000 + 14 * txFee);
            a1.changeTrust(cur1, 12000);
            a1.changeTrust(cur2, 12000);
            issuer.pay(a1, cur1, 4000);
            issuer.pay(a1, cur2, 1000);

            std::vector<TestMarketOffer> offers;
            createOffer(a1, native, cur1, offers);
            createOffer(a1, native, cur1, offers);
            createOffer(a1, cur1, native, offers);
            createOffer(a1, cur1, native, offers);
            createOffer(a1, native, cur2, offers);
            createOffer(a1, native, cur2, offers);
            createOffer(a1, cur2, native, offers, OfferState::DELETED);
            createOffer(a1, cur2, native, offers, OfferState::DELETED);
            createOffer(a1, cur1, cur2, offers);
            createOffer(a1, cur1, cur2, offers);
            createOffer(a1, cur2, cur1, offers, OfferState::DELETED);
            createOffer(a1, cur2, cur1, offers, OfferState::DELETED);

            market.requireChanges(offers, executeUpgrade);
            REQUIRE(getLiabilities(a1) == Liabilities{4000, 4000});
            REQUIRE(getAssetLiabilities(a1, cur1) == Liabilities{4000, 4000});
            REQUIRE(getAssetLiabilities(a1, cur2) == Liabilities{8000, 0});
        }
    }

    SECTION("multiple accounts, multiple offers, multiple asset pairs")
    {
        SECTION("all valid")
        {
            auto a1 =
                root.create("A", lm.getLastMinBalance(14) + 4000 + 14 * txFee);
            a1.changeTrust(cur1, 12000);
            a1.changeTrust(cur2, 12000);
            issuer.pay(a1, cur1, 4000);
            issuer.pay(a1, cur2, 4000);

            auto a2 =
                root.create("B", lm.getLastMinBalance(14) + 4000 + 14 * txFee);
            a2.changeTrust(cur1, 12000);
            a2.changeTrust(cur2, 12000);
            issuer.pay(a2, cur1, 4000);
            issuer.pay(a2, cur2, 4000);

            std::vector<TestMarketOffer> offers;
            createOffer(a1, native, cur1, offers);
            createOffer(a1, native, cur1, offers);
            createOffer(a1, cur1, native, offers);
            createOffer(a1, cur1, native, offers);
            createOffer(a1, native, cur2, offers);
            createOffer(a1, native, cur2, offers);
            createOffer(a1, cur2, native, offers);
            createOffer(a1, cur2, native, offers);
            createOffer(a1, cur1, cur2, offers);
            createOffer(a1, cur1, cur2, offers);
            createOffer(a1, cur2, cur1, offers);
            createOffer(a1, cur2, cur1, offers);

            createOffer(a2, native, cur1, offers);
            createOffer(a2, native, cur1, offers);
            createOffer(a2, cur1, native, offers);
            createOffer(a2, cur1, native, offers);
            createOffer(a2, native, cur2, offers);
            createOffer(a2, native, cur2, offers);
            createOffer(a2, cur2, native, offers);
            createOffer(a2, cur2, native, offers);
            createOffer(a2, cur1, cur2, offers);
            createOffer(a2, cur1, cur2, offers);
            createOffer(a2, cur2, cur1, offers);
            createOffer(a2, cur2, cur1, offers);

            market.requireChanges(offers, executeUpgrade);
            REQUIRE(getLiabilities(a1) == Liabilities{8000, 4000});
            REQUIRE(getAssetLiabilities(a1, cur1) == Liabilities{8000, 4000});
            REQUIRE(getAssetLiabilities(a1, cur2) == Liabilities{8000, 4000});
            REQUIRE(getLiabilities(a2) == Liabilities{8000, 4000});
            REQUIRE(getAssetLiabilities(a2, cur1) == Liabilities{8000, 4000});
            REQUIRE(getAssetLiabilities(a2, cur2) == Liabilities{8000, 4000});
        }

        SECTION("one invalid per account")
        {
            auto a1 =
                root.create("A", lm.getLastMinBalance(14) + 2000 + 14 * txFee);
            a1.changeTrust(cur1, 12000);
            a1.changeTrust(cur2, 12000);
            issuer.pay(a1, cur1, 4000);
            issuer.pay(a1, cur2, 4000);

            auto a2 =
                root.create("B", lm.getLastMinBalance(14) + 4000 + 14 * txFee);
            a2.changeTrust(cur1, 12000);
            a2.changeTrust(cur2, 12000);
            issuer.pay(a2, cur1, 4000);
            issuer.pay(a2, cur2, 2000);

            std::vector<TestMarketOffer> offers;
            createOffer(a1, native, cur1, offers, OfferState::DELETED);
            createOffer(a1, native, cur1, offers, OfferState::DELETED);
            createOffer(a1, cur1, native, offers);
            createOffer(a1, cur1, native, offers);
            createOffer(a1, native, cur2, offers, OfferState::DELETED);
            createOffer(a1, native, cur2, offers, OfferState::DELETED);
            createOffer(a1, cur2, native, offers);
            createOffer(a1, cur2, native, offers);
            createOffer(a1, cur1, cur2, offers);
            createOffer(a1, cur1, cur2, offers);
            createOffer(a1, cur2, cur1, offers);
            createOffer(a1, cur2, cur1, offers);

            createOffer(a2, native, cur1, offers);
            createOffer(a2, native, cur1, offers);
            createOffer(a2, cur1, native, offers);
            createOffer(a2, cur1, native, offers);
            createOffer(a2, native, cur2, offers);
            createOffer(a2, native, cur2, offers);
            createOffer(a2, cur2, native, offers, OfferState::DELETED);
            createOffer(a2, cur2, native, offers, OfferState::DELETED);
            createOffer(a2, cur1, cur2, offers);
            createOffer(a2, cur1, cur2, offers);
            createOffer(a2, cur2, cur1, offers, OfferState::DELETED);
            createOffer(a2, cur2, cur1, offers, OfferState::DELETED);

            market.requireChanges(offers, executeUpgrade);
            REQUIRE(getLiabilities(a1) == Liabilities{8000, 0});
            REQUIRE(getAssetLiabilities(a1, cur1) == Liabilities{4000, 4000});
            REQUIRE(getAssetLiabilities(a1, cur2) == Liabilities{4000, 4000});
            REQUIRE(getLiabilities(a2) == Liabilities{4000, 4000});
            REQUIRE(getAssetLiabilities(a2, cur1) == Liabilities{4000, 4000});
            REQUIRE(getAssetLiabilities(a2, cur2) == Liabilities{8000, 0});
        }
    }

    SECTION("liabilities overflow")
    {
        auto createOfferLarge = [&](TestAccount& acc, Asset const& selling,
                                    Asset const& buying,
                                    std::vector<TestMarketOffer>& offers,
                                    OfferState const& afterUpgrade =
                                        OfferState::SAME) {
            OfferState state = {selling, buying, Price{2, 1}, INT64_MAX / 3};
            auto offer = market.requireChangesWithOffer(
                {}, [&] { return market.addOffer(acc, state); });
            if (afterUpgrade == OfferState::SAME)
            {
                offers.push_back({offer.key, offer.state});
            }
            else
            {
                offers.push_back({offer.key, afterUpgrade});
            }
        };

        SECTION("non-native for non-native, all invalid")
        {
            auto a1 = root.create("A", lm.getLastMinBalance(6) + 6 * txFee);
            a1.changeTrust(cur1, INT64_MAX);
            a1.changeTrust(cur2, INT64_MAX);
            issuer.pay(a1, cur1, INT64_MAX / 3);
            issuer.pay(a1, cur2, INT64_MAX / 3);

            std::vector<TestMarketOffer> offers;
            createOfferLarge(a1, cur1, cur2, offers, OfferState::DELETED);
            createOfferLarge(a1, cur1, cur2, offers, OfferState::DELETED);
            createOfferLarge(a1, cur2, cur1, offers, OfferState::DELETED);
            createOfferLarge(a1, cur2, cur1, offers, OfferState::DELETED);

            market.requireChanges(offers, executeUpgrade);
            REQUIRE(getAssetLiabilities(a1, cur1) == Liabilities{0, 0});
            REQUIRE(getAssetLiabilities(a1, cur2) == Liabilities{0, 0});
        }

        SECTION("non-native for non-native, half invalid")
        {
            auto a1 = root.create("A", lm.getLastMinBalance(6) + 6 * txFee);
            a1.changeTrust(cur1, INT64_MAX);
            a1.changeTrust(cur2, INT64_MAX);
            issuer.pay(a1, cur1, INT64_MAX / 3);
            issuer.pay(a1, cur2, INT64_MAX / 3);

            std::vector<TestMarketOffer> offers;
            createOfferLarge(a1, cur1, cur2, offers, OfferState::DELETED);
            createOfferLarge(a1, cur1, cur2, offers, OfferState::DELETED);
            createOfferLarge(a1, cur2, cur1, offers);

            market.requireChanges(offers, executeUpgrade);
            REQUIRE(getAssetLiabilities(a1, cur1) ==
                    Liabilities{INT64_MAX / 3 * 2, 0});
            REQUIRE(getAssetLiabilities(a1, cur2) ==
                    Liabilities{0, INT64_MAX / 3});
        }

        SECTION("issued asset for issued asset")
        {
            auto a1 = root.create("A", lm.getLastMinBalance(4) + 4 * txFee);
            auto issuedCur1 = a1.asset("CUR1");
            auto issuedCur2 = a1.asset("CUR2");

            std::vector<TestMarketOffer> offers;
            createOfferLarge(a1, issuedCur1, issuedCur2, offers);
            createOfferLarge(a1, issuedCur1, issuedCur2, offers);
            createOfferLarge(a1, issuedCur2, issuedCur1, offers);
            createOfferLarge(a1, issuedCur2, issuedCur1, offers);

            market.requireChanges(offers, executeUpgrade);
        }
    }

    SECTION("adjust offers")
    {
        SECTION("offers that do not satisfy thresholds are deleted")
        {
            auto createOfferQuantity =
                [&](TestAccount& acc, Asset const& selling, Asset const& buying,
                    int64_t quantity, std::vector<TestMarketOffer>& offers,
                    OfferState const& afterUpgrade = OfferState::SAME) {
                    OfferState state = {selling, buying, Price{3, 2}, quantity};
                    auto offer = market.requireChangesWithOffer(
                        {}, [&] { return market.addOffer(acc, state); });
                    if (afterUpgrade == OfferState::SAME)
                    {
                        offers.push_back({offer.key, offer.state});
                    }
                    else
                    {
                        offers.push_back({offer.key, afterUpgrade});
                    }
                };

            auto a1 = root.create("A", lm.getLastMinBalance(6) + 6 * txFee);
            a1.changeTrust(cur1, 1000);
            a1.changeTrust(cur2, 1000);
            issuer.pay(a1, cur1, 500);
            issuer.pay(a1, cur2, 500);

            std::vector<TestMarketOffer> offers;
            createOfferQuantity(a1, cur1, cur2, 27, offers,
                                OfferState::DELETED);
            createOfferQuantity(a1, cur1, cur2, 28, offers);
            createOfferQuantity(a1, cur2, cur1, 27, offers,
                                OfferState::DELETED);
            createOfferQuantity(a1, cur2, cur1, 28, offers);

            market.requireChanges(offers, executeUpgrade);
            REQUIRE(getLiabilities(a1) == Liabilities{0, 0});
            REQUIRE(getAssetLiabilities(a1, cur1) == Liabilities{42, 28});
            REQUIRE(getAssetLiabilities(a1, cur2) == Liabilities{42, 28});
        }

        SECTION("offers that need rounding are rounded")
        {
            auto createOfferQuantity =
                [&](TestAccount& acc, Asset const& selling, Asset const& buying,
                    int64_t quantity, std::vector<TestMarketOffer>& offers,
                    OfferState const& afterUpgrade = OfferState::SAME) {
                    OfferState state = {selling, buying, Price{2, 3}, quantity};
                    auto offer = market.requireChangesWithOffer(
                        {}, [&] { return market.addOffer(acc, state); });
                    if (afterUpgrade == OfferState::SAME)
                    {
                        offers.push_back({offer.key, offer.state});
                    }
                    else
                    {
                        offers.push_back({offer.key, afterUpgrade});
                    }
                };

            auto a1 = root.create("A", lm.getLastMinBalance(4) + 4 * txFee);
            a1.changeTrust(cur1, 1000);
            a1.changeTrust(cur2, 1000);
            issuer.pay(a1, cur1, 500);

            std::vector<TestMarketOffer> offers;
            createOfferQuantity(a1, cur1, cur2, 201, offers);
            createOfferQuantity(a1, cur1, cur2, 202, offers,
                                {cur1, cur2, Price{2, 3}, 201});

            market.requireChanges(offers, executeUpgrade);
            REQUIRE(getLiabilities(a1) == Liabilities{0, 0});
            REQUIRE(getAssetLiabilities(a1, cur1) == Liabilities{0, 402});
            REQUIRE(getAssetLiabilities(a1, cur2) == Liabilities{268, 0});
        }

        SECTION("offers that do not satisfy thresholds still contribute "
                "liabilities")
        {
            auto createOfferQuantity =
                [&](TestAccount& acc, Asset const& selling, Asset const& buying,
                    int64_t quantity, std::vector<TestMarketOffer>& offers,
                    OfferState const& afterUpgrade = OfferState::SAME) {
                    OfferState state = {selling, buying, Price{3, 2}, quantity};
                    auto offer = market.requireChangesWithOffer(
                        {}, [&] { return market.addOffer(acc, state); });
                    if (afterUpgrade == OfferState::SAME)
                    {
                        offers.push_back({offer.key, offer.state});
                    }
                    else
                    {
                        offers.push_back({offer.key, afterUpgrade});
                    }
                };

            auto a1 =
                root.create("A", lm.getLastMinBalance(10) + 2000 + 12 * txFee);
            a1.changeTrust(cur1, 5125);
            a1.changeTrust(cur2, 5125);
            issuer.pay(a1, cur1, 2050);
            issuer.pay(a1, cur2, 2050);

            SECTION("normal offers remain without liabilities from"
                    " offers that do not satisfy thresholds")
            {
                // Pay txFee to send 4*baseReserve + 3*txFee for net balance
                // decrease of 4*baseReserve + 4*txFee. This matches the balance
                // decrease from creating 4 offers as in the next test section.
                a1.pay(root, 4 * lm.getLastReserve() + 3 * txFee);

                std::vector<TestMarketOffer> offers;
                createOfferQuantity(a1, cur1, native, 1000, offers);
                createOfferQuantity(a1, cur1, native, 1000, offers);
                createOfferQuantity(a1, native, cur1, 1000, offers);
                createOfferQuantity(a1, native, cur1, 1000, offers);

                market.requireChanges(offers, executeUpgrade);
                REQUIRE(getLiabilities(a1) == Liabilities{3000, 2000});
                REQUIRE(getAssetLiabilities(a1, cur1) ==
                        Liabilities{3000, 2000});
                REQUIRE(getAssetLiabilities(a1, cur2) == Liabilities{0, 0});
            }

            SECTION("normal offers deleted with liabilities from"
                    " offers that do not satisfy thresholds")
            {
                std::vector<TestMarketOffer> offers;
                createOfferQuantity(a1, cur1, cur2, 27, offers,
                                    OfferState::DELETED);
                createOfferQuantity(a1, cur1, cur2, 27, offers,
                                    OfferState::DELETED);
                createOfferQuantity(a1, cur1, native, 1000, offers,
                                    OfferState::DELETED);
                createOfferQuantity(a1, cur1, native, 1000, offers,
                                    OfferState::DELETED);
                createOfferQuantity(a1, cur2, cur1, 27, offers,
                                    OfferState::DELETED);
                createOfferQuantity(a1, cur2, cur1, 27, offers,
                                    OfferState::DELETED);
                createOfferQuantity(a1, native, cur1, 1000, offers,
                                    OfferState::DELETED);
                createOfferQuantity(a1, native, cur1, 1000, offers,
                                    OfferState::DELETED);

                market.requireChanges(offers, executeUpgrade);
                REQUIRE(getLiabilities(a1) == Liabilities{0, 0});
                REQUIRE(getAssetLiabilities(a1, cur1) == Liabilities{0, 0});
                REQUIRE(getAssetLiabilities(a1, cur2) == Liabilities{0, 0});
            }
        }
    }

    SECTION("unauthorized offers")
    {
        auto toSet = static_cast<uint32_t>(AUTH_REQUIRED_FLAG) |
                     static_cast<uint32_t>(AUTH_REVOCABLE_FLAG);
        issuer.setOptions(txtest::setFlags(toSet));

        SECTION("both assets require authorization and authorized")
        {
            auto a1 = root.create("A", lm.getLastMinBalance(6) + 6 * txFee);
            a1.changeTrust(cur1, 6000);
            a1.changeTrust(cur2, 6000);
            issuer.allowTrust(cur1, a1);
            issuer.allowTrust(cur2, a1);
            issuer.pay(a1, cur1, 2000);
            issuer.pay(a1, cur2, 2000);

            std::vector<TestMarketOffer> offers;
            createOffer(a1, cur1, cur2, offers);
            createOffer(a1, cur1, cur2, offers);
            createOffer(a1, cur2, cur1, offers);
            createOffer(a1, cur2, cur1, offers);

            market.requireChanges(offers, executeUpgrade);
            REQUIRE(getAssetLiabilities(a1, cur1) == Liabilities{4000, 2000});
            REQUIRE(getAssetLiabilities(a1, cur2) == Liabilities{4000, 2000});
        }

        SECTION("selling asset not authorized")
        {
            auto a1 =
                root.create("A", lm.getLastMinBalance(6) + 4000 + 6 * txFee);
            a1.changeTrust(cur1, 6000);
            a1.changeTrust(cur2, 6000);
            issuer.allowTrust(cur1, a1);
            issuer.allowTrust(cur2, a1);
            issuer.pay(a1, cur1, 2000);
            issuer.pay(a1, cur2, 2000);

            std::vector<TestMarketOffer> offers;
            createOffer(a1, cur1, native, offers, OfferState::DELETED);
            createOffer(a1, cur1, native, offers, OfferState::DELETED);
            createOffer(a1, cur2, native, offers);
            createOffer(a1, cur2, native, offers);

            issuer.denyTrust(cur1, a1);

            market.requireChanges(offers, executeUpgrade);
            REQUIRE(getLiabilities(a1) == Liabilities{4000, 0});
            REQUIRE(getAssetLiabilities(a1, cur1) == Liabilities{0, 0});
            REQUIRE(getAssetLiabilities(a1, cur2) == Liabilities{0, 2000});
        }

        SECTION("buying asset not authorized")
        {
            auto a1 =
                root.create("A", lm.getLastMinBalance(6) + 4000 + 6 * txFee);
            a1.changeTrust(cur1, 6000);
            a1.changeTrust(cur2, 6000);
            issuer.allowTrust(cur1, a1);
            issuer.allowTrust(cur2, a1);
            issuer.pay(a1, cur1, 2000);
            issuer.pay(a1, cur2, 2000);

            std::vector<TestMarketOffer> offers;
            createOffer(a1, native, cur1, offers, OfferState::DELETED);
            createOffer(a1, native, cur1, offers, OfferState::DELETED);
            createOffer(a1, native, cur2, offers);
            createOffer(a1, native, cur2, offers);

            issuer.denyTrust(cur1, a1);

            market.requireChanges(offers, executeUpgrade);
            REQUIRE(getLiabilities(a1) == Liabilities{0, 2000});
            REQUIRE(getAssetLiabilities(a1, cur1) == Liabilities{0, 0});
            REQUIRE(getAssetLiabilities(a1, cur2) == Liabilities{4000, 0});
        }

        SECTION("unauthorized offers still contribute liabilities")
        {
            auto a1 =
                root.create("A", lm.getLastMinBalance(10) + 2000 + 10 * txFee);
            a1.changeTrust(cur1, 6000);
            a1.changeTrust(cur2, 6000);
            issuer.allowTrust(cur1, a1);
            issuer.allowTrust(cur2, a1);
            issuer.pay(a1, cur1, 2000);
            issuer.pay(a1, cur2, 2000);

            SECTION("authorized offers remain without liabilities from"
                    " unauthorized offers")
            {
                // Pay txFee to send 4*baseReserve + 3*txFee for net balance
                // decrease of 4*baseReserve + 4*txFee. This matches the balance
                // decrease from creating 4 offers as in the next test section.
                a1.pay(root, 4 * lm.getLastReserve() + 3 * txFee);

                std::vector<TestMarketOffer> offers;
                createOffer(a1, cur1, native, offers);
                createOffer(a1, cur1, native, offers);
                createOffer(a1, native, cur1, offers);
                createOffer(a1, native, cur1, offers);

                issuer.denyTrust(cur2, a1);

                market.requireChanges(offers, executeUpgrade);
                REQUIRE(getLiabilities(a1) == Liabilities{4000, 2000});
                REQUIRE(getAssetLiabilities(a1, cur1) ==
                        Liabilities{4000, 2000});
                REQUIRE(getAssetLiabilities(a1, cur2) == Liabilities{0, 0});
            }

            SECTION("authorized offers deleted with liabilities from"
                    " unauthorized offers")
            {
                std::vector<TestMarketOffer> offers;
                createOffer(a1, cur1, cur2, offers, OfferState::DELETED);
                createOffer(a1, cur1, cur2, offers, OfferState::DELETED);
                createOffer(a1, cur1, native, offers, OfferState::DELETED);
                createOffer(a1, cur1, native, offers, OfferState::DELETED);
                createOffer(a1, cur2, cur1, offers, OfferState::DELETED);
                createOffer(a1, cur2, cur1, offers, OfferState::DELETED);
                createOffer(a1, native, cur1, offers, OfferState::DELETED);
                createOffer(a1, native, cur1, offers, OfferState::DELETED);

                issuer.denyTrust(cur2, a1);

                market.requireChanges(offers, executeUpgrade);
                REQUIRE(getLiabilities(a1) == Liabilities{0, 0});
                REQUIRE(getAssetLiabilities(a1, cur1) == Liabilities{0, 0});
                REQUIRE(getAssetLiabilities(a1, cur2) == Liabilities{0, 0});
            }
        }
    }

    SECTION("deleted trust lines")
    {
        auto a1 = root.create("A", lm.getLastMinBalance(4) + 6 * txFee);
        a1.changeTrust(cur1, 6000);
        a1.changeTrust(cur2, 6000);
        issuer.pay(a1, cur1, 2000);

        std::vector<TestMarketOffer> offers;
        createOffer(a1, cur1, cur2, offers, OfferState::DELETED);
        createOffer(a1, cur1, cur2, offers, OfferState::DELETED);

        SECTION("deleted selling trust line")
        {
            a1.pay(issuer, cur1, 2000);
            a1.changeTrust(cur1, 0);

            market.requireChanges(offers, executeUpgrade);
            REQUIRE(getAssetLiabilities(a1, cur1) == Liabilities{0, 0});
            REQUIRE(getAssetLiabilities(a1, cur2) == Liabilities{0, 0});
        }
        SECTION("deleted buying trust line")
        {
            a1.changeTrust(cur2, 0);

            market.requireChanges(offers, executeUpgrade);
            REQUIRE(getAssetLiabilities(a1, cur1) == Liabilities{0, 0});
            REQUIRE(getAssetLiabilities(a1, cur2) == Liabilities{0, 0});
        }
    }

    SECTION("offers with deleted trust lines still contribute liabilities")
    {
        auto a1 =
            root.create("A", lm.getLastMinBalance(10) + 2000 + 12 * txFee);
        a1.changeTrust(cur1, 6000);
        a1.changeTrust(cur2, 6000);
        issuer.pay(a1, cur1, 2000);
        issuer.pay(a1, cur2, 2000);

        SECTION("normal offers remain without liabilities from"
                " offers with deleted trust lines")
        {
            // Pay txFee to send 4*baseReserve + 3*txFee for net balance
            // decrease of 4*baseReserve + 4*txFee. This matches the balance
            // decrease from creating 4 offers as in the next test section.
            a1.pay(root, 4 * lm.getLastReserve() + 3 * txFee);

            std::vector<TestMarketOffer> offers;
            createOffer(a1, cur1, native, offers);
            createOffer(a1, cur1, native, offers);
            createOffer(a1, native, cur1, offers);
            createOffer(a1, native, cur1, offers);

            a1.pay(issuer, cur2, 2000);
            a1.changeTrust(cur2, 0);

            market.requireChanges(offers, executeUpgrade);
            REQUIRE(getLiabilities(a1) == Liabilities{4000, 2000});
            REQUIRE(getAssetLiabilities(a1, cur1) == Liabilities{4000, 2000});
            REQUIRE(getAssetLiabilities(a1, cur2) == Liabilities{0, 0});
        }

        SECTION("normal offers deleted with liabilities from"
                " offers with deleted trust lines")
        {
            std::vector<TestMarketOffer> offers;
            createOffer(a1, cur1, cur2, offers, OfferState::DELETED);
            createOffer(a1, cur1, cur2, offers, OfferState::DELETED);
            createOffer(a1, cur1, native, offers, OfferState::DELETED);
            createOffer(a1, cur1, native, offers, OfferState::DELETED);
            createOffer(a1, cur2, cur1, offers, OfferState::DELETED);
            createOffer(a1, cur2, cur1, offers, OfferState::DELETED);
            createOffer(a1, native, cur1, offers, OfferState::DELETED);
            createOffer(a1, native, cur1, offers, OfferState::DELETED);

            a1.pay(issuer, cur2, 2000);
            a1.changeTrust(cur2, 0);

            market.requireChanges(offers, executeUpgrade);
            REQUIRE(getLiabilities(a1) == Liabilities{0, 0});
            REQUIRE(getAssetLiabilities(a1, cur1) == Liabilities{0, 0});
            REQUIRE(getAssetLiabilities(a1, cur2) == Liabilities{0, 0});
        }
    }
}

TEST_CASE("upgrade to version 11", "[upgrades]")
{
    VirtualClock clock;
    auto cfg = getTestConfig(0);
    cfg.USE_CONFIG_FOR_GENESIS = false;

    auto app = createTestApplication(clock, cfg);
    app->start();
    executeUpgrade(*app, makeProtocolVersionUpgrade(10));

    auto& lm = app->getLedgerManager();
    uint32_t newProto = 11;
    auto root = TestAccount{*app, txtest::getRoot(app->getNetworkID())};

    for (size_t i = 0; i < 10; ++i)
    {
        auto stranger =
            TestAccount{*app, txtest::getAccount(fmt::format("stranger{}", i))};
        TxSetFramePtr txSet =
            std::make_shared<TxSetFrame>(lm.getLastClosedLedgerHeader().hash);
        uint32_t ledgerSeq = lm.getLastClosedLedgerNum() + 1;
        uint64_t minBalance = lm.getLastMinBalance(5);
        uint64_t big = minBalance + ledgerSeq;
        uint64_t closeTime = 60 * 5 * ledgerSeq;
        txSet->add(root.tx({txtest::createAccount(stranger, big)}));
        // Provoke sortForHash and hash-caching:
        txSet->getContentsHash();

        // On 4th iteration of advance (a.k.a. ledgerSeq 5), perform a
        // ledger-protocol version upgrade to the new protocol, to activate
        // INITENTRY behaviour.
        auto upgrades = xdr::xvector<UpgradeType, 6>{};
        if (ledgerSeq == 5)
        {
            auto ledgerUpgrade = LedgerUpgrade{LEDGER_UPGRADE_VERSION};
            ledgerUpgrade.newLedgerVersion() = newProto;
            auto v = xdr::xdr_to_opaque(ledgerUpgrade);
            upgrades.push_back(UpgradeType{v.begin(), v.end()});
            CLOG_INFO(Ledger, "Ledger {} upgrading to v{}", ledgerSeq,
                      newProto);
        }

        StellarValue sv = app->getHerder().makeStellarValue(
            txSet->getContentsHash(), closeTime, upgrades,
            app->getConfig().NODE_SEED);
        lm.closeLedger(LedgerCloseData(ledgerSeq, txSet, sv));
        auto& bm = app->getBucketManager();
        auto mc = bm.readMergeCounters();
        CLOG_INFO(Bucket,
                  "Ledger {} did {} old-protocol merges, {} new-protocol "
                  "merges, {} new INITENTRYs, {} old INITENTRYs",
                  ledgerSeq, mc.mPreInitEntryProtocolMerges,
                  mc.mPostInitEntryProtocolMerges, mc.mNewInitEntries,
                  mc.mOldInitEntries);
        for (uint32_t level = 0; level < BucketList::kNumLevels; ++level)
        {
            auto& lev = bm.getBucketList().getLevel(level);
            BucketTests::EntryCounts currCounts(lev.getCurr());
            BucketTests::EntryCounts snapCounts(lev.getSnap());
            CLOG_INFO(
                Bucket,
                "post-ledger {} close, init counts: level {}, {} in curr, "
                "{} in snap",
                ledgerSeq, level, currCounts.nInit, snapCounts.nInit);
        }
        if (ledgerSeq < 5)
        {
            // Check that before upgrade, we did not do any INITENTRY.
            REQUIRE(mc.mPreInitEntryProtocolMerges != 0);
            REQUIRE(mc.mPostInitEntryProtocolMerges == 0);
            REQUIRE(mc.mNewInitEntries == 0);
            REQUIRE(mc.mOldInitEntries == 0);
        }
        else
        {
            // Check several subtle characteristics of the post-upgrade
            // environment:
            //   - Old-protocol merges stop happening (there should have
            //     been 6 before the upgrade, but we re-use a merge we did at
            //     ledger 1 for ledger 2 spill, so the counter is at 5)
            //   - New-protocol merges start happening.
            //   - At the upgrade (5), we find 1 INITENTRY in lev[0].curr
            //   - The next two (6, 7), propagate INITENTRYs to lev[0].snap
            //   - From 8 on, the INITENTRYs propagate to lev[1].curr
            REQUIRE(mc.mPreInitEntryProtocolMerges == 5);
            REQUIRE(mc.mPostInitEntryProtocolMerges != 0);
            auto& lev0 = bm.getBucketList().getLevel(0);
            auto& lev1 = bm.getBucketList().getLevel(1);
            auto lev0Curr = lev0.getCurr();
            auto lev0Snap = lev0.getSnap();
            auto lev1Curr = lev1.getCurr();
            auto lev1Snap = lev1.getSnap();
            BucketTests::EntryCounts lev0CurrCounts(lev0Curr);
            BucketTests::EntryCounts lev0SnapCounts(lev0Snap);
            BucketTests::EntryCounts lev1CurrCounts(lev1Curr);
            auto getVers = [](std::shared_ptr<Bucket> b) -> uint32_t {
                return BucketInputIterator(b).getMetadata().ledgerVersion;
            };
            switch (ledgerSeq)
            {
            default:
            case 8:
                REQUIRE(getVers(lev1Curr) == newProto);
                REQUIRE(lev1CurrCounts.nInit != 0);
            case 7:
            case 6:
                REQUIRE(getVers(lev0Snap) == newProto);
                REQUIRE(lev0SnapCounts.nInit != 0);
            case 5:
                REQUIRE(getVers(lev0Curr) == newProto);
                REQUIRE(lev0CurrCounts.nInit != 0);
            }
        }
    }
}

TEST_CASE("upgrade to version 12", "[upgrades]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.USE_CONFIG_FOR_GENESIS = false;

    auto app = createTestApplication(clock, cfg);
    app->start();
    executeUpgrade(*app, makeProtocolVersionUpgrade(11));

    auto& lm = app->getLedgerManager();
    uint32_t oldProto = 11;
    uint32_t newProto = 12;
    auto root = TestAccount{*app, txtest::getRoot(app->getNetworkID())};

    for (size_t i = 0; i < 10; ++i)
    {
        auto stranger =
            TestAccount{*app, txtest::getAccount(fmt::format("stranger{}", i))};
        TxSetFramePtr txSet =
            std::make_shared<TxSetFrame>(lm.getLastClosedLedgerHeader().hash);
        uint32_t ledgerSeq = lm.getLastClosedLedgerNum() + 1;
        uint64_t minBalance = lm.getLastMinBalance(5);
        uint64_t big = minBalance + ledgerSeq;
        uint64_t closeTime = 60 * 5 * ledgerSeq;
        txSet->add(root.tx({txtest::createAccount(stranger, big)}));
        // Provoke sortForHash and hash-caching:
        txSet->getContentsHash();

        // On 4th iteration of advance (a.k.a. ledgerSeq 5), perform a
        // ledger-protocol version upgrade to the new protocol, to
        // start new-style merges (no shadows)
        auto upgrades = xdr::xvector<UpgradeType, 6>{};
        if (ledgerSeq == 5)
        {
            auto ledgerUpgrade = LedgerUpgrade{LEDGER_UPGRADE_VERSION};
            ledgerUpgrade.newLedgerVersion() = newProto;
            auto v = xdr::xdr_to_opaque(ledgerUpgrade);
            upgrades.push_back(UpgradeType{v.begin(), v.end()});
            CLOG_INFO(Ledger, "Ledger {} upgrading to v{}", ledgerSeq,
                      newProto);
        }
        StellarValue sv = app->getHerder().makeStellarValue(
            txSet->getContentsHash(), closeTime, upgrades,
            app->getConfig().NODE_SEED);
        lm.closeLedger(LedgerCloseData(ledgerSeq, txSet, sv));
        auto& bm = app->getBucketManager();
        auto& bl = bm.getBucketList();
        while (!bl.futuresAllResolved())
        {
            bl.resolveAnyReadyFutures();
        }
        auto mc = bm.readMergeCounters();

        if (ledgerSeq < 5)
        {
            REQUIRE(mc.mPreShadowRemovalProtocolMerges != 0);
        }
        else
        {
            auto& lev0 = bm.getBucketList().getLevel(0);
            auto& lev1 = bm.getBucketList().getLevel(1);
            auto lev0Curr = lev0.getCurr();
            auto lev0Snap = lev0.getSnap();
            auto lev1Curr = lev1.getCurr();
            auto lev1Snap = lev1.getSnap();
            auto getVers = [](std::shared_ptr<Bucket> b) -> uint32_t {
                return BucketInputIterator(b).getMetadata().ledgerVersion;
            };
            switch (ledgerSeq)
            {
            case 8:
                REQUIRE(getVers(lev1Curr) == newProto);
                REQUIRE(getVers(lev1Snap) == oldProto);
                REQUIRE(mc.mPostShadowRemovalProtocolMerges == 6);
                // One more old-style merge despite the upgrade
                // At ledger 8, level 2 spills, and starts an old-style merge,
                // as level 1 snap is still of old version
                REQUIRE(mc.mPreShadowRemovalProtocolMerges == 6);
                break;
            case 7:
                REQUIRE(getVers(lev0Snap) == newProto);
                REQUIRE(getVers(lev1Curr) == oldProto);
                REQUIRE(mc.mPostShadowRemovalProtocolMerges == 4);
                REQUIRE(mc.mPreShadowRemovalProtocolMerges == 5);
                break;
            case 6:
                REQUIRE(getVers(lev0Snap) == newProto);
                REQUIRE(getVers(lev1Curr) == oldProto);
                REQUIRE(mc.mPostShadowRemovalProtocolMerges == 3);
                REQUIRE(mc.mPreShadowRemovalProtocolMerges == 5);
                break;
            case 5:
                REQUIRE(getVers(lev0Curr) == newProto);
                REQUIRE(getVers(lev0Snap) == oldProto);
                REQUIRE(mc.mPostShadowRemovalProtocolMerges == 1);
                REQUIRE(mc.mPreShadowRemovalProtocolMerges == 5);
                break;
            default:
                break;
            }
        }
    }
}

TEST_CASE("upgrade to version 13", "[upgrades]")
{
    VirtualClock clock;
    auto cfg = getTestConfig(0);
    cfg.USE_CONFIG_FOR_GENESIS = false;

    auto app = createTestApplication(clock, cfg);
    app->start();
    executeUpgrade(*app, makeProtocolVersionUpgrade(12));

    auto& lm = app->getLedgerManager();
    auto& herder = static_cast<HerderImpl&>(app->getHerder());

    auto root = TestAccount::createRoot(*app);
    auto acc = root.create("A", lm.getLastMinBalance(2));

    herder.recvTransaction(root.tx({payment(root, 1)}));
    herder.recvTransaction(root.tx({payment(root, 2)}));
    herder.recvTransaction(acc.tx({payment(acc, 1)}));
    herder.recvTransaction(acc.tx({payment(acc, 2)}));

    auto txSet = herder.getTransactionQueue().toTxSet({});
    for (auto const& tx : txSet->mTransactions)
    {
        REQUIRE(tx->getEnvelope().type() == ENVELOPE_TYPE_TX_V0);
    }

    {
        auto const& lcl = lm.getLastClosedLedgerHeader();
        auto ledgerSeq = lcl.header.ledgerSeq + 1;

        auto emptyTxSet = std::make_shared<TxSetFrame>(lcl.hash);
        herder.getPendingEnvelopes().putTxSet(emptyTxSet->getContentsHash(),
                                              ledgerSeq, emptyTxSet);

        auto upgrade = toUpgradeType(makeProtocolVersionUpgrade(13));
        StellarValue sv =
            herder.makeStellarValue(emptyTxSet->getContentsHash(), 2,
                                    xdr::xvector<UpgradeType, 6>({upgrade}),
                                    app->getConfig().NODE_SEED);
        herder.getHerderSCPDriver().valueExternalized(ledgerSeq,
                                                      xdr::xdr_to_opaque(sv));
    }

    txSet = herder.getTransactionQueue().toTxSet({});
    for (auto const& tx : txSet->mTransactions)
    {
        REQUIRE(tx->getEnvelope().type() == ENVELOPE_TYPE_TX);
    }
}

TEST_CASE("upgrade base reserve", "[upgrades]")
{
    VirtualClock clock;
    auto cfg = getTestConfig(0);

    // Do our setup in version 1 so that for_versions_* below do not
    // try to downgrade us from >1 to 1.
    cfg.USE_CONFIG_FOR_GENESIS = false;

    auto app = createTestApplication(clock, cfg);
    app->start();

    auto& lm = app->getLedgerManager();
    auto txFee = lm.getLastTxFee();

    auto const& lcl = lm.getLastClosedLedgerHeader();
    auto txSet = std::make_shared<TxSetFrame>(lcl.hash);

    auto root = TestAccount::createRoot(*app);
    auto issuer = root.create("issuer", lm.getLastMinBalance(0) + 100 * txFee);
    auto native = txtest::makeNativeAsset();
    auto cur1 = issuer.asset("CUR1");
    auto cur2 = issuer.asset("CUR2");

    auto market = TestMarket{*app};

    auto executeUpgrade = [&](uint32_t newReserve) {
        REQUIRE(::executeUpgrade(*app, makeBaseReserveUpgrade(newReserve))
                    .baseReserve == newReserve);
    };

    auto getLiabilities = [&](TestAccount& acc) {
        Liabilities res;
        LedgerTxn ltx(app->getLedgerTxnRoot());
        auto account = stellar::loadAccount(ltx, acc.getPublicKey());
        res.selling = getSellingLiabilities(ltx.loadHeader(), account);
        res.buying = getBuyingLiabilities(ltx.loadHeader(), account);
        return res;
    };
    auto getAssetLiabilities = [&](TestAccount& acc, Asset const& asset) {
        Liabilities res;
        if (acc.hasTrustLine(asset))
        {
            LedgerTxn ltx(app->getLedgerTxnRoot());
            auto trust = stellar::loadTrustLine(ltx, acc.getPublicKey(), asset);
            res.selling = trust.getSellingLiabilities(ltx.loadHeader());
            res.buying = trust.getBuyingLiabilities(ltx.loadHeader());
        }
        return res;
    };
    auto getNumSponsoringEntries = [&](TestAccount& acc) {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        auto account = stellar::loadAccount(ltx, acc.getPublicKey());
        return getNumSponsoring(account.current());
    };
    auto getNumSponsoredEntries = [&](TestAccount& acc) {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        auto account = stellar::loadAccount(ltx, acc.getPublicKey());
        return getNumSponsored(account.current());
    };

    auto createOffer = [&](TestAccount& acc, Asset const& selling,
                           Asset const& buying,
                           std::vector<TestMarketOffer>& offers,
                           OfferState const& afterUpgrade = OfferState::SAME) {
        OfferState state = {selling, buying, Price{2, 1}, 1000};
        auto offer = market.requireChangesWithOffer(
            {}, [&] { return market.addOffer(acc, state); });
        if (afterUpgrade == OfferState::SAME)
        {
            offers.push_back({offer.key, offer.state});
        }
        else
        {
            offers.push_back({offer.key, afterUpgrade});
        }
    };

    auto createOffers = [&](TestAccount& acc,
                            std::vector<TestMarketOffer>& offers,
                            bool expectToDeleteNativeSells = false) {
        OfferState nativeSellState =
            expectToDeleteNativeSells ? OfferState::DELETED : OfferState::SAME;

        createOffer(acc, native, cur1, offers, nativeSellState);
        createOffer(acc, native, cur1, offers, nativeSellState);
        createOffer(acc, cur1, native, offers);
        createOffer(acc, cur1, native, offers);
        createOffer(acc, native, cur2, offers, nativeSellState);
        createOffer(acc, native, cur2, offers, nativeSellState);
        createOffer(acc, cur2, native, offers);
        createOffer(acc, cur2, native, offers);
        createOffer(acc, cur1, cur2, offers);
        createOffer(acc, cur1, cur2, offers);
        createOffer(acc, cur2, cur1, offers);
        createOffer(acc, cur2, cur1, offers);
    };

    auto deleteOffers = [&](TestAccount& acc,
                            std::vector<TestMarketOffer> const& offers) {
        for (auto const& offer : offers)
        {
            auto delOfferState = offer.state;
            delOfferState.amount = 0;
            market.requireChangesWithOffer({}, [&] {
                return market.updateOffer(acc, offer.key.offerID, delOfferState,
                                          OfferState::DELETED);
            });
        }
    };

    SECTION("decrease reserve")
    {
        auto a1 =
            root.create("A", lm.getLastMinBalance(14) + 4000 + 14 * txFee);
        a1.changeTrust(cur1, 12000);
        a1.changeTrust(cur2, 12000);
        issuer.pay(a1, cur1, 4000);
        issuer.pay(a1, cur2, 4000);

        for_versions_to(9, *app, [&] {
            std::vector<TestMarketOffer> offers;
            createOffers(a1, offers);
            uint32_t baseReserve = lm.getLastReserve();
            market.requireChanges(offers,
                                  std::bind(executeUpgrade, baseReserve / 2));
            deleteOffers(a1, offers);
        });
        for_versions_from(10, *app, [&] {
            std::vector<TestMarketOffer> offers;
            createOffers(a1, offers);
            uint32_t baseReserve = lm.getLastReserve();
            market.requireChanges(offers,
                                  std::bind(executeUpgrade, baseReserve / 2));
            REQUIRE(getLiabilities(a1) == Liabilities{8000, 4000});
            REQUIRE(getAssetLiabilities(a1, cur1) == Liabilities{8000, 4000});
            REQUIRE(getAssetLiabilities(a1, cur2) == Liabilities{8000, 4000});
            deleteOffers(a1, offers);
        });
    }

    SECTION("increase reserve")
    {
        for_versions_to(9, *app, [&] {
            auto a1 = root.create("A", 2 * lm.getLastMinBalance(14) + 3999 +
                                           14 * txFee);
            a1.changeTrust(cur1, 12000);
            a1.changeTrust(cur2, 12000);
            issuer.pay(a1, cur1, 4000);
            issuer.pay(a1, cur2, 4000);

            auto a2 = root.create("B", 2 * lm.getLastMinBalance(14) + 4000 +
                                           14 * txFee);
            a2.changeTrust(cur1, 12000);
            a2.changeTrust(cur2, 12000);
            issuer.pay(a2, cur1, 4000);
            issuer.pay(a2, cur2, 4000);

            std::vector<TestMarketOffer> offers;
            createOffers(a1, offers);
            createOffers(a2, offers);

            uint32_t baseReserve = lm.getLastReserve();
            market.requireChanges(offers,
                                  std::bind(executeUpgrade, 2 * baseReserve));
        });

        auto submitTx = [&](TransactionFrameBasePtr tx) {
            LedgerTxn ltx(app->getLedgerTxnRoot());
            TransactionMeta txm(2);
            REQUIRE(tx->checkValid(ltx, 0, 0, 0));
            REQUIRE(tx->apply(*app, ltx, txm));
            ltx.commit();

            REQUIRE(tx->getResultCode() == txSUCCESS);
        };

        auto increaseReserveFromV10 = [&](bool allowMaintainLiablities,
                                          bool flipSponsorship) {
            auto a1 = root.create("A", 2 * lm.getLastMinBalance(14) + 3999 +
                                           14 * txFee);
            a1.changeTrust(cur1, 12000);
            a1.changeTrust(cur2, 12000);
            issuer.pay(a1, cur1, 4000);
            issuer.pay(a1, cur2, 4000);

            auto a2 = root.create("B", 2 * lm.getLastMinBalance(14) + 4000 +
                                           14 * txFee);
            a2.changeTrust(cur1, 12000);
            a2.changeTrust(cur2, 12000);
            issuer.pay(a2, cur1, 4000);
            issuer.pay(a2, cur2, 4000);

            std::vector<TestMarketOffer> offers;
            createOffers(a1, offers, true);
            createOffers(a2, offers);

            if (allowMaintainLiablities)
            {
                issuer.setOptions(txtest::setFlags(
                    static_cast<uint32_t>(AUTH_REQUIRED_FLAG) |
                    static_cast<uint32_t>(AUTH_REVOCABLE_FLAG)));
                issuer.allowMaintainLiabilities(cur1, a1);
            }

            if (flipSponsorship)
            {
                std::vector<Operation> opsA1 = {
                    a1.op(beginSponsoringFutureReserves(a2))};
                std::vector<Operation> opsA2 = {
                    a2.op(beginSponsoringFutureReserves(a1))};
                for (auto const& offer : offers)
                {
                    if (offer.key.sellerID == a2.getPublicKey())
                    {
                        opsA1.emplace_back(a2.op(revokeSponsorship(
                            offerKey(a2, offer.key.offerID))));
                    }
                    else
                    {
                        opsA2.emplace_back(a1.op(revokeSponsorship(
                            offerKey(a1, offer.key.offerID))));
                    }
                }
                opsA1.emplace_back(a2.op(endSponsoringFutureReserves()));
                opsA2.emplace_back(a1.op(endSponsoringFutureReserves()));

                // submit tx to update sponsorship
                submitTx(transactionFrameFromOps(app->getNetworkID(), a1, opsA1,
                                                 {a2}));
                submitTx(transactionFrameFromOps(app->getNetworkID(), a2, opsA2,
                                                 {a1}));
            }

            uint32_t baseReserve = lm.getLastReserve();
            market.requireChanges(offers,
                                  std::bind(executeUpgrade, 2 * baseReserve));
            REQUIRE(getLiabilities(a1) == Liabilities{8000, 0});
            REQUIRE(getAssetLiabilities(a1, cur1) == Liabilities{4000, 4000});
            REQUIRE(getAssetLiabilities(a1, cur2) == Liabilities{4000, 4000});
            REQUIRE(getLiabilities(a2) == Liabilities{8000, 4000});
            REQUIRE(getAssetLiabilities(a2, cur1) == Liabilities{8000, 4000});
            REQUIRE(getAssetLiabilities(a2, cur2) == Liabilities{8000, 4000});
        };

        SECTION("authorized")
        {
            for_versions_from(10, *app,
                              [&] { increaseReserveFromV10(false, false); });
        }

        SECTION("authorized to maintain liabilities")
        {
            for_versions_from(13, *app,
                              [&] { increaseReserveFromV10(true, false); });
        }

        SECTION("sponsorships")
        {
            auto accSponsorsAllOffersTest = [&](TestAccount& sponsoringAcc,
                                                TestAccount& sponsoredAcc,
                                                TestAccount& sponsoredAcc2,
                                                bool sponsoringAccPullOffers,
                                                bool sponsoredAccPullOffers) {
                sponsoringAcc.changeTrust(cur1, 12000);
                sponsoringAcc.changeTrust(cur2, 12000);
                issuer.pay(sponsoringAcc, cur1, 4000);
                issuer.pay(sponsoringAcc, cur2, 4000);

                sponsoredAcc.changeTrust(cur1, 12000);
                sponsoredAcc.changeTrust(cur2, 12000);
                issuer.pay(sponsoredAcc, cur1, 4000);
                issuer.pay(sponsoredAcc, cur2, 4000);

                sponsoredAcc2.changeTrust(cur1, 12000);
                sponsoredAcc2.changeTrust(cur2, 12000);
                issuer.pay(sponsoredAcc2, cur1, 4000);
                issuer.pay(sponsoredAcc2, cur2, 4000);

                std::vector<TestMarketOffer> offers;
                createOffers(sponsoringAcc, offers, sponsoringAccPullOffers);
                createOffers(sponsoredAcc, offers, sponsoredAccPullOffers);
                createOffers(sponsoredAcc2, offers, true);

                // prepare ops to transfer sponsorship of all sponsoredAcc
                // offers and one offer from sponsoredAcc2 to sponsoringAcc
                std::vector<Operation> ops = {
                    sponsoringAcc.op(
                        beginSponsoringFutureReserves(sponsoredAcc)),
                    sponsoringAcc.op(
                        beginSponsoringFutureReserves(sponsoredAcc2))};
                for (auto const& offer : offers)
                {
                    if (offer.key.sellerID == sponsoredAcc.getPublicKey())
                    {
                        ops.emplace_back(sponsoredAcc.op(revokeSponsorship(
                            offerKey(sponsoredAcc, offer.key.offerID))));
                    }
                }

                // last offer in offers is for sponsoredAcc2
                ops.emplace_back(sponsoredAcc2.op(revokeSponsorship(
                    offerKey(sponsoredAcc2, offers.back().key.offerID))));

                ops.emplace_back(
                    sponsoredAcc.op(endSponsoringFutureReserves()));
                ops.emplace_back(
                    sponsoredAcc2.op(endSponsoringFutureReserves()));

                // submit tx to update sponsorship
                submitTx(transactionFrameFromOps(
                    app->getNetworkID(), sponsoringAcc, ops,
                    {sponsoredAcc, sponsoredAcc2}));

                REQUIRE(getNumSponsoredEntries(sponsoredAcc) == 12);
                REQUIRE(getNumSponsoredEntries(sponsoredAcc2) == 1);
                REQUIRE(getNumSponsoringEntries(sponsoringAcc) == 13);

                uint32_t baseReserve = lm.getLastReserve();

                if (sponsoredAccPullOffers)
                {
                    // SponsoringAcc is now sponsoring all 12 of sponsoredAcc's
                    // offers. SponsoredAcc has 4 subentries. It also has enough
                    // lumens to cover 12 more subentries after the sponsorship
                    // update. After the upgrade to double the baseReserve, this
                    // account will need to cover the 4 subEntries, so we only
                    // need 4 extra baseReserves before the upgrade. Pay out the
                    // rest (8 reserves) so we can get our orders pulled on
                    // upgrade. 16(total reserves) - 4(subEntries) -
                    // 4(base reserve increase) = 8(extra base reserves)

                    sponsoredAcc.pay(root, baseReserve * 8);
                }
                else
                {
                    sponsoredAcc.pay(root, baseReserve * 8 - 1);
                }

                if (sponsoringAccPullOffers)
                {
                    sponsoringAcc.pay(root, 1);
                }

                // This account needs to lose a base reserve to get its orders
                // pulled
                sponsoredAcc2.pay(root, baseReserve);

                // execute upgrade
                market.requireChanges(
                    offers, std::bind(executeUpgrade, 2 * baseReserve));

                if (sponsoredAccPullOffers)
                {
                    REQUIRE(getLiabilities(sponsoredAcc) ==
                            Liabilities{8000, 0});
                    REQUIRE(getAssetLiabilities(sponsoredAcc, cur1) ==
                            Liabilities{4000, 4000});
                    REQUIRE(getAssetLiabilities(sponsoredAcc, cur2) ==
                            Liabilities{4000, 4000});

                    // the 4 native offers were pulled
                    REQUIRE(getNumSponsoredEntries(sponsoredAcc) == 8);
                    REQUIRE(getNumSponsoringEntries(sponsoringAcc) == 9);
                }
                else
                {
                    REQUIRE(getLiabilities(sponsoredAcc) ==
                            Liabilities{8000, 4000});
                    REQUIRE(getAssetLiabilities(sponsoredAcc, cur1) ==
                            Liabilities{8000, 4000});
                    REQUIRE(getAssetLiabilities(sponsoredAcc, cur2) ==
                            Liabilities{8000, 4000});

                    REQUIRE(getNumSponsoredEntries(sponsoredAcc) == 12);
                    REQUIRE(getNumSponsoringEntries(sponsoringAcc) == 13);
                }

                if (sponsoringAccPullOffers)
                {
                    REQUIRE(getLiabilities(sponsoringAcc) ==
                            Liabilities{8000, 0});
                    REQUIRE(getAssetLiabilities(sponsoringAcc, cur1) ==
                            Liabilities{4000, 4000});
                    REQUIRE(getAssetLiabilities(sponsoringAcc, cur2) ==
                            Liabilities{4000, 4000});
                }
                else
                {
                    REQUIRE(getLiabilities(sponsoringAcc) ==
                            Liabilities{8000, 4000});
                    REQUIRE(getAssetLiabilities(sponsoringAcc, cur1) ==
                            Liabilities{8000, 4000});
                    REQUIRE(getAssetLiabilities(sponsoringAcc, cur2) ==
                            Liabilities{8000, 4000});
                }

                REQUIRE(getLiabilities(sponsoredAcc2) == Liabilities{8000, 0});
                REQUIRE(getAssetLiabilities(sponsoredAcc2, cur1) ==
                        Liabilities{4000, 4000});
                REQUIRE(getAssetLiabilities(sponsoredAcc2, cur2) ==
                        Liabilities{4000, 4000});
            };

            auto sponsorshipTestsBySeed = [&](std::string sponsoringSeed,
                                              std::string sponsoredSeed) {
                auto sponsoring =
                    root.create(sponsoringSeed, 2 * lm.getLastMinBalance(27) +
                                                    4000 + 15 * txFee);

                auto sponsored =
                    root.create(sponsoredSeed,
                                lm.getLastMinBalance(14) + 3999 + 15 * txFee);

                // This account will have one sponsored offer and will always
                // have it's offers pulled.
                auto sponsored2 = root.create(
                    "C", 2 * lm.getLastMinBalance(13) + 3999 + 15 * txFee);

                SECTION("sponsored and sponsoring accounts get offers "
                        "pulled on upgrade")
                {
                    accSponsorsAllOffersTest(sponsoring, sponsored, sponsored2,
                                             true, true);
                }
                SECTION("no offers pulled")
                {
                    accSponsorsAllOffersTest(sponsoring, sponsored, sponsored2,
                                             false, false);
                }
                SECTION("offers for sponsored account pulled")
                {
                    accSponsorsAllOffersTest(sponsoring, sponsored, sponsored2,
                                             true, false);
                }
                SECTION("offers for sponsoring account pulled")
                {
                    accSponsorsAllOffersTest(sponsoring, sponsored, sponsored2,
                                             false, true);
                }
            };

            for_versions_from(14, *app, [&] {
                // Swap the seeds to test that the ordering of accounts doesn't
                // matter when upgrading
                SECTION("account A is sponsored")
                {
                    sponsorshipTestsBySeed("B", "A");
                }
                SECTION("account B is sponsored")
                {
                    sponsorshipTestsBySeed("A", "B");
                }
                SECTION("swap sponsorship of orders")
                {
                    increaseReserveFromV10(false, true);
                }
            });
        }
    }
}

TEST_CASE("simulate upgrades", "[herder][upgrades][acceptance]")
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
            {genesis(0, 28), {upgrade, upgrade, upgrade}}};
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
            {genesis(0, 37), {upgrade, upgrade, upgrade}}};
        simulateUpgrade(nodes, checks);
    }

    SECTION("2 of 3 vote early (v-blocking) - 3 upgrade anyways")
    {
        auto nodes = std::vector<LedgerUpgradeNode>{{upgrade, genesis(0, 10)},
                                                    {upgrade, genesis(0, 10)},
                                                    {upgrade, genesis(0, 30)}};
        auto checks = std::vector<LedgerUpgradeCheck>{
            {genesis(0, 9), {noUpgrade, noUpgrade, noUpgrade}},
            {genesis(0, 27), {upgrade, upgrade, upgrade}}};
        simulateUpgrade(nodes, checks);
    }
}

TEST_CASE("upgrade invalid during ledger close", "[upgrades]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    app->start();

    // Version upgrade to unsupported
    REQUIRE_THROWS(
        executeUpgrade(*app, makeProtocolVersionUpgrade(
                                 Config::CURRENT_LEDGER_PROTOCOL_VERSION + 1)));

    // Version downgrade
    REQUIRE_THROWS(
        executeUpgrade(*app, makeProtocolVersionUpgrade(
                                 Config::CURRENT_LEDGER_PROTOCOL_VERSION - 1)));

    // Base Fee / Base Reserve to 0
    REQUIRE_THROWS(executeUpgrade(*app, makeBaseFeeUpgrade(0)));
    REQUIRE_THROWS(executeUpgrade(*app, makeBaseReserveUpgrade(0)));
}

TEST_CASE("validate upgrade expiration logic", "[upgrades]")
{
    auto cfg = getTestConfig();
    cfg.LEDGER_PROTOCOL_VERSION = 10;
    cfg.TESTING_UPGRADE_DESIRED_FEE = 100;
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 50;
    cfg.TESTING_UPGRADE_RESERVE = 100000000;
    cfg.TESTING_UPGRADE_DATETIME = genesis(0, 0);

    auto header = LedgerHeader{};

    // make sure the network info is different than what's armed
    header.ledgerVersion = cfg.LEDGER_PROTOCOL_VERSION - 1;
    header.baseFee = cfg.TESTING_UPGRADE_DESIRED_FEE - 1;
    header.baseReserve = cfg.TESTING_UPGRADE_RESERVE - 1;
    header.maxTxSetSize = cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE - 1;

    SECTION("remove expired upgrades")
    {
        header.scpValue.closeTime = VirtualClock::to_time_t(
            cfg.TESTING_UPGRADE_DATETIME + Upgrades::UPDGRADE_EXPIRATION_HOURS);

        bool updated = false;
        auto upgrades = Upgrades{cfg}.removeUpgrades(
            header.scpValue.upgrades.begin(), header.scpValue.upgrades.end(),
            header.scpValue.closeTime, updated);

        REQUIRE(updated);
        REQUIRE(!upgrades.mProtocolVersion);
        REQUIRE(!upgrades.mBaseFee);
        REQUIRE(!upgrades.mMaxTxSize);
        REQUIRE(!upgrades.mBaseReserve);
    }

    SECTION("upgrades not yet expired")
    {
        header.scpValue.closeTime = VirtualClock::to_time_t(
            cfg.TESTING_UPGRADE_DATETIME + Upgrades::UPDGRADE_EXPIRATION_HOURS -
            std::chrono::seconds(1));

        bool updated = false;
        auto upgrades = Upgrades{cfg}.removeUpgrades(
            header.scpValue.upgrades.begin(), header.scpValue.upgrades.end(),
            header.scpValue.closeTime, updated);

        REQUIRE(!updated);
        REQUIRE(upgrades.mProtocolVersion);
        REQUIRE(upgrades.mBaseFee);
        REQUIRE(upgrades.mMaxTxSize);
        REQUIRE(upgrades.mBaseReserve);
    }
}
