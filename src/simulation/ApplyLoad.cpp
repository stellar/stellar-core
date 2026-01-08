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
#include "util/MetricsRegistry.h"
#include "util/types.h"

#include "herder/HerderImpl.h"

#include "bucket/BucketListSnapshotBase.h"
#include "bucket/BucketSnapshotManager.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/XDRCereal.h"
#include "xdrpp/printer.h"
#include <crypto/SHA.h>

namespace stellar
{
namespace
{
SorobanUpgradeConfig
getUpgradeConfig(Config const& cfg, bool validate = true)
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
        cfg.APPLY_LOAD_LEDGER_MAX_DISK_READ_LEDGER_ENTRIES;
    upgradeConfig.ledgerMaxDiskReadBytes =
        cfg.APPLY_LOAD_LEDGER_MAX_DISK_READ_BYTES;
    upgradeConfig.ledgerMaxWriteLedgerEntries =
        cfg.APPLY_LOAD_LEDGER_MAX_WRITE_LEDGER_ENTRIES;
    upgradeConfig.ledgerMaxWriteBytes = cfg.APPLY_LOAD_LEDGER_MAX_WRITE_BYTES;
    upgradeConfig.ledgerMaxTxCount = cfg.APPLY_LOAD_MAX_SOROBAN_TX_COUNT;
    upgradeConfig.txMaxDiskReadEntries =
        cfg.APPLY_LOAD_TX_MAX_DISK_READ_LEDGER_ENTRIES;
    upgradeConfig.txMaxFootprintEntries = cfg.APPLY_LOAD_TX_MAX_FOOTPRINT_SIZE;
    upgradeConfig.txMaxDiskReadBytes = cfg.APPLY_LOAD_TX_MAX_DISK_READ_BYTES;
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
    if (validate)
    {
        releaseAssert(*upgradeConfig.ledgerMaxInstructions > 0);
        releaseAssert(*upgradeConfig.ledgerMaxDiskReadEntries > 0);
        releaseAssert(*upgradeConfig.ledgerMaxDiskReadBytes > 0);
        releaseAssert(*upgradeConfig.ledgerMaxWriteLedgerEntries > 0);
        releaseAssert(*upgradeConfig.ledgerMaxWriteBytes > 0);
        releaseAssert(*upgradeConfig.ledgerMaxTransactionsSizeBytes > 0);
        releaseAssert(*upgradeConfig.ledgerMaxTxCount > 0);
        releaseAssert(*upgradeConfig.txMaxInstructions > 0);
        releaseAssert(*upgradeConfig.txMaxDiskReadEntries > 0);
        releaseAssert(*upgradeConfig.txMaxDiskReadBytes > 0);
        releaseAssert(*upgradeConfig.txMaxWriteLedgerEntries > 0);
        releaseAssert(*upgradeConfig.txMaxWriteBytes > 0);
        releaseAssert(*upgradeConfig.txMaxContractEventsSizeBytes > 0);
        releaseAssert(*upgradeConfig.txMaxSizeBytes > 0);
    }
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

