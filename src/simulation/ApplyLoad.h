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
    SOROBAN,
    CLASSIC,
    MIX,
    MAX_SAC_TPS
};

class ApplyLoad
{
  public:
    ApplyLoad(Application& app, ApplyLoadMode mode = ApplyLoadMode::SOROBAN);

    // Fills up a list of transactions with
    // SOROBAN_TRANSACTION_QUEUE_SIZE_MULTIPLIER * the max ledger resources
    // specified in the ApplyLoad constructor, create a TransactionSet out of
    // those transactions, and then close a ledger with that TransactionSet. The
    // generated transactions are generated using the LOADGEN_* config
    // parameters.
    void benchmark();

    // Generates SAC transactions and times just the application phase (fee and
    // sequence number processing, tx execution, and post process, but no disk
    // writes). This will do a binary search from APPLY_LOAD_MAX_SAC_TPS_MIN_TPS
    // to APPLY_LOAD_MAX_SAC_TPS_MAX_TPS, attempting to find the largest
    // transaction set we can execute in under
    // APPLY_LOAD_MAX_SAC_TPS_TARGET_CLOSE_TIME_MS.
    void findMaxSacTps();

    // Returns the % of transactions that succeeded during apply time. The range
    // of values is [0,1.0].
    double successRate();

    // These metrics track what percentage of available resources were used when
    // creating the list of transactions in benchmark().
    // Histogram uses integers, so the values are scaled up by 100,000
    // Ex. We store 18000 for .18 (or 18%)
    medida::Histogram const& getTxCountUtilization();
    medida::Histogram const& getInstructionUtilization();
    medida::Histogram const& getTxSizeUtilization();
    medida::Histogram const& getReadByteUtilization();
    medida::Histogram const& getWriteByteUtilization();
    medida::Histogram const& getReadEntryUtilization();
    medida::Histogram const& getWriteEntryUtilization();

  private:
    void closeLedger(std::vector<TransactionFrameBasePtr> const& txs,
                     xdr::xvector<UpgradeType, 6> const& upgrades = {});

    void setup();

    void setupAccounts();
    void setupUpgradeContract();
    void setupLoadContract();
    void setupXLMContract();
    void setupBucketList();

    // Upgrades using mUpgradeConfig
    void upgradeSettings();

    // Upgrades to very high limits for max TPS apply load test
    void upgradeSettingsForMaxTPS(uint32_t txsToGenerate);

    // Helper method to apply a config upgrade
    void applyConfigUpgrade(SorobanUpgradeConfig const& upgradeConfig);

    // Run iterations at the given TPS. Reports average time over all runs, in
    // milliseconds.
    double benchmarkSacTps(uint32_t targetTps);

    // Generates the given number of native asset SAC payment TXs with no
    // conflicts.
    void generateSacPayments(std::vector<TransactionFrameBasePtr>& txs,
                             uint32_t count);

    // Setup batch transfer contracts for parallel SAC payment processing
    void setupBatchTransferContracts();

    // Iterate over all available accounts to make sure they are loaded into the
    // BucketListDB cache. Note that this should be run everytime an account
    // entry is modified.
    void warmAccountCache();

    LedgerKey mUpgradeCodeKey;
    LedgerKey mUpgradeInstanceKey;

    LedgerKey mLoadCodeKey;
    // Used to generate soroban load transactions
    TxGenerator::ContractInstance mLoadInstance;
    // Used to generate XLM payments
    TxGenerator::ContractInstance mSACInstanceXLM;
    // Batch transfer contract instances (one per cluster for parallelism)
    std::vector<TxGenerator::ContractInstance> mBatchTransferInstances;
    size_t mDataEntryCount = 0;
    size_t mDataEntrySize = 0;

    TxGenerator mTxGenerator;
    Application& mApp;
    TxGenerator::TestAccountPtr mRoot;

    uint32_t mNumAccounts;

    medida::Histogram& mTxCountUtilization;
    medida::Histogram& mInstructionUtilization;
    medida::Histogram& mTxSizeUtilization;
    medida::Histogram& mReadByteUtilization;
    medida::Histogram& mWriteByteUtilization;
    medida::Histogram& mReadEntryUtilization;
    medida::Histogram& mWriteEntryUtilization;

    ApplyLoadMode mMode;
    uint32_t mAccountRotationIndex = 0;
};

}