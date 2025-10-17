// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "TestUtils.h"
#include "herder/TxSetFrame.h"
#include "ledger/LedgerStateSnapshot.h"
#include "ledger/test/LedgerTestUtils.h"
#include "simulation/LoadGenerator.h"
#include "simulation/Simulation.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/test/SorobanTxTestUtils.h"
#include "util/ProtocolVersion.h"
#include "work/WorkScheduler.h"
#include "xdrpp/marshal.h"
#include <limits>

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
crankUntil(Application::pointer app, std::function<bool()> const& predicate,
           VirtualClock::duration timeout)
{
    crankUntil(*app, predicate, timeout);
}

void
crankUntil(Application& app, std::function<bool()> const& predicate,
           VirtualClock::duration timeout)
{
    auto start = std::chrono::system_clock::now();
    while (!predicate())
    {
        app.getClock().crank(false);
        auto current = std::chrono::system_clock::now();
        auto diff = current - start;
        if (diff > timeout)
        {
            break;
        }
    }
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

template <class BucketT>
BucketListDepthModifier<BucketT>::BucketListDepthModifier(uint32_t newDepth)
    : mPrevDepth(BucketListBase<BucketT>::kNumLevels)
{
    BucketListBase<BucketT>::kNumLevels = newDepth;
}

template <class BucketT>
BucketListDepthModifier<BucketT>::~BucketListDepthModifier()
{
    BucketListBase<BucketT>::kNumLevels = mPrevDepth;
}

template class BucketListDepthModifier<LiveBucket>;
template class BucketListDepthModifier<HotArchiveBucket>;
}

TestInvariantManager::TestInvariantManager(medida::MetricsRegistry& registry)
    : InvariantManagerImpl(registry)
{
}

void
TestInvariantManager::handleInvariantFailure(bool isStrict,
                                             std::string const& message) const
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

void
upgradeSorobanNetworkConfig(std::function<void(SorobanNetworkConfig&)> modifyFn,
                            std::shared_ptr<Simulation> simulation,
                            bool applyUpgrade)
{
    auto nodes = simulation->getNodes();
    auto& lg = nodes[0]->getLoadGenerator();
    auto& app = *nodes[0];

    auto& complete =
        app.getMetrics().NewMeter({"loadgen", "run", "complete"}, "run");
    auto completeCount = complete.count();

    // Only create an account if upgrade has not ran before.
    if (!simulation->isSetUpForSorobanUpgrade())
    {
        // Create upload wasm transaction using root account.
        auto createUploadCfg =
            GeneratedLoadConfig::createSorobanUpgradeSetupLoad();
        lg.generateLoad(createUploadCfg);
        completeCount = complete.count();
        simulation->crankUntil(
            [&]() { return complete.count() == completeCount + 1; },
            300 * simulation->getExpectedLedgerCloseTime(), false);

        simulation->markReadyForSorobanUpgrade();
    }

    // Create upgrade transaction using root account.
    auto createUpgradeLoadGenConfig = GeneratedLoadConfig::txLoad(
        LoadGenMode::SOROBAN_CREATE_UPGRADE, 1, 1, 1);
    // Get current network config.
    auto cfg = nodes[0]->getLedgerManager().getLastClosedSorobanNetworkConfig();
    modifyFn(cfg);
    createUpgradeLoadGenConfig.copySorobanNetworkConfigToUpgradeConfig(
        nodes[0]->getLedgerManager().getLastClosedSorobanNetworkConfig(), cfg);
    auto upgradeSetKey = lg.getConfigUpgradeSetKey(
        createUpgradeLoadGenConfig.getSorobanUpgradeConfig());
    lg.generateLoad(createUpgradeLoadGenConfig);
    completeCount = complete.count();
    simulation->crankUntil(
        [&]() { return complete.count() == completeCount + 1; },
        4 * simulation->getExpectedLedgerCloseTime(), false);

    // Arm for upgrade.
    for (auto app : nodes)
    {
        Upgrades::UpgradeParameters scheduledUpgrades;
        auto lclHeader =
            app->getLedgerManager().getLastClosedLedgerHeader().header;
        scheduledUpgrades.mUpgradeTime =
            VirtualClock::from_time_t(lclHeader.scpValue.closeTime);
        scheduledUpgrades.mConfigUpgradeSetKey = upgradeSetKey;
        app->getHerder().setUpgrades(scheduledUpgrades);
    }

    if (applyUpgrade)
    {
        // Wait for upgrade to be applied
        simulation->crankUntil(
            [&]() {
                return std::all_of(
                    nodes.begin(), nodes.end(), [&](auto const& node) {
                        return node->getLedgerManager()
                                   .getLastClosedSorobanNetworkConfig() == cfg;
                    });
            },
            2 * simulation->getExpectedLedgerCloseTime(), false);
    }
}

