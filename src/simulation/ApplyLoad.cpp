#include "simulation/ApplyLoad.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <string>
#include <unordered_map>

#include "bucket/test/BucketTestUtils.h"
#include "herder/Herder.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerManagerImpl.h"
#include "simulation/TxGenerator.h"
#include "test/TxTests.h"
#include "transactions/MutableTransactionResult.h"
#include "transactions/TransactionUtils.h"
#include "util/types.h"

#include "herder/HerderImpl.h"

#include "medida/metrics_registry.h"

#include "bucket/BucketListSnapshotBase.h"
#include "bucket/BucketSnapshotManager.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/XDRCereal.h"
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
    releaseAssert(*upgradeConfig.ledgerMaxInstructions > 0);
    releaseAssert(*upgradeConfig.txMaxInstructions > 0);
    releaseAssert(*upgradeConfig.ledgerMaxDiskReadEntries > 0);
    releaseAssert(*upgradeConfig.ledgerMaxDiskReadBytes > 0);
    releaseAssert(*upgradeConfig.ledgerMaxWriteLedgerEntries > 0);
    releaseAssert(*upgradeConfig.ledgerMaxWriteBytes > 0);
    releaseAssert(*upgradeConfig.ledgerMaxTxCount > 0);
    releaseAssert(*upgradeConfig.txMaxDiskReadEntries > 0);
    releaseAssert(*upgradeConfig.txMaxDiskReadBytes > 0);
    releaseAssert(*upgradeConfig.txMaxWriteLedgerEntries > 0);
    releaseAssert(*upgradeConfig.txMaxWriteBytes > 0);
    releaseAssert(*upgradeConfig.txMaxContractEventsSizeBytes > 0);
    releaseAssert(*upgradeConfig.ledgerMaxTransactionsSizeBytes > 0);
    releaseAssert(*upgradeConfig.txMaxSizeBytes > 0);
    return upgradeConfig;
}

SorobanUpgradeConfig
getUpgradeConfigForMaxTPS(Config const& cfg, uint64_t instructionsPerCluster,
                          uint32_t totalTxs)
{
    SorobanUpgradeConfig upgradeConfig;
    upgradeConfig.ledgerMaxDependentTxClusters =
        cfg.APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS;

    // Set high limits to avoid resource constraints during testing
    constexpr uint32_t LEDGER_MAX_LIMIT = UINT32_MAX / 2;
    constexpr uint32_t TX_MAX_LIMIT = UINT32_MAX / 4;

    upgradeConfig.maxContractSizeBytes = LEDGER_MAX_LIMIT;
    upgradeConfig.maxContractDataKeySizeBytes = LEDGER_MAX_LIMIT;
    upgradeConfig.maxContractDataEntrySizeBytes = LEDGER_MAX_LIMIT;
    upgradeConfig.txMemoryLimit = LEDGER_MAX_LIMIT;
    upgradeConfig.evictionScanSize = 100;
    upgradeConfig.startingEvictionScanLevel = 7;

    upgradeConfig.ledgerMaxDiskReadEntries = LEDGER_MAX_LIMIT;
    upgradeConfig.ledgerMaxDiskReadBytes = LEDGER_MAX_LIMIT;
    upgradeConfig.ledgerMaxWriteLedgerEntries = LEDGER_MAX_LIMIT;
    upgradeConfig.ledgerMaxWriteBytes = LEDGER_MAX_LIMIT;

    upgradeConfig.txMaxDiskReadEntries = TX_MAX_LIMIT;
    upgradeConfig.txMaxFootprintEntries = TX_MAX_LIMIT;
    upgradeConfig.txMaxDiskReadBytes = TX_MAX_LIMIT;
    upgradeConfig.txMaxWriteLedgerEntries = TX_MAX_LIMIT;
    upgradeConfig.txMaxWriteBytes = TX_MAX_LIMIT;

    upgradeConfig.ledgerMaxTransactionsSizeBytes = LEDGER_MAX_LIMIT;
    upgradeConfig.txMaxSizeBytes = TX_MAX_LIMIT;
    upgradeConfig.txMaxContractEventsSizeBytes = TX_MAX_LIMIT;

    // Increase the default TTL and reduce the rent rate in order to avoid the
    // state archival and too high rent fees. The apply load test is generally
    // not concerned about the resource fees.
    upgradeConfig.minPersistentTTL = 1'000'000'000;
    upgradeConfig.minTemporaryTTL = 1'000'000'000;
    upgradeConfig.maxEntryTTL = 1'000'000'001;
    upgradeConfig.persistentRentRateDenominator = 1'000'000'000'000LL;
    upgradeConfig.tempRentRateDenominator = 1'000'000'000'000LL;

    // Set the instruction and max tx count just high enough so that we generate
    // a full ledger. This ensures all available clusters are filled for maximum
    // parallelism.
    upgradeConfig.ledgerMaxInstructions = instructionsPerCluster;
    upgradeConfig.ledgerMaxTxCount = totalTxs;
    upgradeConfig.txMaxInstructions = instructionsPerCluster;

    releaseAssert(*upgradeConfig.ledgerMaxInstructions > 0);
    releaseAssert(*upgradeConfig.txMaxInstructions > 0);

    if (*upgradeConfig.ledgerMaxInstructions < *upgradeConfig.txMaxInstructions)
    {
        throw std::runtime_error("TPS too low, cannot achieve parallelism");
    }

    return upgradeConfig;
}
}

