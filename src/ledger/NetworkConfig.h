#pragma once

// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTxn.h"
#include "main/Config.h"
#include <cstdint>
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
#include "rust/RustBridge.h"
#endif

namespace stellar
{

// Defines the initial values of the network configuration
// settings that are applied during the protocol version upgrade.
// These values should never be changed after the protocol upgrade
// happens - any further changes should be performed separately via
// config upgrade mechanism.
struct InitialSorobanNetworkConfig
{
    // Contract size settings
    static constexpr uint32_t MAX_CONTRACT_SIZE = 64 * 1024; // 64KB

    // Contract data settings
    static constexpr uint32_t MAX_CONTRACT_DATA_KEY_SIZE_BYTES = 300;
    static constexpr uint32_t MAX_CONTRACT_DATA_ENTRY_SIZE_BYTES =
        64 * 1024; // 64KB

    // Compute settings
    static constexpr int64_t TX_MAX_INSTRUCTIONS = 40'000'000;
    static constexpr int64_t LEDGER_MAX_INSTRUCTIONS = 10 * TX_MAX_INSTRUCTIONS;
    static constexpr int64_t FEE_RATE_PER_INSTRUCTIONS_INCREMENT =
        100;                                                   // 0.2 XLM/max tx
    static constexpr uint32_t MEMORY_LIMIT = 50 * 1024 * 1024; // 50MB

    // Ledger access settings
    static constexpr uint32_t TX_MAX_READ_LEDGER_ENTRIES = 40;
    static constexpr uint32_t TX_MAX_READ_BYTES = 200 * 1024;
    static constexpr uint32_t TX_MAX_WRITE_LEDGER_ENTRIES = 20;
    static constexpr uint32_t TX_MAX_WRITE_BYTES = 100 * 1024;
    static constexpr uint32_t LEDGER_MAX_READ_LEDGER_ENTRIES =
        10 * TX_MAX_READ_LEDGER_ENTRIES;
    static constexpr uint32_t LEDGER_MAX_READ_BYTES = 10 * TX_MAX_READ_BYTES;
    static constexpr uint32_t LEDGER_MAX_WRITE_LEDGER_ENTRIES =
        10 * TX_MAX_WRITE_LEDGER_ENTRIES;
    static constexpr uint32_t LEDGER_MAX_WRITE_BYTES = 10 * TX_MAX_WRITE_BYTES;
    static constexpr int64_t FEE_READ_LEDGER_ENTRY = 5'000;   // 0.02 XLM/max tx
    static constexpr int64_t FEE_WRITE_LEDGER_ENTRY = 20'000; // 0.04 XLM/max tx
    static constexpr int64_t FEE_READ_1KB = 1'000;            // 0.02 XLM/max tx
    static constexpr int64_t FEE_WRITE_1KB = 4'000;           // 0.04 XLM/max tx
    static constexpr int64_t BUCKET_LIST_SIZE_BYTES = 1;
    static constexpr int64_t BUCKET_LIST_FEE_RATE_LOW = 1;
    static constexpr int64_t BUCKET_LIST_FEE_RATE_HIGH = 1;
    static constexpr uint32_t BUCKET_LIST_GROWTH_FACTOR = 1;

    // Historical data settings
    static constexpr int64_t FEE_HISTORICAL_1KB = 100; // 0.001 XLM/max tx

    // Bandwidth settings
    static constexpr uint32_t TX_MAX_SIZE_BYTES = 100 * 1024;
    static constexpr uint32_t LEDGER_MAX_PROPAGATE_SIZE_BYTES =
        10 * TX_MAX_SIZE_BYTES;
    static constexpr int64_t FEE_PROPAGATE_DATA_1KB = 2'000; // 0.02 XLM/max tx

    // Meta data settings
    static constexpr uint32_t TX_MAX_EXTENDED_META_DATA_SIZE_BYTES = 500 * 1024;
    static constexpr int64_t FEE_EXTENDED_META_DATA_1KB = 200;

    // State expiration settings
    // 1 year in ledgers
    static constexpr uint32_t MAXIMUM_ENTRY_LIFETIME = 6'312'000;