// This will go through the full process of upgrading the network config.
// This includes:
// 1. Deploying the upgrade contract wasm
// 2. Creating the upgrade contract instance
// 3. Creating the upgrade ContractData entry
// 4. Arming for the upgrade
// Note that the armed ledger will not be closed.
std::pair<SorobanNetworkConfig, UpgradeType>
prepareSorobanNetworkConfigUpgrade(
    Application& app, std::function<void(SorobanNetworkConfig&)> modifyFn)
{
    releaseAssertOrThrow(modifyFn);

    TxGenerator txGenerator(app);
    auto root = app.getRoot();

    auto closeWithTx = [&](TransactionFrameBaseConstPtr tx) {
        auto res = txtest::closeLedgerOn(
            app, app.getLedgerManager().getLastClosedLedgerNum() + 1, 2, 1,
            2016, {tx});
        root->loadSequenceNumber();
    };

    auto wasm = rust_bridge::get_write_bytes();
    xdr::opaque_vec<> wasmBytes;
    wasmBytes.assign(wasm.data.begin(), wasm.data.end());

    LedgerKey contractCodeLedgerKey;
    contractCodeLedgerKey.type(CONTRACT_CODE);
    contractCodeLedgerKey.contractCode().hash = sha256(wasmBytes);
    auto instanceSalt = sha256("upgrade");

    auto contractIDPreimage =
        txtest::makeContractIDPreimage(*root, instanceSalt);
    auto contractID = xdrSha256(txtest::makeFullContractIdPreimage(
        app.getNetworkID(), contractIDPreimage));
    auto instanceLk = txtest::makeContractInstanceKey(
        txtest::makeContractAddress(contractID));

    // Step 1: Create upload wasm transaction.
    auto createUploadWasmTxnPair = txGenerator.createUploadWasmTransaction(
        app.getLedgerManager().getLastClosedLedgerNum(),
        TxGenerator::ROOT_ACCOUNT_ID, wasmBytes, contractCodeLedgerKey,
        std::nullopt);
    closeWithTx(createUploadWasmTxnPair.second);

    bool instanceExists = false;
    {
        // Step 1: Check if instance already exists.
        LedgerTxn ltx(app.getLedgerTxnRoot());
        if (ltx.load(instanceLk))
        {
            instanceExists = true;
        }
    }

    if (!instanceExists)
    {
        // Step 2: Create instance txn
        auto contractOverhead = 160 + wasmBytes.size();
        auto instanceTxPair = txGenerator.createContractTransaction(
            app.getLedgerManager().getLastClosedLedgerNum(),
            TxGenerator::ROOT_ACCOUNT_ID, contractCodeLedgerKey,
            contractOverhead, instanceSalt, std::nullopt);
        closeWithTx(instanceTxPair.second);
    }

    // Step 3: Create upgrade transaction.
    auto createUpgradeLoadGenConfig = GeneratedLoadConfig::txLoad(
        LoadGenMode::SOROBAN_CREATE_UPGRADE, 1, 1, 1);
    auto upgradeCfg =
        app.getLedgerManager().getLastClosedSorobanNetworkConfig();
    modifyFn(upgradeCfg);
    createUpgradeLoadGenConfig.copySorobanNetworkConfigToUpgradeConfig(
        app.getLedgerManager().getLastClosedSorobanNetworkConfig(), upgradeCfg);

    auto sorobanUpgradeCfg =
        createUpgradeLoadGenConfig.getSorobanUpgradeConfig();
    auto upgradeBytes =
        txGenerator.getConfigUpgradeSetFromLoadConfig(sorobanUpgradeCfg);
    auto txPair = txGenerator.invokeSorobanCreateUpgradeTransaction(
        app.getLedgerManager().getLastClosedLedgerNum(),
        TxGenerator::ROOT_ACCOUNT_ID, upgradeBytes, contractCodeLedgerKey,
        instanceLk, std::nullopt);
    closeWithTx(txPair.second);

    // Step 4: Arm for upgrade.
    auto lclHeader = app.getLedgerManager().getLastClosedLedgerHeader();

    auto upgradeSetKey =
        txGenerator.getConfigUpgradeSetKey(sorobanUpgradeCfg, contractID);
    ConfigUpgradeSet configUpgradeSet;
    xdr::xdr_from_opaque(upgradeBytes, configUpgradeSet);

    Upgrades::UpgradeParameters scheduledUpgrades;
    scheduledUpgrades.mUpgradeTime =
        VirtualClock::from_time_t(lclHeader.header.scpValue.closeTime + 1);
    scheduledUpgrades.mConfigUpgradeSetKey = upgradeSetKey;
    app.getHerder().setUpgrades(scheduledUpgrades);

    auto configSetFrame = std::make_shared<ConfigUpgradeSetFrame>(
        configUpgradeSet, upgradeSetKey, lclHeader.header.ledgerVersion);
    auto ledgerUpgrade = txtest::makeConfigUpgrade(*configSetFrame);
    return {upgradeCfg, LedgerTestUtils::toUpgradeType(ledgerUpgrade)};
}

