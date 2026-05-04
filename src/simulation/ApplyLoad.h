// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "main/Application.h"
#include "simulation/TxGenerator.h"

namespace stellar
{

class ApplyLoad
{
  public:
    explicit ApplyLoad(Application& app);

    // Execute the benchmark according to the mode specified in config.
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

    uint32_t getTotalHotArchiveEntries() const;

  private:
    uint32_t calculateRequiredHotArchiveEntries(Config const& cfg);

    void setup();
    void setupUpgradeContract();
    void setupLoadContract();
    void setupXLMContract();
    void setupBatchTransferContracts();
    void setupTokenContract();
    void setupSoroswapContracts();
    void setupBucketList();

    // Runs for `execute() in `ApplyLoadMode::LIMIT_BASED` mode.
    // Runs APPLY_LOAD_NUM_LEDGERS iterations of `benchmarkLimitsIteration` and
    // outputs the measured ledger close time metrics, as well as some other
    // support metrics.
    void benchmarkLimits();

    // Runs for `execute() in `ApplyLoadMode::MAX_SAC_TPS` mode.
    // Generates SAC transactions and times just the application phase (fee and
    // sequence number processing, tx execution, and post process, but no disk
    // writes). This will do a binary search from APPLY_LOAD_MAX_SAC_TPS_MIN_TPS
    // to APPLY_LOAD_MAX_SAC_TPS_MAX_TPS, attempting to find the largest
    // transaction set we can execute in under
    // APPLY_LOAD_TARGET_CLOSE_TIME_MS.
    void findMaxSacTps();

    // Runs for `execute() in `ApplyLoadMode::BENCHMARK_MODEL_TX` mode.
    // Benchmarks APPLY_LOAD_NUM_LEDGERS ledgers containing
    // APPLY_LOAD_MAX_SOROBAN_TX_COUNT model transactions each and outputs
    // close-time summary statistics.
    void benchmarkModelTx();

    // Run a single ledger benchmark at the given TPS. Returns the close time
    // in milliseconds for that ledger.
    double benchmarkModelTxTpsSingleLedger(ApplyLoadModelTx modelTx,
                                           uint32_t txsPerLedger);

    // Run a single ledger benchmark for the model transaction mode. Returns
    // the close time in milliseconds for that ledger.
    // Fills up a list of transactions with
    // SOROBAN_TRANSACTION_QUEUE_SIZE_MULTIPLIER * the max ledger resources
    // specified in config, create a TransactionSet out of
    // those transactions, and then close a ledger with that TransactionSet. The
    // generated transactions are generated using the LOADGEN_* config
    // parameters.
    double benchmarkLimitsIteration();

    // Generates APPLY_LOAD_CLASSIC_TXS_PER_LEDGER classic payment TXs
    // using accounts starting at startAccountIdx.
    void generateClassicPayments(std::vector<TransactionFrameBasePtr>& txs,
                                 uint32_t startAccountIdx);

    // Generates the given number of native asset SAC payment TXs with no
    // conflicts.
    void generateSacPayments(std::vector<TransactionFrameBasePtr>& txs,
                             uint32_t count);

    // Generates the given number of custom token transfer TXs between genesis
    // accounts with no conflicts.
    void generateTokenTransfers(std::vector<TransactionFrameBasePtr>& txs,
                                uint32_t count);

    // Generates the given number of Soroswap swap TXs across pairs with no
    // conflicts.
    void generateSoroswapSwaps(std::vector<TransactionFrameBasePtr>& txs,
                               uint32_t count);

    // Calculate instructions per transaction based on batch size
    uint64_t calculateInstructionsPerTx() const;

    // Convert benchmark model SAC transfer count into number of tx envelopes
    // to execute, taking APPLY_LOAD_BATCH_SAC_COUNT into account.
    uint32_t calculateBenchmarkModelTxCount() const;

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

    Application& mApp;
    ApplyLoadMode mMode;
    ApplyLoadModelTx mModelTx;
    ApplyLoadTxProfile mLimitsBasedTxProfile;

    uint32_t mTotalHotArchiveEntries;

    uint32_t mNumAccounts;

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

    // Used to generate custom token transfer transactions
    TxGenerator::ContractInstance mTokenInstance;

    // Soroswap AMM benchmark state (type defined in TxGenerator.h)
    TxGenerator::SoroswapState mSoroswapState;

    // Counter for alternating swap direction per pair
    std::vector<uint32_t> mSoroswapSwapCounters;

    // Counter for generating unique destination addresses for SAC payments
    uint32_t mDestCounter = 0;
};

#ifdef BUILD_TESTS
std::pair<uint32_t, uint32_t> noisyBinarySearch(
    std::function<double(uint32_t)> const& f, double targetA, uint32_t xMin,
    uint32_t xMax, double confidence, uint32_t xTolerance,
    size_t maxSamplesPerPoint,
    std::function<void(uint32_t)> const& prepareIteration = nullptr,
    std::function<void(uint32_t, bool)> const& iterationResult = nullptr);
#endif

}
