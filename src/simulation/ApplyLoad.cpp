#include "simulation/ApplyLoad.h"

#include <numeric>

#include "bucket/test/BucketTestUtils.h"
#include "herder/Herder.h"
#include "ledger/LedgerManager.h"
#include "test/TxTests.h"
#include "transactions/MutableTransactionResult.h"
#include "transactions/TransactionBridge.h"
#include "transactions/TransactionUtils.h"

#include "herder/HerderImpl.h"

#include "medida/meter.h"
#include "medida/metrics_registry.h"

#include "bucket/BucketListSnapshotBase.h"
#include "bucket/BucketSnapshotManager.h"
#include "util/GlobalChecks.h"
#include "util/XDRCereal.h"
#include "xdrpp/printer.h"
#include <crypto/SHA.h>

namespace stellar
{
namespace
{
SorobanUpgradeConfig
getUpgradeConfig(Config const& cfg)
{
    SorobanUpgradeConfig upgradeConfig;
    upgradeConfig.maxContractSizeBytes = 65536;
    upgradeConfig.maxContractDataKeySizeBytes = 250;
    upgradeConfig.maxContractDataEntrySizeBytes = 65536;
    upgradeConfig.ledgerMaxInstructions =
        cfg.APPLY_LOAD_LEDGER_MAX_INSTRUCTIONS;
    upgradeConfig.txMaxInstructions = cfg.APPLY_LOAD_TX_MAX_INSTRUCTIONS;
    upgradeConfig.txMemoryLimit = 41943040;
    upgradeConfig.ledgerMaxDiskReadEntries =
        cfg.APPLY_LOAD_LEDGER_MAX_READ_LEDGER_ENTRIES;
    upgradeConfig.ledgerMaxDiskReadBytes = cfg.APPLY_LOAD_LEDGER_MAX_READ_BYTES;
    upgradeConfig.ledgerMaxWriteLedgerEntries =
        cfg.APPLY_LOAD_LEDGER_MAX_WRITE_LEDGER_ENTRIES;
    upgradeConfig.ledgerMaxWriteBytes = cfg.APPLY_LOAD_LEDGER_MAX_WRITE_BYTES;
    upgradeConfig.ledgerMaxTxCount = cfg.APPLY_LOAD_MAX_TX_COUNT;
    upgradeConfig.txMaxDiskReadEntries =
        cfg.APPLY_LOAD_TX_MAX_READ_LEDGER_ENTRIES;
    upgradeConfig.txMaxFootprintEntries =
        cfg.APPLY_LOAD_TX_MAX_READ_LEDGER_ENTRIES;
    upgradeConfig.txMaxDiskReadBytes = cfg.APPLY_LOAD_TX_MAX_READ_BYTES;
    upgradeConfig.txMaxWriteLedgerEntries =
        cfg.APPLY_LOAD_TX_MAX_WRITE_LEDGER_ENTRIES;
    upgradeConfig.txMaxWriteBytes = cfg.APPLY_LOAD_TX_MAX_WRITE_BYTES;
    upgradeConfig.txMaxContractEventsSizeBytes =
        cfg.APPLY_LOAD_MAX_CONTRACT_EVENT_SIZE_BYTES;
    upgradeConfig.ledgerMaxTransactionsSizeBytes =
        cfg.APPLY_LOAD_MAX_LEDGER_TX_SIZE_BYTES;
    upgradeConfig.txMaxSizeBytes = cfg.APPLY_LOAD_MAX_TX_SIZE_BYTES;
    upgradeConfig.liveSorobanStateSizeWindowSampleSize = 30;
    upgradeConfig.evictionScanSize = 100000;
    upgradeConfig.startingEvictionScanLevel = 7;

    upgradeConfig.ledgerMaxDependentTxClusters =
        cfg.APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS;

    // Increase the default TTL and reduce the rent rate in order to avoid the
    // state archival and too high rent fees. The apply load test is generally
    // not concerned about the resource fees.
    upgradeConfig.minPersistentTTL = 1'000'000'000;
    upgradeConfig.minTemporaryTTL = 1'000'000'000;
    upgradeConfig.maxEntryTTL = 1'000'000'001;
    upgradeConfig.persistentRentRateDenominator = 1'000'000'000'000LL;
    upgradeConfig.tempRentRateDenominator = 1'000'000'000'000LL;

    // These values are set above using values from Config, so the assertions
    // will fail if the config file is missing any of these values.
    releaseAssert(upgradeConfig.ledgerMaxInstructions > 0);
    releaseAssert(upgradeConfig.txMaxInstructions > 0);
    releaseAssert(upgradeConfig.ledgerMaxDiskReadEntries > 0);
    releaseAssert(upgradeConfig.ledgerMaxDiskReadBytes > 0);
    releaseAssert(upgradeConfig.ledgerMaxWriteLedgerEntries > 0);
    releaseAssert(upgradeConfig.ledgerMaxWriteBytes > 0);
    releaseAssert(upgradeConfig.ledgerMaxTxCount > 0);
    releaseAssert(upgradeConfig.txMaxDiskReadEntries > 0);
    releaseAssert(upgradeConfig.txMaxDiskReadBytes > 0);
    releaseAssert(upgradeConfig.txMaxWriteLedgerEntries > 0);
    releaseAssert(upgradeConfig.txMaxWriteBytes > 0);
    releaseAssert(upgradeConfig.txMaxContractEventsSizeBytes > 0);
    releaseAssert(upgradeConfig.ledgerMaxTransactionsSizeBytes > 0);
    releaseAssert(upgradeConfig.txMaxSizeBytes > 0);
    return upgradeConfig;
}
}

ApplyLoad::ApplyLoad(Application& app, ApplyLoadMode mode)
    : mTxGenerator(app)
    , mApp(app)
    , mRoot(app.getRoot())
    , mNumAccounts(
          mApp.getConfig().APPLY_LOAD_MAX_TX_COUNT *
              mApp.getConfig().SOROBAN_TRANSACTION_QUEUE_SIZE_MULTIPLIER +
          1)
    , mTxCountUtilization(
          mApp.getMetrics().NewHistogram({"soroban", "benchmark", "tx-count"}))
    , mInstructionUtilization(
          mApp.getMetrics().NewHistogram({"soroban", "benchmark", "ins"}))
    , mTxSizeUtilization(
          mApp.getMetrics().NewHistogram({"soroban", "benchmark", "tx-size"}))
    , mReadByteUtilization(
          mApp.getMetrics().NewHistogram({"soroban", "benchmark", "read-byte"}))
    , mWriteByteUtilization(mApp.getMetrics().NewHistogram(
          {"soroban", "benchmark", "write-byte"}))
    , mReadEntryUtilization(mApp.getMetrics().NewHistogram(
          {"soroban", "benchmark", "read-entry"}))
    , mWriteEntryUtilization(mApp.getMetrics().NewHistogram(
          {"soroban", "benchmark", "write-entry"}))
    , mMode(mode)
{
    setup();
}

void
ApplyLoad::setup()
{
    releaseAssert(mTxGenerator.loadAccount(mRoot));

    setupAccounts();

    if (mMode == ApplyLoadMode::SOROBAN)
    {
        setupUpgradeContract();

        upgradeSettings();

        setupLoadContract();
    }
    else
    {
        auto upgrade = xdr::xvector<UpgradeType, 6>{};

        LedgerUpgrade ledgerUpgrade;
        ledgerUpgrade.type(LEDGER_UPGRADE_MAX_TX_SET_SIZE);
        ledgerUpgrade.newMaxTxSetSize() =
            mApp.getConfig().APPLY_LOAD_MAX_TX_COUNT;
        auto v = xdr::xdr_to_opaque(ledgerUpgrade);
        upgrade.push_back(UpgradeType{v.begin(), v.end()});
        closeLedger({}, upgrade);
    }

    setupBucketList();
}

void
ApplyLoad::closeLedger(std::vector<TransactionFrameBasePtr> const& txs,
                       xdr::xvector<UpgradeType, 6> const& upgrades)
{
    auto txSet = makeTxSetFromTransactions(txs, mApp, 0, UINT64_MAX);

    auto sv =
        mApp.getHerder().makeStellarValue(txSet.first->getContentsHash(), 1,
                                          upgrades, mApp.getConfig().NODE_SEED);

    stellar::txtest::closeLedger(mApp, txs, /* strictOrder */ false, upgrades);
}

void
ApplyLoad::setupAccounts()
{
    auto const& lm = mApp.getLedgerManager();
    // pass in false for initialAccounts so we fund new account with a lower
    // balance, allowing the creation of more accounts.
    std::vector<Operation> creationOps = mTxGenerator.createAccounts(
        0, mNumAccounts, lm.getLastClosedLedgerNum() + 1, false);

    for (size_t i = 0; i < creationOps.size(); i += MAX_OPS_PER_TX)
    {
        std::vector<TransactionFrameBaseConstPtr> txs;

        size_t end_id = std::min(i + MAX_OPS_PER_TX, creationOps.size());
        std::vector<Operation> currOps(creationOps.begin() + i,
                                       creationOps.begin() + end_id);
        txs.push_back(mTxGenerator.createTransactionFramePtr(
            mRoot, currOps, false, std::nullopt));

        closeLedger(txs);
    }
}

void
ApplyLoad::setupUpgradeContract()
{
    auto wasm = rust_bridge::get_write_bytes();
    xdr::opaque_vec<> wasmBytes;
    wasmBytes.assign(wasm.data.begin(), wasm.data.end());

    LedgerKey contractCodeLedgerKey;
    contractCodeLedgerKey.type(CONTRACT_CODE);
    contractCodeLedgerKey.contractCode().hash = sha256(wasmBytes);

    mUpgradeCodeKey = contractCodeLedgerKey;

    SorobanResources uploadResources;
    uploadResources.instructions = 2'000'000;
    uploadResources.diskReadBytes = wasmBytes.size() + 500;
    uploadResources.writeBytes = wasmBytes.size() + 500;

    auto const& lm = mApp.getLedgerManager();
    auto uploadTx = mTxGenerator.createUploadWasmTransaction(
        lm.getLastClosedLedgerNum() + 1, 0, wasmBytes, contractCodeLedgerKey,
        std::nullopt, uploadResources);

    closeLedger({uploadTx.second});

    auto salt = sha256("upgrade contract salt preimage");

    auto createTx = mTxGenerator.createContractTransaction(
        lm.getLastClosedLedgerNum() + 1, 0, contractCodeLedgerKey,
        wasmBytes.size() + 160, salt, std::nullopt);
    closeLedger({createTx.second});

    mUpgradeInstanceKey =
        createTx.second->sorobanResources().footprint.readWrite.back();

    releaseAssert(mTxGenerator.getApplySorobanSuccess().count() == 2);
}

// To upgrade settings, just modify mUpgradeConfig and then call
// upgradeSettings()
void
ApplyLoad::upgradeSettings()
{
    auto const& lm = mApp.getLedgerManager();
    auto upgradeConfig = getUpgradeConfig(mApp.getConfig());
    auto upgradeBytes =
        mTxGenerator.getConfigUpgradeSetFromLoadConfig(upgradeConfig);

    SorobanResources resources;
    resources.instructions = 1'250'000;
    resources.diskReadBytes = 3'100;
    resources.writeBytes = 3'100;

    auto invokeTx = mTxGenerator.invokeSorobanCreateUpgradeTransaction(
        lm.getLastClosedLedgerNum() + 1, 0, upgradeBytes, mUpgradeCodeKey,
        mUpgradeInstanceKey, std::nullopt, resources);

    auto upgradeSetKey = mTxGenerator.getConfigUpgradeSetKey(
        upgradeConfig,
        mUpgradeInstanceKey.contractData().contract.contractId());

    auto upgrade = xdr::xvector<UpgradeType, 6>{};
    auto ledgerUpgrade = LedgerUpgrade{LEDGER_UPGRADE_CONFIG};
    ledgerUpgrade.newConfig() = upgradeSetKey;
    auto v = xdr::xdr_to_opaque(ledgerUpgrade);
    upgrade.push_back(UpgradeType{v.begin(), v.end()});

    closeLedger({invokeTx.second}, upgrade);
}

void
ApplyLoad::setupLoadContract()
{
    auto wasm = rust_bridge::get_test_wasm_loadgen();
    xdr::opaque_vec<> wasmBytes;
    wasmBytes.assign(wasm.data.begin(), wasm.data.end());

    LedgerKey contractCodeLedgerKey;
    contractCodeLedgerKey.type(CONTRACT_CODE);
    contractCodeLedgerKey.contractCode().hash = sha256(wasmBytes);

    mLoadCodeKey = contractCodeLedgerKey;

    int64_t currApplySorobanSuccess =
        mTxGenerator.getApplySorobanSuccess().count();

    auto const& lm = mApp.getLedgerManager();
    auto uploadTx = mTxGenerator.createUploadWasmTransaction(
        lm.getLastClosedLedgerNum() + 1, 0, wasmBytes, contractCodeLedgerKey,
        std::nullopt);

    closeLedger({uploadTx.second});

    auto salt = sha256("Load contract");

    auto createTx = mTxGenerator.createContractTransaction(
        lm.getLastClosedLedgerNum() + 1, 0, contractCodeLedgerKey,
        wasmBytes.size() + 160, salt, std::nullopt);
    closeLedger({createTx.second});

    releaseAssert(mTxGenerator.getApplySorobanSuccess().count() -
                      currApplySorobanSuccess ==
                  2);
    releaseAssert(mTxGenerator.getApplySorobanFailure().count() == 0);

    auto instanceKey =
        createTx.second->sorobanResources().footprint.readWrite.back();

    mLoadInstance.readOnlyKeys.emplace_back(mLoadCodeKey);
    mLoadInstance.readOnlyKeys.emplace_back(instanceKey);
    mLoadInstance.contractID = instanceKey.contractData().contract;
    mLoadInstance.contractEntriesSize =
        footprintSize(mApp, mLoadInstance.readOnlyKeys);
}

void
ApplyLoad::setupBucketList()
{
    auto lh = mApp.getLedgerManager().getLastClosedLedgerHeader().header;
    auto& bl = mApp.getBucketManager().getLiveBucketList();
    auto& hotArchiveBl = mApp.getBucketManager().getHotArchiveBucketList();
    auto const& cfg = mApp.getConfig();

    mHotArchiveContractID.type(SC_ADDRESS_TYPE_CONTRACT);
    mHotArchiveContractID.contractId() = sha256("ApplyLoad hot archive");

    uint64_t currentLiveKey = 0;
    uint64_t currentHotArchiveKey = 0;

    // Prepare base entries for both live and hot archive
    LedgerEntry baseLiveEntry;
    baseLiveEntry.data.type(CONTRACT_DATA);
    baseLiveEntry.data.contractData().contract = mLoadInstance.contractID;
    baseLiveEntry.data.contractData().key.type(SCV_U64);
    baseLiveEntry.data.contractData().key.u64() = 0;
    baseLiveEntry.data.contractData().durability =
        ContractDataDurability::PERSISTENT;
    baseLiveEntry.data.contractData().val.type(SCV_BYTES);

    mDataEntrySize = xdr::xdr_size(baseLiveEntry);
    // Add some padding to reach the configured LE size.
    if (mDataEntrySize <
        mApp.getConfig().APPLY_LOAD_DATA_ENTRY_SIZE_FOR_TESTING)
    {
        baseLiveEntry.data.contractData().val.bytes().resize(
            mApp.getConfig().APPLY_LOAD_DATA_ENTRY_SIZE_FOR_TESTING -
            mDataEntrySize);
        mDataEntrySize =
            mApp.getConfig().APPLY_LOAD_DATA_ENTRY_SIZE_FOR_TESTING;
    }
    else
    {
        CLOG_WARNING(Perf,
                     "Apply load generated entry size is larger than "
                     "APPLY_LOAD_DATA_ENTRY_SIZE_FOR_TESTING: {} > {}",
                     mApp.getConfig().APPLY_LOAD_DATA_ENTRY_SIZE_FOR_TESTING,
                     mDataEntrySize);
    }

    LedgerEntry baseHotArchiveEntry;
    baseHotArchiveEntry = baseLiveEntry;
    baseHotArchiveEntry.data.contractData().contract = mHotArchiveContractID;

    uint32_t totalHotArchiveEntries =
        mMode == ApplyLoadMode::SOROBAN
            ? cfg.APPLY_LOAD_MAX_TOTAL_ENTRIES_TO_READ
            : 0;
    uint32_t hotArchiveEntriesPerBatch = 0;
    if (totalHotArchiveEntries > 0)
    {
        releaseAssert(cfg.APPLY_LOAD_BL_SIMULATED_LEDGERS > 0);
        hotArchiveEntriesPerBatch = std::max(
            1u, totalHotArchiveEntries / cfg.APPLY_LOAD_BL_SIMULATED_LEDGERS);
    }

    for (uint32_t i = 0; i < cfg.APPLY_LOAD_BL_SIMULATED_LEDGERS; ++i)
    {
        if (i % 1000 == 0)
        {
            CLOG_INFO(Bucket, "Generating BL ledger {}, levels thus far", i);
            for (uint32_t j = 0; j < LiveBucketList::kNumLevels; ++j)
            {
                auto const& lev = bl.getLevel(j);
                auto currSz = BucketTestUtils::countEntries(lev.getCurr());
                auto snapSz = BucketTestUtils::countEntries(lev.getSnap());
                CLOG_INFO(Bucket, "Level {}: {} = {} + {}", j, currSz + snapSz,
                          currSz, snapSz);
            }
        }
        lh.ledgerSeq++;

        // Prepare live entries
        std::vector<LedgerEntry> liveEntries;
        bool isLastBatch = i >= cfg.APPLY_LOAD_BL_SIMULATED_LEDGERS -
                                    cfg.APPLY_LOAD_BL_LAST_BATCH_LEDGERS;
        if (i % cfg.APPLY_LOAD_BL_WRITE_FREQUENCY == 0 || isLastBatch)
        {
            uint32_t entryCount = isLastBatch
                                      ? cfg.APPLY_LOAD_BL_LAST_BATCH_SIZE
                                      : cfg.APPLY_LOAD_BL_BATCH_SIZE;
            for (uint32_t j = 0; j < entryCount; j++)
            {
                LedgerEntry le = baseLiveEntry;
                le.lastModifiedLedgerSeq = lh.ledgerSeq;
                le.data.contractData().key.u64() = currentLiveKey++;
                liveEntries.push_back(le);

                LedgerEntry ttlEntry;
                ttlEntry.data.type(TTL);
                ttlEntry.lastModifiedLedgerSeq = lh.ledgerSeq;
                ttlEntry.data.ttl().keyHash = xdrSha256(LedgerEntryKey(le));
                ttlEntry.data.ttl().liveUntilLedgerSeq = 1'000'000'000;
                liveEntries.push_back(ttlEntry);
            }
        }

        bl.addBatch(mApp, lh.ledgerSeq, lh.ledgerVersion, liveEntries, {}, {});

        // Add hot archive entries
        if (currentHotArchiveKey < totalHotArchiveEntries)
        {
            std::vector<LedgerEntry> hotArchiveEntries;
            uint32_t entriesToAdd =
                std::min(hotArchiveEntriesPerBatch,
                         totalHotArchiveEntries -
                             static_cast<uint32_t>(currentHotArchiveKey));

            for (uint32_t j = 0; j < entriesToAdd; j++)
            {
                LedgerEntry le = baseHotArchiveEntry;
                le.lastModifiedLedgerSeq = lh.ledgerSeq;
                le.data.contractData().key.u64() = currentHotArchiveKey;
                hotArchiveEntries.push_back(le);
                ++currentHotArchiveKey;
            }

            if (!hotArchiveEntries.empty())
            {
                hotArchiveBl.addBatch(mApp, lh.ledgerSeq, lh.ledgerVersion,
                                      hotArchiveEntries, {});
            }
        }
    }
    lh.ledgerSeq++;
    mDataEntryCount = currentLiveKey;

    auto logBucketListStats = [](std::string const& name,
                                 auto const& bucketList) {
        CLOG_INFO(Bucket, "Final generated {} bucket list levels", name);
        for (uint32_t i = 0;
             i < std::remove_reference_t<decltype(bucketList)>::kNumLevels; ++i)
        {
            auto const& lev = bucketList.getLevel(i);
            auto currSz = BucketTestUtils::countEntries(lev.getCurr());
            auto snapSz = BucketTestUtils::countEntries(lev.getSnap());
            CLOG_INFO(Bucket, "Level {}: {} = {} + {}", i, currSz + snapSz,
                      currSz, snapSz);
        }
    };

    logBucketListStats("live", bl);

    if (totalHotArchiveEntries > 0)
    {
        logBucketListStats("hot archive", hotArchiveBl);
    }

    HistoryArchiveState has;
    has.currentLedger = lh.ledgerSeq;
    mApp.getPersistentState().setState(PersistentState::kHistoryArchiveState,
                                       has.toString(),
                                       mApp.getDatabase().getSession());
    mApp.getBucketManager().snapshotLedger(lh);
    {
        LedgerTxn ltx(mApp.getLedgerTxnRoot());
        ltx.loadHeader().current() = lh;
        mApp.getLedgerManager().manuallyAdvanceLedgerHeader(
            ltx.loadHeader().current());
        ltx.commit();
    }
    mApp.getLedgerManager().storeCurrentLedgerForTest(lh);
    mApp.getLedgerManager().rebuildInMemorySorobanStateForTesting(
        lh.ledgerVersion);
    mApp.getHerder().forceSCPStateIntoSyncWithLastClosedLedger();
    closeLedger({}, {});
}

void
ApplyLoad::benchmark()
{
    auto& lm = mApp.getLedgerManager();
    std::vector<TransactionFrameBasePtr> txs;

    auto resources = multiplyByDouble(
        lm.maxLedgerResources(true),
        mApp.getConfig().SOROBAN_TRANSACTION_QUEUE_SIZE_MULTIPLIER);

    // The TxSet validation will compare the ledger instruction limit against
    // the sum of the instructions of the slowest cluster in each stage, so we
    // just multiply the instructions limit by the max number of clusters.
    resources.setVal(
        Resource::Type::INSTRUCTIONS,
        resources.getVal(Resource::Type::INSTRUCTIONS) *
            mApp.getConfig().APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS);
    auto classicResources =
        multiplyByDouble(lm.maxLedgerResources(false),
                         mApp.getConfig().TRANSACTION_QUEUE_SIZE_MULTIPLIER);

    // Save a snapshot so we can calculate what % we used up.
    auto const resourcesSnapshot = resources;
    auto const classicResourcesSnapshot = classicResources;

    auto const& accounts = mTxGenerator.getAccounts();
    std::vector<uint64_t> shuffledAccounts(accounts.size());
    std::iota(shuffledAccounts.begin(), shuffledAccounts.end(), 0);
    stellar::shuffle(std::begin(shuffledAccounts), std::end(shuffledAccounts),
                     gRandomEngine);

    bool limitHit = false;
    for (auto accountIndex : shuffledAccounts)
    {
        auto it = accounts.find(accountIndex);
        releaseAssert(it != accounts.end());

        std::pair<TxGenerator::TestAccountPtr, TransactionFrameBaseConstPtr> tx;

        if (mMode == ApplyLoadMode::CLASSIC)
        {
            it->second->loadSequenceNumber();
            tx = mTxGenerator.paymentTransaction(
                mNumAccounts, 0, lm.getLastClosedLedgerNum() + 1, it->first, 1,
                std::nullopt);
        }
        else
        {
            tx = mTxGenerator.invokeSorobanLoadTransactionV2(
                lm.getLastClosedLedgerNum() + 1, it->first, mLoadInstance,
                mDataEntryCount, mDataEntrySize, 1'000'000);
        }

        {
            LedgerTxn ltx(mApp.getLedgerTxnRoot());
            auto diagnostics = DiagnosticEventManager::createDisabled();
            auto res = tx.second->checkValid(mApp.getAppConnector(), ltx, 0, 0,
                                             UINT64_MAX, diagnostics);
            releaseAssert((res && res->isSuccess()));
        }

        uint32_t ledgerVersion = mApp.getLedgerManager()
                                     .getLastClosedLedgerHeader()
                                     .header.ledgerVersion;
        if (mMode == ApplyLoadMode::SOROBAN)
        {
            if (!anyGreater(tx.second->getResources(false, ledgerVersion),
                            resources))
            {
                resources -= tx.second->getResources(false, ledgerVersion);
            }
            else
            {
                for (size_t i = 0; i < resources.size(); ++i)
                {
                    auto type = static_cast<Resource::Type>(i);
                    if (tx.second->getResources(false, ledgerVersion)
                            .getVal(type) > resources.getVal(type))
                    {
                        CLOG_INFO(Perf,
                                  "Ledger {} limit hit during tx generation",
                                  Resource::getStringFromType(type));
                        limitHit = true;
                    }
                }

                break;
            }
        }
        else if (mMode == ApplyLoadMode::CLASSIC)
        {
            if (!anyGreater(tx.second->getResources(false, ledgerVersion),
                            classicResources))
            {
                classicResources -=
                    tx.second->getResources(false, ledgerVersion);
            }
            else
            {
                CLOG_INFO(Perf,
                          "Operation ledger limit hit during tx generation");
                limitHit = true;
                break;
            }
        }

        txs.emplace_back(tx.second);
    }

    // If this assert fails, it most likely means that we ran out of
    // accounts, which should not happen.
    releaseAssert(limitHit);

    // Only update Soroban-specific metrics in Soroban mode
    if (mMode == ApplyLoadMode::SOROBAN)
    {
        mTxCountUtilization.Update(
            (1.0 - (resources.getVal(Resource::Type::OPERATIONS) * 1.0 /
                    resourcesSnapshot.getVal(Resource::Type::OPERATIONS))) *
            100000.0);
        mInstructionUtilization.Update(
            (1.0 - (resources.getVal(Resource::Type::INSTRUCTIONS) * 1.0 /
                    resourcesSnapshot.getVal(Resource::Type::INSTRUCTIONS))) *
            100000.0);
        mTxSizeUtilization.Update(
            (1.0 - (resources.getVal(Resource::Type::TX_BYTE_SIZE) * 1.0 /
                    resourcesSnapshot.getVal(Resource::Type::TX_BYTE_SIZE))) *
            100000.0);
        mReadByteUtilization.Update(
            (1.0 -
             (resources.getVal(Resource::Type::DISK_READ_BYTES) * 1.0 /
              resourcesSnapshot.getVal(Resource::Type::DISK_READ_BYTES))) *
            100000.0);
        mWriteByteUtilization.Update(
            (1.0 - (resources.getVal(Resource::Type::WRITE_BYTES) * 1.0 /
                    resourcesSnapshot.getVal(Resource::Type::WRITE_BYTES))) *
            100000.0);
        mReadEntryUtilization.Update(
            (1.0 -
             (resources.getVal(Resource::Type::READ_LEDGER_ENTRIES) * 1.0 /
              resourcesSnapshot.getVal(Resource::Type::READ_LEDGER_ENTRIES))) *
            100000.0);
        mWriteEntryUtilization.Update(
            (1.0 -
             (resources.getVal(Resource::Type::WRITE_LEDGER_ENTRIES) * 1.0 /
              resourcesSnapshot.getVal(Resource::Type::WRITE_LEDGER_ENTRIES))) *
            100000.0);
    }
    else if (mMode == ApplyLoadMode::CLASSIC)
    {
        mTxCountUtilization.Update(
            (1.0 -
             (classicResources.getVal(Resource::Type::OPERATIONS) * 1.0 /
              classicResourcesSnapshot.getVal(Resource::Type::OPERATIONS))) *
            100000.0);
    }

    closeLedger(txs);
}

double
ApplyLoad::successRate()
{
    if (mMode == ApplyLoadMode::CLASSIC)
    {
        auto& success =
            mApp.getMetrics().NewCounter({"ledger", "apply", "success"});
        auto& failure =
            mApp.getMetrics().NewCounter({"ledger", "apply", "failure"});
        return success.count() * 1.0 / (success.count() + failure.count());
    }
    else
    {
        return mTxGenerator.getApplySorobanSuccess().count() * 1.0 /
               (mTxGenerator.getApplySorobanSuccess().count() +
                mTxGenerator.getApplySorobanFailure().count());
    }
}

medida::Histogram const&
ApplyLoad::getTxCountUtilization()
{
    return mTxCountUtilization;
}
medida::Histogram const&
ApplyLoad::getInstructionUtilization()
{
    return mInstructionUtilization;
}
medida::Histogram const&
ApplyLoad::getTxSizeUtilization()
{
    return mTxSizeUtilization;
}
medida::Histogram const&
ApplyLoad::getReadByteUtilization()
{
    return mReadByteUtilization;
}
medida::Histogram const&
ApplyLoad::getWriteByteUtilization()
{
    return mWriteByteUtilization;
}
medida::Histogram const&
ApplyLoad::getReadEntryUtilization()
{
    return mReadEntryUtilization;
}
medida::Histogram const&
ApplyLoad::getWriteEntryUtilization()
{
    return mWriteEntryUtilization;
}

}
