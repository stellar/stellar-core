#pragma once

// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <cstdint>
#include <ledger/LedgerTxn.h>

namespace stellar
{

// Defines the initial values of the network configuration
// settings that are applied during the protocol version upgrade.
// These values should never be changed after the protocol upgrade
// happens - any further changes should be performed separately via
// config upgrade mechanism.
struct InitialContractNetworkConfig
{
    // Contract size settings
    static constexpr uint32_t MAX_CONTRACT_SIZE = 65536;

    // Compute settings
    static constexpr int64_t LEDGER_MAX_INSTRUCTIONS = 1;
    static constexpr int64_t TX_MAX_INSTRUCTIONS = 1;
    static constexpr int64_t FEE_RATE_PER_INSTRUCTIONS_INCREMENT = 1;
    static constexpr uint32_t MEMORY_LIMIT = 1;

    // Ledger access settings
    static constexpr uint32_t LEDGER_MAX_READ_LEDGER_ENTRIES = 1;
    static constexpr uint32_t LEDGER_MAX_READ_BYTES = 1;
    static constexpr uint32_t LEDGER_MAX_WRITE_LEDGER_ENTRIES = 1;
    static constexpr uint32_t LEDGER_MAX_WRITE_BYTES = 1;
    static constexpr uint32_t TX_MAX_READ_LEDGER_ENTRIES = 1;
    static constexpr uint32_t TX_MAX_READ_BYTES = 1;
    static constexpr uint32_t TX_MAX_WRITE_LEDGER_ENTRIES = 1;
    static constexpr uint32_t TX_MAX_WRITE_BYTES = 1;
    static constexpr int64_t FEE_READ_LEDGER_ENTRY = 1;
    static constexpr int64_t FEE_WRITE_LEDGER_ENTRY = 1;
    static constexpr int64_t FEE_READ_1KB = 1;
    static constexpr int64_t FEE_WRITE_1KB = 1;
    static constexpr int64_t BUCKET_LIST_SIZE_BYTES = 1;
    static constexpr int64_t BUCKET_LIST_FEE_RATE_LOW = 1;
    static constexpr int64_t BUCKET_LIST_FEE_RATE_HIGH = 1;
    static constexpr uint32_t BUCKET_LIST_GROWTH_FACTOR = 1;

    // Historical data settings
    static constexpr int64_t FEE_HISTORICAL_1KB = 1;

    // Bandwidth settings
    static constexpr uint32_t LEDGER_MAX_PROPAGATE_SIZE_BYTES = 1;
    static constexpr uint32_t TX_MAX_SIZE_BYTES = 1;
    static constexpr int64_t FEE_PROPAGATE_DATA_1KB = 1;

    // Meta data settings
    static constexpr uint32_t TX_MAX_EXTENDED_META_DATA_SIZE_BYTES = 1;
    static constexpr int64_t FEE_EXTENDED_META_DATA_1KB = 1;
};

// Wrapper for the contract-related network configuration.
class ContractNetworkConfig
{
  public:
    // Creates the initial contract configuration entries for protocol v20.
    // This should happen once during the correspondent protocol version
    // upgrade.
    static void createLedgerEntriesForV20(AbstractLedgerTxn& ltx);
    // Test-only function that initializes contract network configuration
    // bypassing the normal upgrade process (i.e. when genesis ledger starts not
    // at v1)
    static void
    initializeGenesisLedgerForTesting(uint32_t genesisLedgerProtocol,
                                      AbstractLedgerTxn& ltx);

    void loadFromLedger(AbstractLedgerTxn& ltx);
    // Maximum allowed size of the contract Wasm that can be uploaded (in
    // bytes).
    uint32_t maxContractSizeBytes() const;

    // Compute settings for contracts (instructions and memory).
    // Maximum instructions per ledger
    int64_t ledgerMaxInstructions() const;
    // Maximum instructions per transaction
    int64_t txMaxInstructions() const;
    // Cost of 10000 instructions
    int64_t feeRatePerInstructionsIncrement() const;
    // Memory limit per contract/host function invocation. Unlike
    // instructions, there is no fee for memory and it's not
    // accumulated between operations - the same limit is applied
    // to every operation.
    uint32_t memoryLimit() const;

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

  private:
    void loadMaxContractSize(AbstractLedgerTxn& ltx);
    void loadComputeSettings(AbstractLedgerTxn& ltx);
    void loadLedgerAccessSettings(AbstractLedgerTxn& ltx);
    void loadHistoricalSettings(AbstractLedgerTxn& ltx);
    void loadMetaDataSettings(AbstractLedgerTxn& ltx);
    void loadBandwidthSettings(AbstractLedgerTxn& ltx);

    uint32_t mMaxContractSizeBytes{};

    // Compute settings for contracts (instructions and memory).
    int64_t mLedgerMaxInstructions{};
    int64_t mTxMaxInstructions{};
    int64_t mFeeRatePerInstructionsIncrement{};
    uint32_t mMemoryLimit{};

    // Ledger access settings for contracts.
    uint32_t mLedgerMaxReadLedgerEntries{};
    uint32_t mLedgerMaxReadBytes{};
    uint32_t mLedgerMaxWriteLedgerEntries{};
    uint32_t mLedgerMaxWriteBytes{};
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
};

}
