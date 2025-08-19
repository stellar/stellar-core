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
    MIX
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

    // Returns LedgerKey for pre-populated archived state at the given index.
    static LedgerKey getKeyForArchivedEntry(uint64_t index);
    static uint32_t calculateRequiredHotArchiveEntries(Config const& cfg);

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

    LedgerKey mUpgradeCodeKey;
    LedgerKey mUpgradeInstanceKey;

    LedgerKey mLoadCodeKey;
    // Used to generate soroban load transactions
    TxGenerator::ContractInstance mLoadInstance;
    // Used to generate XLM payments
    TxGenerator::ContractInstance mSACInstanceXLM;
    size_t mDataEntryCount = 0;
    size_t mDataEntrySize = 0;

    Application& mApp;
    TxGenerator::TestAccountPtr mRoot;

    uint32_t mNumAccounts;
    uint32_t mTotalHotArchiveEntries;

    medida::Histogram& mTxCountUtilization;
    medida::Histogram& mInstructionUtilization;
    medida::Histogram& mTxSizeUtilization;
    medida::Histogram& mReadByteUtilization;
    medida::Histogram& mWriteByteUtilization;
    medida::Histogram& mReadEntryUtilization;
    medida::Histogram& mWriteEntryUtilization;

    ApplyLoadMode mMode;
    TxGenerator mTxGenerator;
};

}