uint64_t
ApplyLoad::calculateInstructionsPerTx() const
{
    uint32_t batchSize = mApp.getConfig().APPLY_LOAD_BATCH_SAC_COUNT;
    if (batchSize > 1)
    {
        // Conservative estimate: each transfer in batch costs same as SAC
        return batchSize * TxGenerator::BATCH_TRANSFER_TX_INSTRUCTIONS;
    }
    return TxGenerator::SAC_TX_INSTRUCTIONS;
}

void
ApplyLoad::upgradeSettingsForMaxTPS(uint32_t txsToGenerate)
{
    // Calculate the actual instructions needed for all transactions. The
    // ledger max instructions is the total instruction count per cluster. In
    // order to have max parallelism, we want each cluster to be full, so
    // upgrade settings such that we have just enough capacity across all
    // clusters.

    uint64_t instructionsPerTx = calculateInstructionsPerTx();

    uint64_t totalInstructions =
        static_cast<uint64_t>(txsToGenerate) * instructionsPerTx;
    uint64_t instructionsPerCluster =
        totalInstructions /
        mApp.getConfig().APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS;

    // Ensure all transactions can fit
    instructionsPerCluster += instructionsPerTx - 1;

    auto upgradeConfig = getUpgradeConfigForMaxTPS(
        mApp.getConfig(), instructionsPerCluster, txsToGenerate);

    applyConfigUpgrade(upgradeConfig);
}

// Given an index, returns the LedgerKey for an archived entry that is
// pre-populated in the Hot Archive.
LedgerKey
ApplyLoad::getKeyForArchivedEntry(uint64_t index)
{

    static const SCAddress hotArchiveContractID = [] {
        SCAddress addr;
        addr.type(SC_ADDRESS_TYPE_CONTRACT);
        addr.contractId() = sha256("archived-entry");
        return addr;
    }();

    LedgerKey lk;
    lk.type(CONTRACT_DATA);
    lk.contractData().contract = hotArchiveContractID;
    lk.contractData().key.type(SCV_U64);
    lk.contractData().key.u64() = index;
    lk.contractData().durability = ContractDataDurability::PERSISTENT;
    return lk;
}