    static SCAddress const hotArchiveContractID = [] {
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
ApplyLoad::calculateRequiredHotArchiveEntries(ApplyLoadMode mode,
                                              Config const& cfg)
{
    // If no RO entries are configured, return 0
    if (cfg.APPLY_LOAD_NUM_DISK_READ_ENTRIES.empty())
    {
        return 0;
    }

    releaseAssertOrThrow(
        cfg.APPLY_LOAD_NUM_DISK_READ_ENTRIES.size() ==
        cfg.APPLY_LOAD_NUM_DISK_READ_ENTRIES_DISTRIBUTION.size());

    // Calculate mean disk reads per transaction
    double totalWeight = std::accumulate(
        cfg.APPLY_LOAD_NUM_DISK_READ_ENTRIES_DISTRIBUTION.begin(),
        cfg.APPLY_LOAD_NUM_DISK_READ_ENTRIES_DISTRIBUTION.end(), 0.0);
    double meanDiskReadsPerTx = 0.0;
    for (size_t i = 0; i < cfg.APPLY_LOAD_NUM_DISK_READ_ENTRIES.size(); ++i)
    {
        meanDiskReadsPerTx +=
            static_cast<double>(cfg.APPLY_LOAD_NUM_DISK_READ_ENTRIES[i]) *
            (cfg.APPLY_LOAD_NUM_DISK_READ_ENTRIES_DISTRIBUTION[i] /
             totalWeight);
    }

    // Calculate total expected disk reads
    double totalExpectedRestores = meanDiskReadsPerTx *
                                   cfg.APPLY_LOAD_MAX_SOROBAN_TX_COUNT *
                                   cfg.APPLY_LOAD_NUM_LEDGERS;
    // We technically can only actually perform totalExpectedRestores, but we
    // still need to create valid transactions in the 'mempool', so we need
    // to scale the expected number of restores by the transaction queue size.
    totalExpectedRestores *= cfg.SOROBAN_TRANSACTION_QUEUE_SIZE_MULTIPLIER;

    // In FIND_LIMITS_FOR_MODEL_TX mode, we perform a binary search that uses
    // new restores and thus we need to additionally scale the restores by
    // log2 of max tx count (which approximates the maximum number of binary
    // search iterations).
    if (mode == ApplyLoadMode::FIND_LIMITS_FOR_MODEL_TX)
    {
        totalExpectedRestores *= log2(cfg.APPLY_LOAD_MAX_SOROBAN_TX_COUNT);
    }

    // Add some generous buffer since actual distributions may vary.
    return totalExpectedRestores * 1.5;
}

ApplyLoad::ApplyLoad(Application& app, ApplyLoadMode mode)
    : mApp(app)
    , mMode(mode)
    , mRoot(app.getRoot())
    , mTotalHotArchiveEntries(
          calculateRequiredHotArchiveEntries(mode, app.getConfig()))
    , mTxCountUtilization(
          mApp.getMetrics().NewHistogram({"soroban", "apply-load", "tx-count"}))
    , mInstructionUtilization(mApp.getMetrics().NewHistogram(
          {"soroban", "apply-load", "instructions"}))
    , mTxSizeUtilization(
          mApp.getMetrics().NewHistogram({"soroban", "apply-load", "tx-size"}))
    , mDiskReadByteUtilization(mApp.getMetrics().NewHistogram(
          {"soroban", "apply-load", "disk-read-byte"}))
    , mWriteByteUtilization(mApp.getMetrics().NewHistogram(
          {"soroban", "apply-load", "write-byte"}))
    , mDiskReadEntryUtilization(mApp.getMetrics().NewHistogram(
          {"soroban", "apply-load", "disk-read-entry"}))
    , mWriteEntryUtilization(mApp.getMetrics().NewHistogram(
          {"soroban", "apply-load", "write-entry"}))
    , mTxGenerator(app, mTotalHotArchiveEntries)
{
    auto const& config = mApp.getConfig();

    switch (mMode)
    {
    case ApplyLoadMode::LIMIT_BASED:
    case ApplyLoadMode::FIND_LIMITS_FOR_MODEL_TX:
        mNumAccounts = config.APPLY_LOAD_MAX_SOROBAN_TX_COUNT *
                           config.SOROBAN_TRANSACTION_QUEUE_SIZE_MULTIPLIER +
                       config.APPLY_LOAD_CLASSIC_TXS_PER_LEDGER *
                           config.TRANSACTION_QUEUE_SIZE_MULTIPLIER +
                       2;
        break;
    case ApplyLoadMode::MAX_SAC_TPS:
        mNumAccounts = config.APPLY_LOAD_MAX_SAC_TPS_MAX_TPS *
                       config.SOROBAN_TRANSACTION_QUEUE_SIZE_MULTIPLIER *
                       config.APPLY_LOAD_TARGET_CLOSE_TIME_MS / 1000.0;
        break;
    }
    if (config.APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS == 0)
    {
        throw std::runtime_error(
            "APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS cannot be zero");
    }
    setup();
}

void
ApplyLoad::setup()
{
    releaseAssert(mTxGenerator.loadAccount(mRoot));

    if (mApp.getLedgerManager()
            .getLastClosedLedgerHeader()
            .header.maxTxSetSize <
        mApp.getConfig().APPLY_LOAD_CLASSIC_TXS_PER_LEDGER)
    {
        auto upgrade = xdr::xvector<UpgradeType, 6>{};

        LedgerUpgrade ledgerUpgrade;
        ledgerUpgrade.type(LEDGER_UPGRADE_MAX_TX_SET_SIZE);
        ledgerUpgrade.newMaxTxSetSize() =
            mApp.getConfig().APPLY_LOAD_CLASSIC_TXS_PER_LEDGER;
        auto v = xdr::xdr_to_opaque(ledgerUpgrade);
        upgrade.push_back(UpgradeType{v.begin(), v.end()});
        closeLedger({}, upgrade);
    }

    setupAccounts();

    setupUpgradeContract();

    switch (mMode)
    {
    case ApplyLoadMode::MAX_SAC_TPS:
    case ApplyLoadMode::FIND_LIMITS_FOR_MODEL_TX:
        // Just upgrade to a placeholder number of TXs, we'll
        // upgrade again before each TPS run.
        upgradeSettingsForMaxTPS(100000);
        break;
    case ApplyLoadMode::LIMIT_BASED:
        upgradeSettings();
        break;
    }

    setupLoadContract();
    setupXLMContract();
    if (mMode == ApplyLoadMode::MAX_SAC_TPS &&
        mApp.getConfig().APPLY_LOAD_BATCH_SAC_COUNT > 1)
    {
        setupBatchTransferContracts();
    }
    if (mMode == ApplyLoadMode::LIMIT_BASED ||
        mMode == ApplyLoadMode::FIND_LIMITS_FOR_MODEL_TX)
    {
        setupBucketList();
    }
}

void
ApplyLoad::closeLedger(std::vector<TransactionFrameBasePtr> const& txs,
                       xdr::xvector<UpgradeType, 6> const& upgrades,
                       bool recordSorobanUtilization)
{
    auto txSet = makeTxSetFromTransactions(txs, mApp, 0, 0);

    if (recordSorobanUtilization)
    {
        auto ledgerResources = mApp.getLedgerManager().maxLedgerResources(true);
        auto txSetResources =
            txSet.second->getPhases()
                .at(static_cast<size_t>(TxSetPhase::SOROBAN))
                .getTotalResources(mApp.getLedgerManager()
                                       .getLastClosedLedgerHeader()
                                       .header.ledgerVersion)
                .value();
        mTxCountUtilization.Update(
            txSetResources.getVal(Resource::Type::OPERATIONS) * 1.0 /
            ledgerResources.getVal(Resource::Type::OPERATIONS) * 100000.0);
        mInstructionUtilization.Update(
            txSetResources.getVal(Resource::Type::INSTRUCTIONS) * 1.0 /
            ledgerResources.getVal(Resource::Type::INSTRUCTIONS) * 100000.0);
        mTxSizeUtilization.Update(
            txSetResources.getVal(Resource::Type::TX_BYTE_SIZE) * 1.0 /
            ledgerResources.getVal(Resource::Type::TX_BYTE_SIZE) * 100000.0);
        mDiskReadByteUtilization.Update(
            txSetResources.getVal(Resource::Type::DISK_READ_BYTES) * 1.0 /
            ledgerResources.getVal(Resource::Type::DISK_READ_BYTES) * 100000.0);
        mWriteByteUtilization.Update(
            txSetResources.getVal(Resource::Type::WRITE_BYTES) * 1.0 /
            ledgerResources.getVal(Resource::Type::WRITE_BYTES) * 100000.0);
        mDiskReadEntryUtilization.Update(
            txSetResources.getVal(Resource::Type::READ_LEDGER_ENTRIES) * 1.0 /
            ledgerResources.getVal(Resource::Type::READ_LEDGER_ENTRIES) *
            100000.0);
        mWriteEntryUtilization.Update(
            txSetResources.getVal(Resource::Type::WRITE_LEDGER_ENTRIES) * 1.0 /
            ledgerResources.getVal(Resource::Type::WRITE_LEDGER_ENTRIES) *
            100000.0);
        CLOG_INFO(Perf, "generated tx set resources: {}/{}",
                  txSetResources.toString(), ledgerResources.toString());
    }
    auto sv =
        mApp.getHerder().makeStellarValue(txSet.first->getContentsHash(), 1,
                                          upgrades, mApp.getConfig().NODE_SEED);

    stellar::txtest::closeLedger(mApp, txs, /* strictOrder */ false, upgrades);
}

void
ApplyLoad::execute()
{
    switch (mMode)
    {
    case ApplyLoadMode::LIMIT_BASED:
        benchmarkLimits();
        break;
    case ApplyLoadMode::MAX_SAC_TPS:
        findMaxSacTps();
        break;
    case ApplyLoadMode::FIND_LIMITS_FOR_MODEL_TX:
        findMaxLimitsForModelTransaction();
        break;
    }
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
    uploadResources.diskReadBytes = 0;
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
    resources.diskReadBytes = 0;
    resources.writeBytes = 3'100;

    auto [_, invokeTx] = mTxGenerator.invokeSorobanCreateUpgradeTransaction(
        lm.getLastClosedLedgerNum() + 1, TxGenerator::ROOT_ACCOUNT_ID,
        upgradeBytes, mUpgradeCodeKey, mUpgradeInstanceKey, std::nullopt,
        resources);
    {
        LedgerSnapshot ls(mApp);
        auto diagnostics =
            DiagnosticEventManager::createForValidation(mApp.getConfig());
        auto validationRes = invokeTx->checkValid(mApp.getAppConnector(), ls, 0,
                                                  0, 0, diagnostics);
        if (!validationRes->isSuccess())
        {
            if (validationRes->getResultCode() == txSOROBAN_INVALID)
            {
                diagnostics.debugLogEvents();
            }
            CLOG_FATAL(Perf, "Created invalid upgrade settings transaction: {}",
                       validationRes->getResultCode());
            releaseAssert(validationRes->isSuccess());
        }
    }

    auto upgradeSetKey = mTxGenerator.getConfigUpgradeSetKey(
        upgradeConfig,
        mUpgradeInstanceKey.contractData().contract.contractId());

    auto upgrade = xdr::xvector<UpgradeType, 6>{};
    auto ledgerUpgrade = LedgerUpgrade{LEDGER_UPGRADE_CONFIG};
    ledgerUpgrade.newConfig() = upgradeSetKey;
    auto v = xdr::xdr_to_opaque(ledgerUpgrade);
    upgrade.push_back(UpgradeType{v.begin(), v.end()});

    closeLedger({invokeTx}, upgrade);

    releaseAssert(mTxGenerator.getApplySorobanSuccess().count() -
                      currApplySorobanSuccess ==
                  1);
}

std::pair<SorobanUpgradeConfig, uint64_t>
ApplyLoad::updateSettingsForTxCount(uint64_t txsPerLedger)
{
    // Round the configuration values down to be a multiple of the respective
    // step in order to get more readable configurations, and also to speeed
    // up the binary search significantly.
    uint64_t const INSTRUCTIONS_ROUNDING_STEP = 5'000'000;
    uint64_t const SIZE_ROUNDING_STEP = 500;
    uint64_t const ENTRIES_ROUNDING_STEP = 10;

    auto const& config = mApp.getConfig();
    uint64_t insns =
        roundDown(txsPerLedger * config.APPLY_LOAD_INSTRUCTIONS[0] /
                      config.APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS,
                  INSTRUCTIONS_ROUNDING_STEP);
    uint64_t txSize = roundDown(
        txsPerLedger * config.APPLY_LOAD_TX_SIZE_BYTES[0], SIZE_ROUNDING_STEP);

    uint64_t writeEntries =
        roundDown(txsPerLedger * config.APPLY_LOAD_NUM_RW_ENTRIES[0],
                  ENTRIES_ROUNDING_STEP);
    uint64_t writeBytes = roundDown(
        writeEntries * config.APPLY_LOAD_DATA_ENTRY_SIZE, SIZE_ROUNDING_STEP);

    uint64_t diskReadEntries =
        roundDown(txsPerLedger * config.APPLY_LOAD_NUM_DISK_READ_ENTRIES[0],
                  ENTRIES_ROUNDING_STEP);
    uint64_t diskReadBytes =
        roundDown(diskReadEntries * config.APPLY_LOAD_DATA_ENTRY_SIZE,
                  SIZE_ROUNDING_STEP);

    if (diskReadEntries == 0)
    {
        diskReadEntries =
            MinimumSorobanNetworkConfig::TX_MAX_READ_LEDGER_ENTRIES;
        diskReadBytes = MinimumSorobanNetworkConfig::TX_MAX_READ_BYTES;
    }

    uint64_t actualMaxTxs = txsPerLedger;
    actualMaxTxs =
        std::min(actualMaxTxs,
                 insns * config.APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS /
                     config.APPLY_LOAD_INSTRUCTIONS[0]);
    actualMaxTxs =
        std::min(actualMaxTxs, txSize / config.APPLY_LOAD_TX_SIZE_BYTES[0]);
    if (config.APPLY_LOAD_NUM_DISK_READ_ENTRIES[0] > 0)
    {
        actualMaxTxs = std::min(actualMaxTxs,
                                diskReadEntries /
                                    config.APPLY_LOAD_NUM_DISK_READ_ENTRIES[0]);
        actualMaxTxs = std::min(
            actualMaxTxs,
            diskReadBytes / (config.APPLY_LOAD_NUM_DISK_READ_ENTRIES[0] *
                             config.APPLY_LOAD_DATA_ENTRY_SIZE));
    }
    actualMaxTxs = std::min(actualMaxTxs,
                            writeEntries / config.APPLY_LOAD_NUM_RW_ENTRIES[0]);

    actualMaxTxs = std::min(actualMaxTxs,
                            writeBytes / (config.APPLY_LOAD_NUM_RW_ENTRIES[0] *
                                          config.APPLY_LOAD_DATA_ENTRY_SIZE));
    CLOG_INFO(Perf,
              "Resources after rounding for testing {} actual max txs per "
              "ledger: "
              "instructions {}, tx size {}, disk read entries {}, "
              "disk read bytes {}, rw entries {}, rw bytes {}",
              actualMaxTxs, insns, txSize, diskReadEntries, diskReadBytes,
              writeEntries, writeBytes);

    auto upgradeConfig = getUpgradeConfig(mApp.getConfig(),
                                          /* validate */ false);
    // Set tx limits to the respective resources of the 'model'
    // transaction.
    upgradeConfig.txMaxInstructions =
        std::max(MinimumSorobanNetworkConfig::TX_MAX_INSTRUCTIONS,
                 config.APPLY_LOAD_INSTRUCTIONS[0]);
    upgradeConfig.txMaxSizeBytes =
        std::max(MinimumSorobanNetworkConfig::TX_MAX_SIZE_BYTES,
                 config.APPLY_LOAD_TX_SIZE_BYTES[0]);
    upgradeConfig.txMaxDiskReadEntries =
        std::max(MinimumSorobanNetworkConfig::TX_MAX_READ_LEDGER_ENTRIES,
                 config.APPLY_LOAD_NUM_DISK_READ_ENTRIES[0]);
    upgradeConfig.txMaxWriteLedgerEntries =
        std::max(MinimumSorobanNetworkConfig::TX_MAX_WRITE_LEDGER_ENTRIES,
                 config.APPLY_LOAD_NUM_RW_ENTRIES[0]);
    upgradeConfig.txMaxDiskReadBytes =
        std::max(MinimumSorobanNetworkConfig::TX_MAX_READ_BYTES,
                 config.APPLY_LOAD_NUM_DISK_READ_ENTRIES[0] *
                     config.APPLY_LOAD_DATA_ENTRY_SIZE);
    upgradeConfig.txMaxWriteBytes =
        std::max(MinimumSorobanNetworkConfig::TX_MAX_WRITE_BYTES,
                 config.APPLY_LOAD_NUM_RW_ENTRIES[0] *
                     config.APPLY_LOAD_DATA_ENTRY_SIZE);
    upgradeConfig.txMaxContractEventsSizeBytes =
        std::max(MinimumSorobanNetworkConfig::TX_MAX_CONTRACT_EVENTS_SIZE_BYTES,
                 config.APPLY_LOAD_EVENT_COUNT[0] *
                         TxGenerator::SOROBAN_LOAD_V2_EVENT_SIZE_BYTES +
                     100);
    upgradeConfig.txMaxFootprintEntries =
        *upgradeConfig.txMaxDiskReadEntries +
        *upgradeConfig.txMaxWriteLedgerEntries;

    // Set the ledger-wide limits to the compute values calculated above.
    // Note, that in theory we could end up with ledger limits lower than
    // the transaction limits, but in normally would be just
    // mis-configuration (using a model transaction that is too large to
    // be applied within the target close time).
    upgradeConfig.ledgerMaxInstructions = insns;
    upgradeConfig.ledgerMaxTransactionsSizeBytes = txSize;
    upgradeConfig.ledgerMaxDiskReadEntries = diskReadEntries;
    upgradeConfig.ledgerMaxWriteLedgerEntries = writeEntries;
    upgradeConfig.ledgerMaxDiskReadBytes = diskReadBytes;
    upgradeConfig.ledgerMaxWriteBytes = writeBytes;

    return std::make_pair(upgradeConfig, actualMaxTxs);
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
    auto wasm = rust_bridge::get_test_contract_sac_transfer(
        mApp.getConfig().LEDGER_PROTOCOL_VERSION);
    xdr::opaque_vec<> wasmBytes;
    wasmBytes.assign(wasm.data.begin(), wasm.data.end());

    LedgerKey contractCodeLedgerKey;
    contractCodeLedgerKey.type(CONTRACT_CODE);
    contractCodeLedgerKey.contractCode().hash = sha256(wasmBytes);

    SorobanResources uploadResources;
    uploadResources.instructions = 5000000;
    uploadResources.diskReadBytes = 0;
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
    if (mDataEntrySize < mApp.getConfig().APPLY_LOAD_DATA_ENTRY_SIZE)
    {
        baseLiveEntry.data.contractData().val.bytes().resize(
            mApp.getConfig().APPLY_LOAD_DATA_ENTRY_SIZE - mDataEntrySize);
        mDataEntrySize = mApp.getConfig().APPLY_LOAD_DATA_ENTRY_SIZE;
        releaseAssertOrThrow(xdr::xdr_size(baseLiveEntry) == mDataEntrySize);
    }
    else
    {
        CLOG_WARNING(Perf,
                     "Apply load generated entry size is larger than "
                     "APPLY_LOAD_DATA_ENTRY_SIZE: {} > {}",
                     mApp.getConfig().APPLY_LOAD_DATA_ENTRY_SIZE,
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
            ? ceil(static_cast<double>(
                       mTotalHotArchiveEntries -
                       (hotArchiveBatchSize * hotArchiveBatchCount)) /
                   cfg.APPLY_LOAD_BL_LAST_BATCH_LEDGERS)
            : 0;

    CLOG_INFO(Perf,
              "Apply load: Hot Archive BL setup: total entries {}, total "
              "batches {}, batch size {}, last batch size {}",
              mTotalHotArchiveEntries, totalBatchCount, hotArchiveBatchSize,
              hotArchiveLastBatchSize);

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
    mApp.getPersistentState().setMainState(
        PersistentState::kHistoryArchiveState, has.toString(),
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
ApplyLoad::benchmarkLimits()
{
    auto& ledgerClose =
        mApp.getMetrics().NewTimer({"ledger", "ledger", "close"});
    ledgerClose.Clear();

    auto& cpuInsRatio = mApp.getMetrics().NewHistogram(
        {"soroban", "host-fn-op", "invoke-time-fsecs-cpu-insn-ratio"});
    cpuInsRatio.Clear();

    auto& cpuInsRatioExclVm = mApp.getMetrics().NewHistogram(
        {"soroban", "host-fn-op", "invoke-time-fsecs-cpu-insn-ratio-excl-vm"});
    cpuInsRatioExclVm.Clear();

    auto& ledgerCpuInsRatio = mApp.getMetrics().NewHistogram(
        {"soroban", "host-fn-op", "ledger-cpu-insns-ratio"});
    ledgerCpuInsRatio.Clear();

    auto& ledgerCpuInsRatioExclVm = mApp.getMetrics().NewHistogram(
        {"soroban", "host-fn-op", "ledger-cpu-insns-ratio-excl-vm"});
    ledgerCpuInsRatioExclVm.Clear();

    auto& totalTxApplyTime =
        mApp.getMetrics().NewTimer({"ledger", "transaction", "total-apply"});
    totalTxApplyTime.Clear();

    for (size_t i = 0; i < mApp.getConfig().APPLY_LOAD_NUM_LEDGERS; ++i)
    {
        benchmarkLimitsIteration();
    }
    CLOG_INFO(Perf,
              "Ledger close min/avg/max: {}/{}/{} milliseconds "
              "(stddev={})",
              ledgerClose.min(), ledgerClose.mean(), ledgerClose.max(),
              ledgerClose.std_dev());
    CLOG_INFO(Perf,
              "Tx apply time min/avg/max: {}/{}/{} milliseconds "
              "(stddev={})",
              totalTxApplyTime.min(), totalTxApplyTime.mean(),
              totalTxApplyTime.max(), totalTxApplyTime.std_dev());

    CLOG_INFO(Perf, "Max CPU ins ratio: {}", cpuInsRatio.max() / 1000000);
    CLOG_INFO(Perf, "Mean CPU ins ratio:  {}", cpuInsRatio.mean() / 1000000);

    CLOG_INFO(Perf, "Max CPU ins ratio excl VM: {}",
              cpuInsRatioExclVm.max() / 1000000);
    CLOG_INFO(Perf, "Mean CPU ins ratio excl VM:  {}",
              cpuInsRatioExclVm.mean() / 1000000);
    CLOG_INFO(Perf, "stddev CPU ins ratio excl VM:  {}",
              cpuInsRatioExclVm.std_dev() / 1000000);

    CLOG_INFO(Perf, "Ledger Max CPU ins ratio: {}",
              ledgerCpuInsRatio.max() / 1000000);
    CLOG_INFO(Perf, "Ledger Mean CPU ins ratio:  {}",
              ledgerCpuInsRatio.mean() / 1000000);
    CLOG_INFO(Perf, "Ledger stddev CPU ins ratio:  {}",
              ledgerCpuInsRatio.std_dev() / 1000000);

    CLOG_INFO(Perf, "Ledger Max CPU ins ratio excl VM: {}",
              ledgerCpuInsRatioExclVm.max() / 1000000);
    CLOG_INFO(Perf, "Ledger Mean CPU ins ratio excl VM:  {}",
              ledgerCpuInsRatioExclVm.mean() / 1000000);
    CLOG_INFO(Perf, "Ledger stddev CPU ins ratio excl VM:  {} milliseconds",
              ledgerCpuInsRatioExclVm.std_dev() / 1000000);
    CLOG_INFO(Perf, "Tx count utilization min/avg/max {}/{}/{}%",
              getTxCountUtilization().min() / 1000.0,
              getTxCountUtilization().mean() / 1000.0,
              getTxCountUtilization().max() / 1000.0);
    CLOG_INFO(Perf, "Instruction utilization min/avg/max {}/{}/{}%",
              getInstructionUtilization().min() / 1000.0,
              getInstructionUtilization().mean() / 1000.0,
              getInstructionUtilization().max() / 1000.0);
    CLOG_INFO(Perf, "Tx size utilization min/avg/max {}/{}/{}%",
              getTxSizeUtilization().min() / 1000.0,
              getTxSizeUtilization().mean() / 1000.0,
              getTxSizeUtilization().max() / 1000.0);
    CLOG_INFO(Perf, "Disk read bytes utilization min/avg/max {}/{}/{}%",
              getDiskReadByteUtilization().min() / 1000.0,
              getDiskReadByteUtilization().mean() / 1000.0,
              getDiskReadByteUtilization().max() / 1000.0);
    CLOG_INFO(Perf, "Write bytes utilization min/avg/max {}/{}/{}%",
              getDiskWriteByteUtilization().min() / 1000.0,
              getDiskWriteByteUtilization().mean() / 1000.0,
              getDiskWriteByteUtilization().max() / 1000.0);
    CLOG_INFO(Perf, "Disk read entry utilization min/avg/max {}/{}/{}%",
              getDiskReadEntryUtilization().min() / 1000.0,
              getDiskReadEntryUtilization().mean() / 1000.0,
              getDiskReadEntryUtilization().max() / 1000.0);
    CLOG_INFO(Perf, "Write entry utilization min/avg/max {}/{}/{}%",
              getWriteEntryUtilization().min() / 1000.0,
              getWriteEntryUtilization().mean() / 1000.0,
              getWriteEntryUtilization().max() / 1000.0);

    CLOG_INFO(Perf, "Tx Success Rate: {:f}%", successRate() * 100);
}

void
ApplyLoad::benchmarkLimitsIteration()
{
    releaseAssert(mMode != ApplyLoadMode::MAX_SAC_TPS);

    mApp.getBucketManager().getLiveBucketList().resolveAllFutures();
    releaseAssert(
        mApp.getBucketManager().getLiveBucketList().futuresAllResolved());

    auto& lm = mApp.getLedgerManager();
    auto const& config = mApp.getConfig();
    std::vector<TransactionFrameBasePtr> txs;

    auto maxResourcesToGenerate = lm.maxLedgerResources(true);
    // The TxSet validation will compare the ledger instruction limit
    // against the sum of the instructions of the slowest cluster in each
    // stage, so we just multiply the instructions limit by the max number
    // of clusters.
    maxResourcesToGenerate.setVal(
        Resource::Type::INSTRUCTIONS,
        maxResourcesToGenerate.getVal(Resource::Type::INSTRUCTIONS) *
            config.APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS);
    // Scale the resources by the tx queue multipler to emulate filled
    // mempool.
    maxResourcesToGenerate =
        multiplyByDouble(maxResourcesToGenerate,
                         config.SOROBAN_TRANSACTION_QUEUE_SIZE_MULTIPLIER);

    CLOG_INFO(Perf, "benchmark max generation resources: {}",
              maxResourcesToGenerate.toString());
    auto resourcesLeft = maxResourcesToGenerate;

    auto const& accounts = mTxGenerator.getAccounts();
    // Omit root account
    std::vector<uint64_t> shuffledAccounts(accounts.size() - 1);
    std::iota(shuffledAccounts.begin(), shuffledAccounts.end(), 0);
    stellar::shuffle(std::begin(shuffledAccounts), std::end(shuffledAccounts),
                     getGlobalRandomEngine());

    LedgerSnapshot ls(mApp);
    auto appConnector = mApp.getAppConnector();

    auto addTx = [&ls, &appConnector, &txs](TransactionFrameBasePtr tx) {
        auto diagnostics = DiagnosticEventManager::createDisabled();
        auto res = tx->checkValid(appConnector, ls, 0, 0, 0, diagnostics);
        releaseAssert(res && res->isSuccess());
        txs.emplace_back(tx);
    };

    releaseAssert(shuffledAccounts.size() >=
                  config.APPLY_LOAD_CLASSIC_TXS_PER_LEDGER);
    for (size_t i = 0; i < config.APPLY_LOAD_CLASSIC_TXS_PER_LEDGER; ++i)
    {
        auto it = accounts.find(shuffledAccounts[i]);
        releaseAssert(it != accounts.end());
        it->second->loadSequenceNumber();
        auto [_, tx] = mTxGenerator.paymentTransaction(
            mNumAccounts, 0, lm.getLastClosedLedgerNum() + 1, it->first, 1,
            std::nullopt);
        addTx(tx);
    }

    bool sorobanLimitHit = false;
    for (size_t i = config.APPLY_LOAD_CLASSIC_TXS_PER_LEDGER;
         i < shuffledAccounts.size(); ++i)
    {
        auto it = accounts.find(shuffledAccounts[i]);
        releaseAssert(it != accounts.end());

        auto [_, tx] = mTxGenerator.invokeSorobanLoadTransactionV2(
            lm.getLastClosedLedgerNum() + 1, it->first, mLoadInstance,
            mDataEntryCount, mDataEntrySize, 1'000'000);

        uint32_t ledgerVersion = mApp.getLedgerManager()
                                     .getLastClosedLedgerHeader()
                                     .header.ledgerVersion;
        auto txResources = tx->getResources(false, ledgerVersion);
        if (!anyGreater(txResources, resourcesLeft))
        {
            resourcesLeft -= txResources;
        }
        else
        {
            for (size_t i = 0; i < resourcesLeft.size(); ++i)
            {
                auto type = static_cast<Resource::Type>(i);
                if (txResources.getVal(type) > resourcesLeft.getVal(type))
                {
                    auto resourcesGenerated = maxResourcesToGenerate;
                    resourcesGenerated -= resourcesLeft;
                    CLOG_INFO(Perf,
                              "Ledger {} limit hit during tx generation, "
                              "total resources generated: {}, not fitting tx "
                              "resources: {}",
                              Resource::getStringFromType(type),
                              resourcesGenerated.toString(),
                              txResources.toString());
                    sorobanLimitHit = true;
                }
            }

            break;
        }
        addTx(tx);
    }
    // If this assert fails, it most likely means that we ran out of
    // accounts, which should not happen.
    releaseAssert(sorobanLimitHit);

    closeLedger(txs, {}, /* recordSorobanUtilization */ true);
}

void
ApplyLoad::findMaxLimitsForModelTransaction()
{
    auto const& config = mApp.getConfig();

    auto validateTxParam = [&config](std::string const& paramName,
                                     auto const& values, auto const& weights,
                                     bool allowZeroValue = false) {
        if (values.size() != 1)
        {
            throw std::runtime_error(
                fmt::format(FMT_STRING("{} must have exactly one entry for "
                                       "'limits-for-model-tx' mode"),
                            paramName));
        }
        if (!allowZeroValue && values[0] == 0)
        {
            throw std::runtime_error(fmt::format(
                FMT_STRING("{} cannot be zero for 'limits-for-model-tx' mode"),
                paramName));
        }
        if (weights.size() != 1 || weights[0] != 1)
        {
            throw std::runtime_error(
                fmt::format(FMT_STRING("{}_DISTRIBUTION must have exactly one "
                                       "entry with the value of 1 for "
                                       "'limits-for-model-tx' mode"),
                            paramName));
        }
    };
    validateTxParam("APPLY_LOAD_INSTRUCTIONS", config.APPLY_LOAD_INSTRUCTIONS,
                    config.APPLY_LOAD_INSTRUCTIONS_DISTRIBUTION);
    validateTxParam("APPLY_LOAD_TX_SIZE_BYTES", config.APPLY_LOAD_TX_SIZE_BYTES,
                    config.APPLY_LOAD_TX_SIZE_BYTES_DISTRIBUTION);
    validateTxParam("APPLY_LOAD_NUM_DISK_READ_ENTRIES",
                    config.APPLY_LOAD_NUM_DISK_READ_ENTRIES,
                    config.APPLY_LOAD_NUM_DISK_READ_ENTRIES_DISTRIBUTION, true);
    validateTxParam("APPLY_LOAD_NUM_RW_ENTRIES",
                    config.APPLY_LOAD_NUM_RW_ENTRIES,
                    config.APPLY_LOAD_NUM_RW_ENTRIES_DISTRIBUTION);
    validateTxParam("APPLY_LOAD_EVENT_COUNT", config.APPLY_LOAD_EVENT_COUNT,
                    config.APPLY_LOAD_EVENT_COUNT_DISTRIBUTION, true);

    auto roundDown = [](uint64_t value, uint64_t step) {
        return value - value % step;
    };

    auto& ledgerCloseTime =
        mApp.getMetrics().NewTimer({"ledger", "ledger", "close"});

    uint64_t minTxsPerLedger = 1;
    uint64_t maxTxsPerLedger = mApp.getConfig().APPLY_LOAD_MAX_SOROBAN_TX_COUNT;
    SorobanUpgradeConfig maxLimitsConfig;
    uint64_t maxLimitsTxsPerLedger = 0;
    uint64_t prevTxsPerLedger = 0;

    double targetTimeMs = mApp.getConfig().APPLY_LOAD_TARGET_CLOSE_TIME_MS;

    while (minTxsPerLedger <= maxTxsPerLedger)
    {
        uint64_t testTxsPerLedger = (minTxsPerLedger + maxTxsPerLedger) / 2;

        CLOG_INFO(Perf,
                  "Testing ledger max model txs: {}, generated limits: "
                  "instructions {}, tx size {}, disk read entries {}, rw "
                  "entries {}",
                  testTxsPerLedger,
                  testTxsPerLedger * config.APPLY_LOAD_INSTRUCTIONS[0],
                  testTxsPerLedger * config.APPLY_LOAD_TX_SIZE_BYTES[0],
                  testTxsPerLedger * config.APPLY_LOAD_NUM_DISK_READ_ENTRIES[0],
                  testTxsPerLedger * config.APPLY_LOAD_NUM_RW_ENTRIES[0]);
        auto [upgradeConfig, actualMaxTxsPerLedger] =
            updateSettingsForTxCount(testTxsPerLedger);
        // Break when due to rounding we've arrived at the same actual txs to
        // test as in the previous iteration, or at the value lower than the
        // best found so far.
        if (actualMaxTxsPerLedger == prevTxsPerLedger ||
            actualMaxTxsPerLedger <= maxLimitsTxsPerLedger)
        {
            CLOG_INFO(Perf, "No change in generated limits after update due to "
                            "rounding, ending search.");
            break;
        }
        applyConfigUpgrade(upgradeConfig);

        prevTxsPerLedger = actualMaxTxsPerLedger;
        ledgerCloseTime.Clear();
        for (size_t i = 0; i < mApp.getConfig().APPLY_LOAD_NUM_LEDGERS; ++i)
        {
            benchmarkLimitsIteration();
        }
        releaseAssert(successRate() == 1.0);
        if (ledgerCloseTime.mean() > targetTimeMs)
        {
            CLOG_INFO(
                Perf,
                "Failed: {} model txs per ledger (avg close time: {:.2f}ms)",
                actualMaxTxsPerLedger, ledgerCloseTime.mean());
            maxTxsPerLedger = testTxsPerLedger - 1;
        }
        else
        {
            CLOG_INFO(Perf,
                      "Success: {} model txs per ledger (avg close time: "
                      "{:.2f}ms)",
                      actualMaxTxsPerLedger, ledgerCloseTime.mean());
            minTxsPerLedger = testTxsPerLedger + 1;
            maxLimitsTxsPerLedger = actualMaxTxsPerLedger;
            maxLimitsConfig = upgradeConfig;
        }
    }
    CLOG_INFO(Perf,
              "Maximum limits found for model transaction ({} TPL): "
              "instructions {}, "
              "tx size {}, disk read entries {}, disk read bytes {}, "
              "write entries {}, write bytes {}",
              maxLimitsTxsPerLedger, *maxLimitsConfig.ledgerMaxInstructions,
              *maxLimitsConfig.ledgerMaxTransactionsSizeBytes,
              *maxLimitsConfig.ledgerMaxDiskReadEntries,
              *maxLimitsConfig.ledgerMaxDiskReadBytes,
              *maxLimitsConfig.ledgerMaxWriteLedgerEntries,
              *maxLimitsConfig.ledgerMaxWriteBytes);
}

double
ApplyLoad::successRate()
{
    auto& success =
        mApp.getMetrics().NewCounter({"ledger", "apply", "success"});
    auto& failure =
        mApp.getMetrics().NewCounter({"ledger", "apply", "failure"});
    return success.count() * 1.0 / (success.count() + failure.count());
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
ApplyLoad::getDiskReadByteUtilization()
{
    return mDiskReadByteUtilization;
}
medida::Histogram const&
ApplyLoad::getDiskWriteByteUtilization()
{
    return mWriteByteUtilization;
}
medida::Histogram const&
ApplyLoad::getDiskReadEntryUtilization()
{
    return mDiskReadEntryUtilization;
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
    uint32_t const MIN_TXS_PER_STEP = 64;
    releaseAssertOrThrow(mMode == ApplyLoadMode::MAX_SAC_TPS);

    uint32_t numClusters =
        mApp.getConfig().APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS;
    uint32_t txsPerStep =
        numClusters * mApp.getConfig().APPLY_LOAD_BATCH_SAC_COUNT;
    if (txsPerStep < MIN_TXS_PER_STEP)
    {
        txsPerStep =
            std::ceil(static_cast<double>(MIN_TXS_PER_STEP) / txsPerStep) *
            txsPerStep;
    }
    uint32_t stepsPerSecond = 1000 / ApplyLoad::TARGET_CLOSE_TIME_STEP_MS;
    // Round min and max rate of txs per step of TARGET_CLOSE_TIME_STEP_MS
    // duration to be multiple of txsPerStep.
    uint32_t minTxRateSteps =
        std::max(1u, mApp.getConfig().APPLY_LOAD_MAX_SAC_TPS_MIN_TPS /
                         stepsPerSecond / txsPerStep);
    uint32_t maxTxRateSteps = std::ceil(
        static_cast<double>(mApp.getConfig().APPLY_LOAD_MAX_SAC_TPS_MAX_TPS) /
        stepsPerSecond / txsPerStep);
    uint32_t bestTps = 0;

    double targetCloseTimeMs = mApp.getConfig().APPLY_LOAD_TARGET_CLOSE_TIME_MS;
    uint32_t targetCloseTimeSteps =
        mApp.getConfig().APPLY_LOAD_TARGET_CLOSE_TIME_MS /
        ApplyLoad::TARGET_CLOSE_TIME_STEP_MS;

    auto txsPerLedgerToTPS =
        [targetCloseTimeMs](uint32_t txsPerLedger) -> uint32_t {
        double targetCloseTimeSec = targetCloseTimeMs / 1000.0;
        return txsPerLedger / targetCloseTimeSec;
    };

    CLOG_WARNING(Perf,
                 "Starting MAX_SAC_TPS binary search between {} and {} TPS "
                 "with search step of {} txs",
                 txsPerLedgerToTPS(minTxRateSteps * txsPerStep),
                 txsPerLedgerToTPS(maxTxRateSteps * txsPerStep), txsPerStep);
    CLOG_WARNING(Perf, "Target close time: {}ms", targetCloseTimeMs);
    CLOG_WARNING(Perf, "Num parallel clusters: {}",
                 mApp.getConfig().APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS);

    while (minTxRateSteps <= maxTxRateSteps)
    {
        uint32_t testTxRateSteps = (minTxRateSteps + maxTxRateSteps) / 2;
        uint32_t testTxRate = testTxRateSteps * txsPerStep;

        // Calculate transactions per ledger based on target close time
        uint32_t txsPerLedger = targetCloseTimeSteps * testTxRate /
                                mApp.getConfig().APPLY_LOAD_BATCH_SAC_COUNT;
        uint32_t testTps = txsPerLedgerToTPS(
            txsPerLedger * mApp.getConfig().APPLY_LOAD_BATCH_SAC_COUNT);

        CLOG_WARNING(
            Perf,
            "Testing {} TPS with {} batched TXs per ledger ({} transfers).",
            testTps, txsPerLedger,
            txsPerLedger * mApp.getConfig().APPLY_LOAD_BATCH_SAC_COUNT);

        upgradeSettingsForMaxTPS(txsPerLedger);

        double avgCloseTime = benchmarkSacTps(txsPerLedger);

        if (avgCloseTime <= targetCloseTimeMs)
        {
            bestTps = testTps;
            minTxRateSteps = testTxRateSteps + 1;
            CLOG_WARNING(Perf, "Success: {} TPS (avg total tx apply: {:.2f}ms)",
                         testTps, avgCloseTime);
        }
        else
        {
            maxTxRateSteps = testTxRateSteps - 1;
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