// This will go through the full process of upgrading the network config.
// This includes:
// 1. Deploying the upgrade contract wasm
// 2. Creating the upgrade contract instance
// 3. Creating the upgrade ContractData entry
// 4. Arming for the upgrade
// 5. Closing the ledger for which the upgrade is armed
void
modifySorobanNetworkConfig(Application& app,
                           std::function<void(SorobanNetworkConfig&)> modifyFn)
{
    if (!modifyFn)
    {
        return;
    }

    auto [upgradeCfg, upgrade] =
        prepareSorobanNetworkConfigUpgrade(app, modifyFn);

    auto lclHeader = app.getLedgerManager().getLastClosedLedgerHeader();
    TimePoint closeTime = lclHeader.header.scpValue.closeTime + 1;

    app.getHerder().externalizeValue(TxSetXDRFrame::makeEmpty(lclHeader),
                                     lclHeader.header.ledgerSeq + 1, closeTime,
                                     {upgrade});
    app.getRoot()->loadSequenceNumber();

    // Check that the upgrade was actually applied.
    auto postUpgradeCfg =
        app.getLedgerManager().getLastClosedSorobanNetworkConfig();
    releaseAssertOrThrow(postUpgradeCfg == upgradeCfg);
}

void
setSorobanNetworkConfigForTest(SorobanNetworkConfig& cfg,
                               std::optional<uint32_t> ledgerVersion)
{
    cfg.mMaxContractSizeBytes = 64 * 1024;
    cfg.mMaxContractDataEntrySizeBytes = 64 * 1024;

    cfg.mTxMaxSizeBytes = 100 * 1024;
    cfg.mLedgerMaxTransactionsSizeBytes = cfg.mTxMaxSizeBytes * 10;

    cfg.mTxMaxInstructions = 100'000'000;
    cfg.mLedgerMaxInstructions = cfg.mTxMaxInstructions * 10;
    cfg.mTxMemoryLimit = 100 * 1024 * 1024;

    cfg.mTxMaxDiskReadEntries = 40;
    cfg.mTxMaxDiskReadBytes = 200 * 1024;

    cfg.mTxMaxWriteLedgerEntries = 20;
    cfg.mTxMaxWriteBytes = 100 * 1024;

    cfg.mLedgerMaxDiskReadEntries = cfg.mTxMaxDiskReadEntries * 10;
    cfg.mLedgerMaxDiskReadBytes = cfg.mTxMaxDiskReadBytes * 10;
    cfg.mLedgerMaxWriteLedgerEntries = cfg.mTxMaxWriteLedgerEntries * 10;
    cfg.mLedgerMaxWriteBytes = cfg.mTxMaxWriteBytes * 10;

    cfg.mStateArchivalSettings.minPersistentTTL = 20;
    cfg.mStateArchivalSettings.maxEntryTTL = 6'312'000;
    cfg.mLedgerMaxTxCount = 100;

    cfg.mTxMaxContractEventsSizeBytes = 10'000;

    if (!ledgerVersion ||
        protocolVersionStartsFrom(*ledgerVersion, ProtocolVersion::V_23))
    {
        cfg.mTxMaxFootprintEntries = cfg.mTxMaxDiskReadEntries;
    }
}

void
overrideSorobanNetworkConfigForTest(Application& app)
{
    modifySorobanNetworkConfig(app, [&app](SorobanNetworkConfig& cfg) {
        setSorobanNetworkConfigForTest(cfg, app.getLedgerManager()
                                                .getLastClosedLedgerHeader()
                                                .header.ledgerVersion);
    });
}

bool
appProtocolVersionStartsFrom(Application& app, ProtocolVersion fromVersion)
{
    LedgerTxn ltx(app.getLedgerTxnRoot());
    auto ledgerVersion = ltx.loadHeader().current().ledgerVersion;

    return protocolVersionStartsFrom(ledgerVersion, fromVersion);
}

void
generateTransactions(Application& app, std::filesystem::path const& outputFile,
                     uint32_t numTransactions, uint32_t accounts,
                     uint32_t offset)
{
    // Create a TxGenerator for generating payment transactions
    TxGenerator txgen(app);

    // Open the output file for writing
    std::remove(outputFile.string().c_str());
    XDROutputFileStream out(app.getClock().getIOContext(), true);
    out.open(outputFile.string());

    if (accounts == 0)
    {
        throw std::runtime_error("Number of accounts must be greater than 0");
    }

    LOG_INFO(DEFAULT_LOG,
             "Generating {} payment transactions using {} accounts with offset "
             "{}...",
             numTransactions, accounts, offset);

    // Loop through accounts to create payment transactions
    for (uint32_t i = 0; i < numTransactions; i++)
    {
        uint64_t sourceAccountId = (i % accounts) + offset;

        // Create a payment transaction
        auto [account, tx] = txgen.paymentTransaction(
            accounts, offset, 0, sourceAccountId, 1, std::nullopt);

        // Convert to TransactionEnvelope and write to output
        TransactionEnvelope txEnv = tx->getEnvelope();
        out.writeOne(txEnv);
    }

    out.close();
    LOG_INFO(DEFAULT_LOG, "Generated {} transactions in {}", numTransactions,
             outputFile);
}
}