uint32_t
ApplyLoad::calculateRequiredHotArchiveEntries(Config const& cfg)
{
    // If no RO entries are configured, return 0
    if (cfg.APPLY_LOAD_NUM_RO_ENTRIES_FOR_TESTING.empty())
    {
        return 0;
    }

    releaseAssertOrThrow(
        cfg.APPLY_LOAD_NUM_RO_ENTRIES_FOR_TESTING.size() ==
        cfg.APPLY_LOAD_NUM_RO_ENTRIES_DISTRIBUTION_FOR_TESTING.size());

    // Calculate weighted average of RO entries per transaction
    double totalWeight = 0;
    double weightedSum = 0;
    for (size_t i = 0; i < cfg.APPLY_LOAD_NUM_RO_ENTRIES_FOR_TESTING.size();
         ++i)
    {
        auto entries = cfg.APPLY_LOAD_NUM_RO_ENTRIES_FOR_TESTING[i];
        auto weight = cfg.APPLY_LOAD_NUM_RO_ENTRIES_DISTRIBUTION_FOR_TESTING[i];

        totalWeight += weight;
        weightedSum += static_cast<double>(entries) * weight;
    }

    double avgROEntriesPerTx = totalWeight > 0 ? weightedSum / totalWeight : 0;

    // Calculate total expected RO accesses
    double totalExpectedRestores = avgROEntriesPerTx *
                                   cfg.APPLY_LOAD_MAX_TX_COUNT *
                                   cfg.APPLY_LOAD_NUM_LEDGERS;

    // Add some generous buffer since actual distributions may vary.
    return totalExpectedRestores * 1.5;
}

ApplyLoad::ApplyLoad(Application& app, ApplyLoadMode mode)
    : mApp(app)
    , mRoot(app.getRoot())
    , mNumAccounts(std::max(
          mApp.getConfig().APPLY_LOAD_MAX_TX_COUNT *
                  (mApp.getConfig().SOROBAN_TRANSACTION_QUEUE_SIZE_MULTIPLIER +
                   mApp.getConfig().TRANSACTION_QUEUE_SIZE_MULTIPLIER) +
              2,
          mApp.getConfig().APPLY_LOAD_NUM_ACCOUNTS))
    , mTotalHotArchiveEntries(
          calculateRequiredHotArchiveEntries(app.getConfig()))
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
    , mTxGenerator(app, mTotalHotArchiveEntries)
{
    setup();
}