    // Live until level 6
    static constexpr uint32_t MINIMUM_PERSISTENT_ENTRY_LIFETIME = 4096;
    static constexpr uint32_t MINIMUM_TEMP_ENTRY_LIFETIME = 16;

    static constexpr uint32_t AUTO_BUMP_NUM_LEDGERS = 10;

    // General execution settings
    static constexpr uint32_t LEDGER_MAX_TX_COUNT = 10;
};

// Wrapper for the contract-related network configuration.
class SorobanNetworkConfig
{
  public:
    // Creates the initial contract configuration entries for protocol v20.
    // This should happen once during the correspondent protocol version
    // upgrade.
    static void createLedgerEntriesForV20(AbstractLedgerTxn& ltx,
                                          Config const& cfg);
    // Test-only function that initializes contract network configuration
    // bypassing the normal upgrade process (i.e. when genesis ledger starts not
    // at v1)
    static void
    initializeGenesisLedgerForTesting(uint32_t genesisLedgerProtocol,
                                      AbstractLedgerTxn& ltx,
                                      Config const& cfg);

    void loadFromLedger(AbstractLedgerTxn& ltx);
    // Maximum allowed size of the contract Wasm that can be uploaded (in
    // bytes).
    uint32_t maxContractSizeBytes() const;
    // Maximum allowed size of a `LedgerKey::CONTRACT_DATA` (in bytes).
    uint32_t maxContractDataKeySizeBytes() const;
    // Maximum allowed size of a `LedgerEntry::CONTRACT_DATA` (in bytes).
    uint32_t maxContractDataEntrySizeBytes() const;

    // Compute settings for contracts (instructions and memory).
    // Maximum instructions per ledger
    int64_t ledgerMaxInstructions() const;
    // Maximum instructions per transaction
    int64_t txMaxInstructions() const;
    // Cost of 10000 instructions
    int64_t feeRatePerInstructionsIncrement() const;
    // Memory limit per transaction.
    uint32_t txMemoryLimit() const;

    // Ledger access settings for contracts.
    // Maximum number of ledger entry read operations per ledger
    uint32_t ledgerMaxReadLedgerEntries() const;
    // Maximum number of bytes that can be read per ledger
    uint32_t ledgerMaxReadBytes() const;
    // Maximum number of ledger entry write operations per ledger
    uint32_t ledgerMaxWriteLedgerEntries() const;
    // Maximum number of bytes that can be written per ledger
    uint32_t ledgerMaxWriteBytes() const;
    // Maximum number of ledger entry read operations per transaction
    uint32_t txMaxReadLedgerEntries() const;
    // Maximum number of bytes that can be read per transaction
    uint32_t txMaxReadBytes() const;
    // Maximum number of ledger entry write operations per transaction
    uint32_t txMaxWriteLedgerEntries() const;
    // Maximum number of bytes that can be written per transaction
    uint32_t txMaxWriteBytes() const;
    // Fee per ledger entry read
    int64_t feeReadLedgerEntry() const;
    // Fee per ledger entry write
    int64_t feeWriteLedgerEntry() const;
    // Fee for reading 1KB
    int64_t feeRead1KB() const;
    // Fee for writing 1KB
    int64_t feeWrite1KB() const;
    // Bucket list fees grow slowly up to that size
    int64_t bucketListSizeBytes() const;
    // Fee rate in stroops when the bucket list is empty
    int64_t bucketListFeeRateLow() const;
    // Fee rate in stroops when the bucket list reached bucketListSizeBytes
    int64_t bucketListFeeRateHigh() const;
    // Rate multiplier for any additional data past the first
    // bucketListSizeBytes
    uint32_t bucketListGrowthFactor() const;

    // Historical data (pushed to core archives) settings for contracts.
    // Fee for storing 1KB in archives
    int64_t feeHistorical1KB() const;

    // Meta data (pushed to downstream systems) settings for contracts.
    // Maximum size of extended meta data produced by a transaction
    uint32_t txMaxExtendedMetaDataSizeBytes() const;
    // Fee for generating 1KB of extended meta data
    int64_t feeExtendedMetaData1KB() const;

