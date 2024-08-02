// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "TestUtils.h"
#include "ledger/LedgerTxn.h"
#include "overlay/test/LoopbackPeer.h"
#include "simulation/LoadGenerator.h"
#include "simulation/Simulation.h"
#include "src/transactions/test/SorobanTxTestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "util/StatusManager.h"
#include "work/WorkScheduler.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-ledger.h"
#include "xdr/Stellar-transaction.h"
#include "xdrpp/printer.h"
#include <iostream>
#include <lib/catch.hpp>
namespace stellar
{

namespace testutil
{

void
crankSome(VirtualClock& clock)
{
    auto start = clock.now();
    for (size_t i = 0;
         (i < 100 && clock.now() < (start + std::chrono::seconds(1)) &&
          clock.crank(false) > 0);
         ++i)
        ;
}

void
crankFor(VirtualClock& clock, VirtualClock::duration duration)
{
    auto start = clock.now();
    while (clock.now() < (start + duration) && clock.crank(false) > 0)
        ;
}

void
shutdownWorkScheduler(Application& app)
{
    if (app.getClock().getIOContext().stopped())
    {
        throw std::runtime_error("Work scheduler attempted to shutdown after "
                                 "VirtualClock io context stopped.");
    }
    app.getWorkScheduler().shutdown();
    while (app.getWorkScheduler().getState() != BasicWork::State::WORK_ABORTED)
    {
        app.getClock().crank();
    }
}

void
injectSendPeersAndReschedule(VirtualClock::time_point& end, VirtualClock& clock,
                             VirtualTimer& timer,
                             LoopbackPeerConnection& connection)
{
    connection.getInitiator()->sendGetPeers();
    if (clock.now() < end && connection.getInitiator()->isConnectedForTesting())
    {
        timer.expires_from_now(std::chrono::milliseconds(10));
        timer.async_wait(
            [&]() {
                injectSendPeersAndReschedule(end, clock, timer, connection);
            },
            &VirtualTimer::onFailureNoop);
    }
}

std::vector<Asset>
getInvalidAssets(SecretKey const& issuer)
{
    std::vector<Asset> assets;

    // control char in asset name
    assets.emplace_back(txtest::makeAsset(issuer, "\n"));

    // non-trailing zero in asset name
    assets.emplace_back(txtest::makeAsset(issuer, "\0a"));

    // zero asset name
    assets.emplace_back(txtest::makeAsset(issuer, "\0"));

    // start right after z(122), and go through some of the
    // extended ascii codes
    for (int v = 123; v < 140; ++v)
    {
        std::string assetCode;
        signed char i = static_cast<signed char>((v < 128) ? v : (127 - v));
        assetCode.push_back(i);
        assets.emplace_back(txtest::makeAsset(issuer, assetCode));
    }

    {
        // AssetCode12 with less than 5 chars
        Asset asset;
        asset.type(ASSET_TYPE_CREDIT_ALPHANUM12);
        asset.alphaNum12().issuer = issuer.getPublicKey();
        strToAssetCode(asset.alphaNum12().assetCode, "aaaa");
        assets.emplace_back(asset);
    }

    return assets;
}

int32_t
computeMultiplier(LedgerEntry const& le)
{
    switch (le.data.type())
    {
    case ACCOUNT:
        return 2;
    case TRUSTLINE:
        return le.data.trustLine().asset.type() == ASSET_TYPE_POOL_SHARE ? 2
                                                                         : 1;
    case OFFER:
    case DATA:
        return 1;
    case CLAIMABLE_BALANCE:
        return static_cast<uint32_t>(
            le.data.claimableBalance().claimants.size());
    case CONFIG_SETTING:
    case CONTRACT_DATA:
    case CONTRACT_CODE:
    case TTL:
    default:
        throw std::runtime_error("Unexpected LedgerEntry type");
    }
}

BucketListDepthModifier::BucketListDepthModifier(uint32_t newDepth)
    : mPrevDepth(BucketList::kNumLevels)
{
    BucketList::kNumLevels = newDepth;
}

BucketListDepthModifier::~BucketListDepthModifier()
{
    BucketList::kNumLevels = mPrevDepth;
}
}

TestInvariantManager::TestInvariantManager(medida::MetricsRegistry& registry)
    : InvariantManagerImpl(registry)
{
}

void
TestInvariantManager::handleInvariantFailure(
    std::shared_ptr<Invariant> invariant, std::string const& message) const
{
    CLOG_DEBUG(Invariant, "{}", message);
    throw InvariantDoesNotHold{message};
}

TestApplication::TestApplication(VirtualClock& clock, Config const& cfg)
    : ApplicationImpl(clock, cfg)
{
}

std::unique_ptr<InvariantManager>
TestApplication::createInvariantManager()
{
    return std::make_unique<TestInvariantManager>(getMetrics());
}

TimePoint
getTestDate(int day, int month, int year)
{
    auto tm = getTestDateTime(day, month, year, 0, 0, 0);

    VirtualClock::system_time_point tp = VirtualClock::tmToSystemPoint(tm);
    TimePoint t = VirtualClock::to_time_t(tp);

    return t;
}

std::tm
getTestDateTime(int day, int month, int year, int hour, int minute, int second)
{
    std::tm tm = {0};
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    tm.tm_mday = day;
    tm.tm_mon = month - 1; // 0 based
    tm.tm_year = year - 1900;
    return tm;
}

VirtualClock::system_time_point
genesis(int minute, int second)
{
    return VirtualClock::tmToSystemPoint(
        getTestDateTime(1, 7, 2014, 0, minute, second));
}

ConfigUpgradeSetKey
upgradeSorobanNetworkConfig(std::function<void(SorobanNetworkConfig&)> modifyFn,
                            Simulation* simulation)
{
    auto nodes = simulation->getNodes();
    auto& lg = nodes[0]->getLoadGenerator();
    auto& app = *nodes[0];

    lg.generateLoad(GeneratedLoadConfig::createAccountsLoad(1, 1));
    auto complete = app.getMetrics()
                        .NewMeter({"loadgen", "run", "complete"}, "run")
                        .count();
    simulation->crankUntil(
        [&]() {
            return app.getMetrics()
                       .NewMeter({"loadgen", "run", "complete"}, "run")
                       .count() == complete + 1;
        },
        300 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);
    complete++;

    auto lgcfg = GeneratedLoadConfig::createSorobanUpgradeSetupLoad();
    releaseAssert(lgcfg.getSorobanConfig().nInstances == 1);
    // Create upload wasm transaction
    lg.generateLoad(lgcfg);
    simulation->crankUntil(
        [&]() {
            return app.getMetrics()
                       .NewMeter({"loadgen", "run", "complete"}, "run")
                       .count() == complete + 1;
        },
        300 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);
    complete++;

    auto createUpgradeLoadGenConfig = GeneratedLoadConfig::txLoad(
        LoadGenMode::SOROBAN_CREATE_UPGRADE, 1, 1, 1);
    auto& upgradeCfg = createUpgradeLoadGenConfig.getMutSorobanUpgradeConfig();
    SorobanNetworkConfig cfg;
    modifyFn(cfg);
    GeneratedLoadConfig::copySorobanNetworkConfigToUpgradeConfig(cfg,
                                                                 upgradeCfg);
    auto upgradeSetKey = lg.getConfigUpgradeSetKey(createUpgradeLoadGenConfig);
    lg.generateLoad(createUpgradeLoadGenConfig);
    simulation->crankUntil(
        [&]() {
            return app.getMetrics()
                       .NewMeter({"loadgen", "run", "complete"}, "run")
                       .count() == complete + 1;
        },
        300 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);
    complete++;
    ConfigUpgradeSet upgrades;
    {
        LedgerTxn ltx(app.getLedgerTxnRoot());
        auto lk = ConfigUpgradeSetFrame::getLedgerKey(upgradeSetKey);
        auto upgradeSet = ltx.load(lk);
        REQUIRE(upgradeSet);
        xdr::xdr_from_opaque(upgradeSet.current().data.contractData().val.bytes(),
                             upgrades);
       
        CLOG_TRACE(LoadGen, "READ upgrade set from ltx");
    }

    CLOG_TRACE(LoadGen, "upgrades = {} ", xdr::xdr_to_string(upgrades));
    // Arm for upgrade.
    for (auto app : nodes)
    {
        Upgrades::UpgradeParameters scheduledUpgrades;
        auto lclHeader =
            app->getLedgerManager().getLastClosedLedgerHeader().header;
        scheduledUpgrades.mUpgradeTime =
            VirtualClock::from_time_t(lclHeader.scpValue.closeTime);
        // scheduledUpgrades.mUpgradeTime = app->getClock().system_now() +
        // std::chrono::seconds(1);
        CLOG_TRACE(LoadGen, "mUpgradeTime {}",
                   scheduledUpgrades.mUpgradeTime.time_since_epoch().count());
        CLOG_TRACE(LoadGen, "curr time {}",
                   app->getClock().now().time_since_epoch().count());

        scheduledUpgrades.mConfigUpgradeSetKey = upgradeSetKey;
        app->getHerder().setUpgrades(scheduledUpgrades);
    }
    // Wait for upgrade to be applied
    simulation->crankUntil(
        [&]() {
            auto status = app.getStatusManager().getStatusMessage(
                StatusCategory::REQUIRES_UPGRADES);
            CLOG_TRACE(LoadGen, "status {}", status);
            return status.empty();
        },
        300 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);
    return upgradeSetKey;
}

void
modifySorobanNetworkConfig(Application& app,
                           std::function<void(SorobanNetworkConfig&)> modifyFn)
{
    if (!modifyFn)
    {
        return;
    }
    LedgerTxn ltx(app.getLedgerTxnRoot());
    app.getLedgerManager().updateNetworkConfig(ltx);
    auto& cfg = app.getLedgerManager().getMutableSorobanNetworkConfig();
    modifyFn(cfg);
    cfg.writeAllSettings(ltx, app);
    ltx.commit();

    // Need to close a ledger following call to `addBatch` from config upgrade
    if (app.getConfig().isUsingBucketListDB())
    {
        txtest::closeLedger(app);
    }
}

void
overrideSorobanNetworkConfigForTest(Application& app)
{
    modifySorobanNetworkConfig(app, [](SorobanNetworkConfig& cfg) {
        cfg.mMaxContractSizeBytes = 64 * 1024;
        cfg.mMaxContractDataEntrySizeBytes = 64 * 1024;

        cfg.mTxMaxSizeBytes = 100 * 1024;
        cfg.mLedgerMaxTransactionsSizeBytes = cfg.mTxMaxSizeBytes * 10;

        cfg.mTxMaxInstructions = 100'000'000;
        cfg.mLedgerMaxInstructions = cfg.mTxMaxInstructions * 10;
        cfg.mTxMemoryLimit = 100 * 1024 * 1024;

        cfg.mTxMaxReadLedgerEntries = 40;
        cfg.mTxMaxReadBytes = 200 * 1024;

        cfg.mTxMaxWriteLedgerEntries = 20;
        cfg.mTxMaxWriteBytes = 100 * 1024;

        cfg.mLedgerMaxReadLedgerEntries = cfg.mTxMaxReadLedgerEntries * 10;
        cfg.mLedgerMaxReadBytes = cfg.mTxMaxReadBytes * 10;
        cfg.mLedgerMaxWriteLedgerEntries = cfg.mTxMaxWriteLedgerEntries * 10;
        cfg.mLedgerMaxWriteBytes = cfg.mTxMaxWriteBytes * 10;

        cfg.mStateArchivalSettings.minPersistentTTL = 20;
        cfg.mStateArchivalSettings.maxEntryTTL = 6'312'000;
        cfg.mLedgerMaxTxCount = 100;

        cfg.mTxMaxContractEventsSizeBytes = 10'000;
    });
}

bool
appProtocolVersionStartsFrom(Application& app, ProtocolVersion fromVersion)
{
    LedgerTxn ltx(app.getLedgerTxnRoot());
    auto ledgerVersion = ltx.loadHeader().current().ledgerVersion;

    return protocolVersionStartsFrom(ledgerVersion, fromVersion);
}
}