void
ApplyLoad::setup()
{
    releaseAssert(mTxGenerator.loadAccount(mRoot));

    setupAccounts();

    if (mMode == ApplyLoadMode::SOROBAN || mMode == ApplyLoadMode::MIX ||
        mMode == ApplyLoadMode::MAX_SAC_TPS)
    {
        setupUpgradeContract();

        if (mMode == ApplyLoadMode::MAX_SAC_TPS)
        {
            // Just upgrade to a placeholder number of TXs, we'll
            // upgrade again before each TPS run.
            upgradeSettingsForMaxTPS(100000);
        }
        else
        {
            upgradeSettings();
        }

        setupLoadContract();

        if (mMode == ApplyLoadMode::MIX || mMode == ApplyLoadMode::MAX_SAC_TPS)
        {
            setupXLMContract();
            if (mMode == ApplyLoadMode::MAX_SAC_TPS &&
                mApp.getConfig().APPLY_LOAD_BATCH_SAC_COUNT > 1)
            {
                setupBatchTransferContracts();
            }
        }
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
        txs.push_back(mTxGenerator.createTransactionFramePtr(mRoot, currOps,
                                                             std::nullopt));

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
ApplyLoad::applyConfigUpgrade(SorobanUpgradeConfig const& upgradeConfig)
{
    int64_t currApplySorobanSuccess =
        mTxGenerator.getApplySorobanSuccess().count();
    auto const& lm = mApp.getLedgerManager();
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

    releaseAssert(mTxGenerator.getApplySorobanSuccess().count() -
                      currApplySorobanSuccess ==
                  1);
}

void
ApplyLoad::upgradeSettings()
{
    releaseAssertOrThrow(mMode != ApplyLoadMode::MAX_SAC_TPS);

    auto upgradeConfig = getUpgradeConfig(mApp.getConfig());
    applyConfigUpgrade(upgradeConfig);
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
ApplyLoad::setupXLMContract()
{
    int64_t currApplySorobanSuccess =
        mTxGenerator.getApplySorobanSuccess().count();

    auto createTx = mTxGenerator.createSACTransaction(
        mApp.getLedgerManager().getLastClosedLedgerNum() + 1, 0,
        txtest::makeNativeAsset(), std::nullopt);
    closeLedger({createTx.second});

    releaseAssert(mTxGenerator.getApplySorobanSuccess().count() -
                      currApplySorobanSuccess ==
                  1);
    releaseAssert(mTxGenerator.getApplySorobanFailure().count() == 0);

    auto instanceKey =
        createTx.second->sorobanResources().footprint.readWrite.back();

    mSACInstanceXLM.readOnlyKeys.emplace_back(instanceKey);
    mSACInstanceXLM.contractID = instanceKey.contractData().contract;
    mSACInstanceXLM.contractEntriesSize =
        footprintSize(mApp, mSACInstanceXLM.readOnlyKeys);
}

void
ApplyLoad::setupBatchTransferContracts()
{
    auto const& lm = mApp.getLedgerManager();

    // First, upload the batch_transfer contract WASM
    auto wasm = rust_bridge::get_test_contract_sac_transfer();
    xdr::opaque_vec<> wasmBytes;
    wasmBytes.assign(wasm.data.begin(), wasm.data.end());

    LedgerKey contractCodeLedgerKey;
    contractCodeLedgerKey.type(CONTRACT_CODE);
    contractCodeLedgerKey.contractCode().hash = sha256(wasmBytes);

    SorobanResources uploadResources;
    uploadResources.instructions = 5000000;
    uploadResources.diskReadBytes = wasmBytes.size() + 500;
    uploadResources.writeBytes = wasmBytes.size() + 500;

    auto uploadTx = mTxGenerator.createUploadWasmTransaction(
        lm.getLastClosedLedgerNum() + 1, TxGenerator::ROOT_ACCOUNT_ID,
        wasmBytes, contractCodeLedgerKey, std::nullopt, uploadResources);
    closeLedger({uploadTx.second});

    // Since we transfer from the batch transfer contract balance, deploy one
    // contract for each cluster to maximize parallelism
    uint32_t numClusters =
        mApp.getConfig().APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS;
    mBatchTransferInstances.reserve(numClusters);

    for (uint32_t i = 0; i < numClusters; ++i)
    {
        auto successCountBefore = mTxGenerator.getApplySorobanSuccess().count();
        auto salt = sha256(std::to_string(i));

        auto createTx = mTxGenerator.createContractTransaction(
            lm.getLastClosedLedgerNum() + 1, TxGenerator::ROOT_ACCOUNT_ID,
            contractCodeLedgerKey, wasmBytes.size() + 160, salt, std::nullopt);
        closeLedger({createTx.second});

        auto instanceKey =
            createTx.second->sorobanResources().footprint.readWrite.back();

        TxGenerator::ContractInstance instance;
        instance.readOnlyKeys.emplace_back(contractCodeLedgerKey);
        instance.readOnlyKeys.emplace_back(instanceKey);
        instance.contractID = instanceKey.contractData().contract;
        instance.contractEntriesSize =
            footprintSize(mApp, instance.readOnlyKeys);

        mBatchTransferInstances.push_back(instance);

        // Initialize XLM balance for the batch_transfer contract
        // We need to transfer enough XLM to cover all batch transfers
        // Each batch will transfer APPLY_LOAD_BATCH_SAC_COUNT * 1 stroop
        int64_t maxTxsPerCluster =
            mApp.getConfig().APPLY_LOAD_MAX_SAC_TPS_MAX_TPS / numClusters;
        int64_t amountToTransfer =
            mApp.getConfig().APPLY_LOAD_BATCH_SAC_COUNT * // Sent per tx
            maxTxsPerCluster * // Max txs per ledger per cluster
            mApp.getConfig().APPLY_LOAD_NUM_LEDGERS * // Number of ledgers
            10;                                       // Buffer

        auto transferTx = mTxGenerator.invokeSACPayment(
            lm.getLastClosedLedgerNum() + 1, TxGenerator::ROOT_ACCOUNT_ID,
            instance.contractID, mSACInstanceXLM, amountToTransfer, 10'000'000);
        closeLedger({transferTx.second});

        auto successCountAfter = mTxGenerator.getApplySorobanSuccess().count();

        // Verify both instantiate and fund transactions succeeded
        releaseAssertOrThrow(successCountAfter == successCountBefore + 2);
        releaseAssertOrThrow(mTxGenerator.getApplySorobanFailure().count() ==
                             0);
    }

    releaseAssertOrThrow(mBatchTransferInstances.size() == numClusters);
}

void
ApplyLoad::setupBucketList()
{
    auto lh = mApp.getLedgerManager().getLastClosedLedgerHeader().header;
    auto& bl = mApp.getBucketManager().getLiveBucketList();
    auto& hotArchiveBl = mApp.getBucketManager().getHotArchiveBucketList();
    auto const& cfg = mApp.getConfig();

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

    auto logBucketListStats = [](std::string const& logStr,
                                 auto const& bucketList) {
        CLOG_INFO(Bucket, "{}", logStr);
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

    LedgerEntry baseHotArchiveEntry = baseLiveEntry;

    // Hot archive entries are added every APPLY_LOAD_BL_WRITE_FREQUENCY
    // ledgers, but save one batch for the last batch to populate upper levels.
    uint32_t totalBatchCount =
        cfg.APPLY_LOAD_BL_SIMULATED_LEDGERS / cfg.APPLY_LOAD_BL_WRITE_FREQUENCY;
    releaseAssertOrThrow(totalBatchCount > 0);

    // Reserve one batch worth of entries for the top level buckets.
    uint32_t hotArchiveBatchCount = totalBatchCount - 1;
    uint32_t hotArchiveBatchSize =
        mTotalHotArchiveEntries / (totalBatchCount + 1);

    // To populate the first few levels of the hot archive BL, we write the
    // remaining entries over APPLY_LOAD_BL_LAST_BATCH_LEDGERS ledgers.
    uint32_t hotArchiveLastBatchSize =
        mTotalHotArchiveEntries > 0
            ? (mTotalHotArchiveEntries -
               (hotArchiveBatchSize * hotArchiveBatchCount)) /
                  cfg.APPLY_LOAD_BL_LAST_BATCH_LEDGERS
            : 0;

    for (uint32_t i = 0; i < cfg.APPLY_LOAD_BL_SIMULATED_LEDGERS; ++i)
    {
        if (i % 1000 == 0)
        {
            logBucketListStats(
                fmt::format("Generating BL ledger {}, levels thus far", i), bl);

            if (mTotalHotArchiveEntries > 0)
            {
                logBucketListStats(
                    fmt::format(
                        "Generating hot archive BL ledger {}, levels thus far",
                        i),
                    hotArchiveBl);
            }
        }
        lh.ledgerSeq++;

        std::vector<LedgerEntry> liveEntries;
        std::vector<LedgerEntry> archivedEntries;
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

            uint32_t archivedEntryCount =
                isLastBatch ? hotArchiveLastBatchSize : hotArchiveBatchSize;
            for (uint32_t j = 0; j < archivedEntryCount; j++)
            {
                LedgerEntry le = baseHotArchiveEntry;
                le.lastModifiedLedgerSeq = lh.ledgerSeq;

                auto lk = getKeyForArchivedEntry(currentHotArchiveKey);
                le.data.contractData().contract = lk.contractData().contract;
                le.data.contractData().key = lk.contractData().key;
                le.data.contractData().durability =
                    lk.contractData().durability;
                le.data.contractData().val =
                    baseLiveEntry.data.contractData().val;

                archivedEntries.push_back(le);
                ++currentHotArchiveKey;
            }
        }

        bl.addBatch(mApp, lh.ledgerSeq, lh.ledgerVersion, liveEntries, {}, {});
        if (mTotalHotArchiveEntries > 0)
        {
            hotArchiveBl.addBatch(mApp, lh.ledgerSeq, lh.ledgerVersion,
                                  archivedEntries, {});
        }
    }
    mDataEntryCount = currentLiveKey;
    releaseAssertOrThrow(mTotalHotArchiveEntries <= currentHotArchiveKey);

    logBucketListStats("Final generated live bucket list levels", bl);
    if (mTotalHotArchiveEntries > 0)
    {
        logBucketListStats("Final generated hot archive bucket list levels",
                           hotArchiveBl);
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
    releaseAssertOrThrow(mMode != ApplyLoadMode::MAX_SAC_TPS);

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
                     getGlobalRandomEngine());

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
        else if (mMode == ApplyLoadMode::SOROBAN)
        {
            tx = mTxGenerator.invokeSorobanLoadTransactionV2(
                lm.getLastClosedLedgerNum() + 1, it->first, mLoadInstance,
                mDataEntryCount, mDataEntrySize, 1'000'000);
        }
        else
        {
            // We use the transaction index to split the load between:
            // - classic payments
            // - SAC payments
            // - Soroban load transactions
            // The distribution between classic and Soroban transactions
            // is not that important because they use separate resource limits.
            if (it->first % 3 == 0)
            {
                it->second->loadSequenceNumber();
                tx = mTxGenerator.paymentTransaction(
                    mNumAccounts, 0, lm.getLastClosedLedgerNum() + 1, it->first,
                    1, std::nullopt);
            }
            else if (it->first % 3 == 1)
            {
                SCAddress toAddress(SC_ADDRESS_TYPE_CONTRACT);
                toAddress.contractId() = sha256(xdr::xdr_to_opaque(it->first));

                tx = mTxGenerator.invokeSACPayment(
                    lm.getLastClosedLedgerNum() + 1, it->first, toAddress,
                    mSACInstanceXLM, 100, 1'000'000);
            }
            else
            {
                tx = mTxGenerator.invokeSorobanLoadTransactionV2(
                    lm.getLastClosedLedgerNum() + 1, it->first, mLoadInstance,
                    mDataEntryCount, mDataEntrySize, 1'000'000);
            }
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
        if (tx.second->isSoroban())
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
        else
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
    if (mMode == ApplyLoadMode::SOROBAN || mMode == ApplyLoadMode::MIX)
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

void
ApplyLoad::warmAccountCache()
{
    auto const& accounts = mTxGenerator.getAccounts();
    CLOG_INFO(Perf, "Warming account cache with {} accounts.", accounts.size());

    LedgerTxn ltx(mApp.getLedgerTxnRoot());
    for (auto const& [_, account] : accounts)
    {
        auto acc = stellar::loadAccount(ltx, account->getPublicKey());
        releaseAssert(acc);
    }
}

void
ApplyLoad::findMaxSacTps()
{
    releaseAssertOrThrow(mMode == ApplyLoadMode::MAX_SAC_TPS);

    uint32_t minTps = mApp.getConfig().APPLY_LOAD_MAX_SAC_TPS_MIN_TPS;
    uint32_t maxTps = mApp.getConfig().APPLY_LOAD_MAX_SAC_TPS_MAX_TPS;
    uint32_t bestTps = 0;
    uint32_t numClusters =
        mApp.getConfig().APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS;
    double targetCloseTime =
        mApp.getConfig().APPLY_LOAD_MAX_SAC_TPS_TARGET_CLOSE_TIME_MS;

    CLOG_WARNING(Perf,
                 "Starting MAX_SAC_TPS binary search between {} and {} TPS",
                 minTps, maxTps);
    CLOG_WARNING(Perf, "Target close time: {}ms", targetCloseTime);
    CLOG_WARNING(Perf, "Num parallel clusters: {}",
                 mApp.getConfig().APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS);

    while (minTps <= maxTps)
    {
        uint32_t testTps = (minTps + maxTps) / 2;

        // Calculate transactions per ledger based on target close time
        uint32_t txsPerLedger = static_cast<uint32_t>(
            static_cast<double>(testTps) * (targetCloseTime / 1000.0));

        // Round down to nearest multiple of cluster count so each cluster has
        // an even distribution
        if (mApp.getConfig().APPLY_LOAD_BATCH_SAC_COUNT > 1)
        {
            txsPerLedger =
                txsPerLedger / mApp.getConfig().APPLY_LOAD_BATCH_SAC_COUNT;
        }

        txsPerLedger = (txsPerLedger / numClusters) * numClusters;
        CLOG_WARNING(Perf, "Testing {} TPS with {} TXs per ledger.", testTps,
                     txsPerLedger);

        upgradeSettingsForMaxTPS(txsPerLedger);

        double avgCloseTime = benchmarkSacTps(txsPerLedger);

        if (avgCloseTime <= targetCloseTime)
        {
            bestTps = testTps;
            minTps = testTps + numClusters;
            CLOG_WARNING(Perf, "Success: {} TPS (avg total tx apply: {:.2f}ms)",
                         testTps, avgCloseTime);
        }
        else
        {
            maxTps = testTps - numClusters;
            CLOG_WARNING(Perf, "Failed: {} TPS (avg total tx apply: {:.2f}ms)",
                         testTps, avgCloseTime);
        }
    }

    CLOG_WARNING(Perf, "================================================");
    CLOG_WARNING(Perf, "Maximum sustainable SAC payments per second: {}",
                 bestTps);
    CLOG_WARNING(Perf, "With parallelism constraint of {} clusters",
                 mApp.getConfig().APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS);
    CLOG_WARNING(Perf, "================================================");
}

double
ApplyLoad::benchmarkSacTps(uint32_t txsPerLedger)
{
    // For timing, we just want to track the TX application itself. This
    // includes charging fees, applying transactions, and post apply work (like
    // meta). It does not include writing the results to disk.
    // When APPLY_LOAD_TIME_WRITES is true, use the ledger close timer instead
    // which includes database writes.
    auto& totalTxApplyTimer =
        mApp.getConfig().APPLY_LOAD_TIME_WRITES
            ? mApp.getMetrics().NewTimer({"ledger", "ledger", "close"})
            : mApp.getMetrics().NewTimer(
                  {"ledger", "transaction", "total-apply"});
    totalTxApplyTimer.Clear();

    uint32_t numLedgers = mApp.getConfig().APPLY_LOAD_NUM_LEDGERS;
    for (uint32_t iter = 0; iter < numLedgers; ++iter)
    {
        warmAccountCache();

        int64_t initialSuccessCount =
            mTxGenerator.getApplySorobanSuccess().count();

        // Generate exactly enough SAC payment transactions
        std::vector<TransactionFrameBasePtr> txs;
        txs.reserve(txsPerLedger);

        generateSacPayments(txs, txsPerLedger);
        releaseAssertOrThrow(txs.size() == txsPerLedger);

        mApp.getBucketManager().getLiveBucketList().resolveAllFutures();
        releaseAssert(
            mApp.getBucketManager().getLiveBucketList().futuresAllResolved());

        closeLedger(txs);

        CLOG_WARNING(Perf, "  Ledger {}/{} completed", iter + 1, numLedgers);

        // Check transaction success rate. We should never have any failures,
        // and all TXs should have been executed.
        int64_t newSuccessCount =
            mTxGenerator.getApplySorobanSuccess().count() - initialSuccessCount;

        releaseAssert(mTxGenerator.getApplySorobanFailure().count() == 0);
        releaseAssert(newSuccessCount == txsPerLedger);

        // Verify we had max parallelism, i.e. 1 stage with
        // maxDependentTxClusters clusters
        auto& stagesMetric =
            mApp.getMetrics().NewCounter({"ledger", "apply-soroban", "stages"});
        auto& maxClustersMetric = mApp.getMetrics().NewCounter(
            {"ledger", "apply-soroban", "max-clusters"});

        releaseAssert(stagesMetric.count() == 1);
        releaseAssert(
            maxClustersMetric.count() ==
            mApp.getConfig().APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS);
    }

    // Calculate average close time from all closed ledgers
    double totalTime = totalTxApplyTimer.sum();
    double avgTime = totalTime / numLedgers;

    CLOG_WARNING(Perf, "  Total time: {:.2f}ms for {} ledgers", totalTime,
                 numLedgers);
    CLOG_WARNING(Perf, "  Average total tx apply time per ledger: {:.2f}ms",
                 avgTime);

    return avgTime;
}

void
ApplyLoad::generateSacPayments(std::vector<TransactionFrameBasePtr>& txs,
                               uint32_t count)
{
    auto const& accounts = mTxGenerator.getAccounts();
    auto& lm = mApp.getLedgerManager();

    releaseAssert(accounts.size() >= count);

    // Use batch_transfer
    uint32_t batchSize = mApp.getConfig().APPLY_LOAD_BATCH_SAC_COUNT;
    if (batchSize > 1)
    {
        uint32_t numClusters =
            mApp.getConfig().APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS;
        releaseAssert(mBatchTransferInstances.size() == numClusters);

        // Calculate how many batch transfer transactions we need. Wrt to TPS,
        // here we consider one transfer a "transaction"
        uint32_t txsPerCluster = count / numClusters;

        for (uint32_t clusterId = 0; clusterId < numClusters; ++clusterId)
        {
            for (uint32_t i = 0; i < txsPerCluster; ++i)
            {
                // Use a different source account for each transaction to avoid
                // conflicts
                uint32_t accountIdx =
                    (clusterId * txsPerCluster + i) % mNumAccounts;

                auto it = accounts.find(accountIdx);
                releaseAssert(it != accounts.end());

                // Make sure all destination addresses are unique to avoid rw
                // conflicts
                std::vector<SCAddress> destinations;
                destinations.reserve(batchSize);
                for (uint32_t j = 0; j < batchSize; ++j)
                {
                    SCAddress dest(SC_ADDRESS_TYPE_CONTRACT);
                    dest.contractId() = sha256(std::to_string(mDestCounter++));
                    destinations.push_back(dest);
                }

                // Create batch transfer transaction
                auto tx = mTxGenerator.invokeBatchTransfer(
                    lm.getLastClosedLedgerNum() + 1, accountIdx,
                    mBatchTransferInstances[clusterId], mSACInstanceXLM,
                    destinations);

                txs.push_back(tx.second);
            }
        }
    }
    else
    {
        // Individual transfers via direct SAC invocation
        for (uint32_t i = 0; i < count; ++i)
        {
            SCAddress toAddress(SC_ADDRESS_TYPE_CONTRACT);
            toAddress.contractId() = sha256(
                fmt::format("dest_{}_{}", i, lm.getLastClosedLedgerNum()));

            // Use a different account for each transaction to avoid conflicts
            uint32_t accountIdx = i % mNumAccounts;
            auto it = accounts.find(accountIdx);
            releaseAssert(it != accounts.end());

            auto tx = mTxGenerator.invokeSACPayment(
                lm.getLastClosedLedgerNum() + 1, accountIdx, toAddress,
                mSACInstanceXLM, 100, 1'000'000);

            txs.push_back(tx.second);
        }
    }
}
}
