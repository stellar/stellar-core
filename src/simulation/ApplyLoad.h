// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "main/Application.h"
#include "simulation/TxGenerator.h"
#include "test/TestAccount.h"

#include "medida/meter.h"

namespace stellar
{

enum class ApplyLoadMode
{
    // Generate load within the configured ledger limits.
    LIMIT_BASED,
    // Generate load that finds max ledger limits for the 'model' transaction.
    FIND_LIMITS_FOR_MODEL_TX,
    // Generate load that only finds max TPS for the cheap operations (SAC
    // transfers), ignoring ledger limits.
    MAX_SAC_TPS
};

class ApplyLoad
{
  public:
    ApplyLoad(Application& app, ApplyLoadMode mode);

    // Execute the benchmark according to the mode specified in the constructor.
    void execute();

    // Returns the % of transactions that succeeded during apply time. The range
    // of values is [0,1.0].
    double successRate();

    // Closes a ledger with the given transactions and optional upgrades.
    // `recordSorobanUtilization` indicates whether to record utilization of
    // Soroban resources in transaction set, this should only be necessary for
    // the benchmark runs.
    void closeLedger(std::vector<TransactionFrameBasePtr> const& txs,
                     xdr::xvector<UpgradeType, 6> const& upgrades = {},
                     bool recordSorobanUtilization = false);

    // These metrics track what percentage of available resources were used when
    // creating the list of transactions in benchmark().
    // Histogram uses integers, so the values are scaled up by 100,000
    // Ex. We store 18000 for .18 (or 18%)
    medida::Histogram const& getTxCountUtilization();
    medida::Histogram const& getInstructionUtilization();
    medida::Histogram const& getTxSizeUtilization();
    medida::Histogram const& getDiskReadByteUtilization();
    medida::Histogram const& getDiskWriteByteUtilization();
    medida::Histogram const& getDiskReadEntryUtilization();
    medida::Histogram const& getWriteEntryUtilization();

    // Returns LedgerKey for pre-populated archived state at the given index.
    static LedgerKey getKeyForArchivedEntry(uint64_t index);
    static uint32_t calculateRequiredHotArchiveEntries(ApplyLoadMode mode,
                                                       Config const& cfg);

    // The target time to close a ledger when running in MAX_SAC_TPS mode must
    // be a multiple of TARGET_CLOSE_TIME_STEP_MS.
    static uint32_t constexpr TARGET_CLOSE_TIME_STEP_MS = 50;

  private:
    void setup();

    void setupAccounts();
    void setupUpgradeContract();
    void setupLoadContract();
    void setupXLMContract();
    void setupBatchTransferContracts();
    void setupBucketList();

    // Runs for `execute() in `ApplyLoadMode::LIMIT_BASED` mode.
    // Runs APPLY_LOAD_NUM_LEDGERS iterations of `benchmarkLimitsIteration` and
    // outputs the measured ledger close time metrics, as well as some other
    // support metrics.
    void benchmarkLimits();

    // Runs for `execute() in `ApplyLoadMode::FIND_LIMITS_FOR_MODEL_TX` mode.
    // Generates transactions according to the 'model' transaction parameters
    // (specified via the transaction generation config), and does a binary
    // search for the maximum number of such transactions that can fit into
    // ledger while not exceeding APPLY_LOAD_TARGET_CLOSE_TIME_MS ledger close
    // time.
    // After finding the maximum number of model transactions, outputs the
    // respective ledger limits.
    // This also performs some rounding on the ledger limits to make the binary
    // search faster, and also to produce more readable limits.
    void findMaxLimitsForModelTransaction();

    // Runs for `execute() in `ApplyLoadMode::MAX_SAC_TPS` mode.
    // Generates SAC transactions and times just the application phase (fee and
    // sequence number processing, tx execution, and post process, but no disk
    // writes). This will do a binary search from APPLY_LOAD_MAX_SAC_TPS_MIN_TPS
    // to APPLY_LOAD_MAX_SAC_TPS_MAX_TPS, attempting to find the largest
    // transaction set we can execute in under
    // APPLY_LOAD_TARGET_CLOSE_TIME_MS.
    void findMaxSacTps();

    // Run iterations at the given TPS. Reports average time over all runs, in
    // milliseconds.
    double benchmarkSacTps(uint32_t targetTps);

    // Fills up a list of transactions with
    // SOROBAN_TRANSACTION_QUEUE_SIZE_MULTIPLIER * the max ledger resources
    // specified in the ApplyLoad constructor, create a TransactionSet out of
    // those transactions, and then close a ledger with that TransactionSet. The
    // generated transactions are generated using the LOADGEN_* config
    // parameters.
    void benchmarkLimitsIteration();

    // Generates the given number of native asset SAC payment TXs with no
    // conflicts.
    void generateSacPayments(std::vector<TransactionFrameBasePtr>& txs,
                             uint32_t count);

    // Calculate instructions per transaction based on batch size
    uint64_t calculateInstructionsPerTx() const;

    // Iterate over all available accounts to make sure they are loaded into the
    // BucketListDB cache. Note that this should be run every time an account
    // entry is modified.
    void warmAccountCache();

    // Upgrades using mUpgradeConfig
    void upgradeSettings();

    // Upgrades to very high limits for max TPS apply load test
    void upgradeSettingsForMaxTPS(uint32_t txsToGenerate);

    // Helper method to apply a config upgrade
    void applyConfigUpgrade(SorobanUpgradeConfig const& upgradeConfig);

    // Updates the configuration settings such a way to accommodate around
    // `txsPerLedger` 'model' transactions per ledger for the
    // `FIND_LIMITS_FOR_MODEL_TX` mode.
    // Returns the network configuration to use for upgrade and the actual
    // number of transactions that can fit withing the limits (it may be
    // slightly lower than `txsPerLedger` due to rounding).
    std::pair<SorobanUpgradeConfig, uint64_t>
    updateSettingsForTxCount(uint64_t txsPerLedger);

    Application& mApp;
    ApplyLoadMode mMode;
    TxGenerator::TestAccountPtr mRoot;

    uint32_t mNumAccounts;
    uint32_t mTotalHotArchiveEntries;

    medida::Histogram& mTxCountUtilization;
    medida::Histogram& mInstructionUtilization;
    medida::Histogram& mTxSizeUtilization;
    medida::Histogram& mDiskReadByteUtilization;
    medida::Histogram& mWriteByteUtilization;
    medida::Histogram& mDiskReadEntryUtilization;
    medida::Histogram& mWriteEntryUtilization;

    TxGenerator mTxGenerator;

    LedgerKey mUpgradeCodeKey;
    LedgerKey mUpgradeInstanceKey;

    LedgerKey mLoadCodeKey;
    // Used to generate soroban load transactions
    TxGenerator::ContractInstance mLoadInstance;
    // Used to generate XLM payments
    TxGenerator::ContractInstance mSACInstanceXLM;
    // Used for batch transfers, one instance for each cluster
    std::vector<TxGenerator::ContractInstance> mBatchTransferInstances;
    size_t mDataEntryCount = 0;
    size_t mDataEntrySize = 0;

    // Counter for generating unique destination addresses for SAC payments
    uint32_t mDestCounter = 0;
};

}