    // Bandwidth related data settings for contracts
    // Maximum size in bytes to propagate per ledger
    uint32_t ledgerMaxPropagateSizeBytes() const;
    // Maximum size in bytes for a transaction
    uint32_t txMaxSizeBytes() const;
    // Fee for propagating 1KB of data
    int64_t feePropagateData1KB() const;

    // General execution ledger settings
    uint32_t ledgerMaxTxCount() const;

#ifdef BUILD_TESTS
    uint32_t& maxContractDataKeySizeBytes();
    uint32_t& maxContractDataEntrySizeBytes();
#endif

#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    // Cost model parameters of the Soroban host
    ContractCostParams const& cpuCostParams() const;
    ContractCostParams const& memCostParams() const;

    static bool isValidCostParams(ContractCostParams const& params);

    CxxFeeConfiguration rustBridgeFeeConfiguration() const;

    // State expiration settings
    StateExpirationSettings const& stateExpirationSettings() const;
#endif

  private:
    void loadMaxContractSize(AbstractLedgerTxn& ltx);
    void loadMaxContractDataKeySize(AbstractLedgerTxn& ltx);
    void loadMaxContractDataEntrySize(AbstractLedgerTxn& ltx);
    void loadComputeSettings(AbstractLedgerTxn& ltx);
    void loadLedgerAccessSettings(AbstractLedgerTxn& ltx);
    void loadHistoricalSettings(AbstractLedgerTxn& ltx);
    void loadMetaDataSettings(AbstractLedgerTxn& ltx);
    void loadBandwidthSettings(AbstractLedgerTxn& ltx);
    void loadCpuCostParams(AbstractLedgerTxn& ltx);
    void loadMemCostParams(AbstractLedgerTxn& ltx);
    void loadStateExpirationSettings(AbstractLedgerTxn& ltx);
    void loadExecutionLanesSettings(AbstractLedgerTxn& ltx);

    uint32_t mMaxContractSizeBytes{};
    uint32_t mMaxContractDataKeySizeBytes{};
    uint32_t mMaxContractDataEntrySizeBytes{};

    // Compute settings for contracts (instructions and memory).
    int64_t mLedgerMaxInstructions{};
    int64_t mTxMaxInstructions{};
    int64_t mFeeRatePerInstructionsIncrement{};
    uint32_t mTxMemoryLimit{};

    // Ledger access settings for contracts.
    uint32_t mLedgerMaxReadLedgerEntries{};
    uint32_t mLedgerMaxReadBytes{};
    uint32_t mLedgerMaxWriteLedgerEntries{};
    uint32_t mLedgerMaxWriteBytes{};
    uint32_t mLedgerMaxTxCount{};
    uint32_t mTxMaxReadLedgerEntries{};
    uint32_t mTxMaxReadBytes{};
    uint32_t mTxMaxWriteLedgerEntries{};
    uint32_t mTxMaxWriteBytes{};
    int64_t mFeeReadLedgerEntry{};
    int64_t mFeeWriteLedgerEntry{};
    int64_t mFeeRead1KB{};
    int64_t mFeeWrite1KB{};
    int64_t mBucketListSizeBytes{};
    int64_t mBucketListFeeRateLow{};
    int64_t mBucketListFeeRateHigh{};
    uint32_t mBucketListGrowthFactor{};

    // Historical data (pushed to core archives) settings for contracts.
    int64_t mFeeHistorical1KB{};

    // Meta data (pushed to downstream systems) settings for contracts.
    uint32_t mTxMaxExtendedMetaDataSizeBytes{};
    int64_t mFeeExtendedMetaData1KB{};

    // Bandwidth related data settings for contracts
    uint32_t mLedgerMaxPropagateSizeBytes{};
    uint32_t mTxMaxSizeBytes{};
    int64_t mFeePropagateData1KB{};

#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    // Host cost params
    ContractCostParams mCpuCostParams{};
    ContractCostParams mMemCostParams{};

    // State expiration settings
    StateExpirationSettings mStateExpirationSettings{};

#endif
};

}
