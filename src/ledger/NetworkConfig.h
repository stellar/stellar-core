#pragma once

// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTxn.h"
#include "main/Config.h"
#include "rust/RustBridge.h"
#include "util/TxResource.h"
#include <cstdint>
#include <deque>

namespace stellar
{

class Application;
class LedgerSnapshot;

// Defines the minimum values allowed for the network configuration
// settings during upgrades. An upgrade that does not follow the minimums
// will be rejected.
struct MinimumSorobanNetworkConfig
{
    static constexpr uint32_t TX_MAX_READ_LEDGER_ENTRIES = 3;
    static constexpr uint32_t TX_MAX_READ_BYTES = 3'200;

    static constexpr uint32_t TX_MAX_WRITE_LEDGER_ENTRIES = 2;
    static constexpr uint32_t TX_MAX_WRITE_BYTES = 3'200;

    static constexpr uint32_t TX_MAX_SIZE_BYTES = 10'000;

    static constexpr uint32_t TX_MAX_INSTRUCTIONS = 2'500'000;
    static constexpr uint32_t MEMORY_LIMIT = 2'000'000;

    static constexpr uint32_t MAX_CONTRACT_DATA_KEY_SIZE_BYTES = 200;
    static constexpr uint32_t MAX_CONTRACT_DATA_ENTRY_SIZE_BYTES = 2'000;
    static constexpr uint32_t MAX_CONTRACT_SIZE = 2'000;

    static constexpr uint32_t MINIMUM_TEMP_ENTRY_LIFETIME = 16;
    static constexpr uint32_t MINIMUM_PERSISTENT_ENTRY_LIFETIME = 10;
    static constexpr uint32_t MAXIMUM_ENTRY_LIFETIME = 1'054'080; // 61 days
    static constexpr int64_t RENT_RATE_DENOMINATOR = INT64_MAX;
    static constexpr uint32_t MAX_ENTRIES_TO_ARCHIVE = 0;
    static constexpr uint32_t BUCKETLIST_SIZE_WINDOW_SAMPLE_SIZE = 1;
    static constexpr uint32_t BUCKETLIST_WINDOW_SAMPLE_PERIOD = 1;
    static constexpr uint32_t EVICTION_SCAN_SIZE = 0;
    static constexpr uint32_t STARTING_EVICTION_LEVEL = 1;

    static constexpr uint32_t TX_MAX_CONTRACT_EVENTS_SIZE_BYTES = 200;

    // SCP timing minimums
    static constexpr uint32_t LEDGER_TARGET_CLOSE_TIME_MILLISECONDS = 4000;
    static constexpr uint32_t NOMINATION_TIMEOUT_INITIAL_MILLISECONDS = 750;
    static constexpr uint32_t NOMINATION_TIMEOUT_INCREMENT_MILLISECONDS = 750;
    static constexpr uint32_t BALLOT_TIMEOUT_INITIAL_MILLISECONDS = 750;
    static constexpr uint32_t BALLOT_TIMEOUT_INCREMENT_MILLISECONDS = 750;
};

// Maximum values for SCP timing configuration
struct MaximumSorobanNetworkConfig
{
    static constexpr uint32_t LEDGER_TARGET_CLOSE_TIME_MILLISECONDS = 5000;
    static constexpr uint32_t NOMINATION_TIMEOUT_INITIAL_MILLISECONDS = 2500;
    static constexpr uint32_t NOMINATION_TIMEOUT_INCREMENT_MILLISECONDS = 2000;
    static constexpr uint32_t BALLOT_TIMEOUT_INITIAL_MILLISECONDS = 2500;
    static constexpr uint32_t BALLOT_TIMEOUT_INCREMENT_MILLISECONDS = 2000;
};

// This is a protocol-level limit for the maximum number of dependent
// transaction clusters per stage (corresponding to
// `ledgerMaxDependentTxClusters` setting).
// This limit is not typical, as the remaining settings tend to have only the
// lower bound. Setting this particular limit to high leads to some
// implementation issues, so we set it to a reasonably low value.
constexpr uint32_t MAX_LEDGER_DEPENDENT_TX_CLUSTERS = 128;

// Defines the initial values of the network configuration
// settings that are applied during the protocol version upgrade.
// These values should never be changed after the protocol upgrade
// happens - any further changes should be performed separately via
// config upgrade mechanism.
struct InitialSorobanNetworkConfig
{
    // Contract size settings
    static constexpr uint32_t MAX_CONTRACT_SIZE =
        MinimumSorobanNetworkConfig::MAX_CONTRACT_SIZE;

    // Contract data settings
    static constexpr uint32_t MAX_CONTRACT_DATA_KEY_SIZE_BYTES =
        MinimumSorobanNetworkConfig::MAX_CONTRACT_DATA_KEY_SIZE_BYTES;
    static constexpr uint32_t MAX_CONTRACT_DATA_ENTRY_SIZE_BYTES =
        MinimumSorobanNetworkConfig::MAX_CONTRACT_DATA_ENTRY_SIZE_BYTES;

    // Compute settings
    static constexpr int64_t TX_MAX_INSTRUCTIONS =
        MinimumSorobanNetworkConfig::TX_MAX_INSTRUCTIONS;
    static constexpr int64_t LEDGER_MAX_INSTRUCTIONS = TX_MAX_INSTRUCTIONS;
    static constexpr int64_t FEE_RATE_PER_INSTRUCTIONS_INCREMENT = 100;
    static constexpr uint32_t MEMORY_LIMIT =
        MinimumSorobanNetworkConfig::MEMORY_LIMIT;

    // Ledger access settings
    static constexpr uint32_t TX_MAX_READ_LEDGER_ENTRIES =
        MinimumSorobanNetworkConfig::TX_MAX_READ_LEDGER_ENTRIES;
    static constexpr uint32_t TX_MAX_READ_BYTES =
        MinimumSorobanNetworkConfig::TX_MAX_READ_BYTES;
    static constexpr uint32_t TX_MAX_WRITE_LEDGER_ENTRIES =
        MinimumSorobanNetworkConfig::TX_MAX_WRITE_LEDGER_ENTRIES;
    static constexpr uint32_t TX_MAX_WRITE_BYTES =
        MinimumSorobanNetworkConfig::TX_MAX_WRITE_BYTES;
    static constexpr uint32_t LEDGER_MAX_READ_LEDGER_ENTRIES =
        TX_MAX_READ_LEDGER_ENTRIES;
    static constexpr uint32_t LEDGER_MAX_READ_BYTES = TX_MAX_READ_BYTES;
    static constexpr uint32_t LEDGER_MAX_WRITE_LEDGER_ENTRIES =
        TX_MAX_WRITE_LEDGER_ENTRIES;
    static constexpr uint32_t LEDGER_MAX_WRITE_BYTES = TX_MAX_WRITE_BYTES;
    static constexpr int64_t FEE_READ_LEDGER_ENTRY = 5'000;
    static constexpr int64_t FEE_WRITE_LEDGER_ENTRY = 20'000;
    static constexpr int64_t FEE_READ_1KB = 1'000;
    static constexpr int64_t BUCKET_LIST_TARGET_SIZE_BYTES =
        30LL * 1024 * 1024 * 1024; // 30 GB
    static constexpr int64_t BUCKET_LIST_FEE_1KB_BUCKET_LIST_LOW = 1'000;
    static constexpr int64_t BUCKET_LIST_FEE_1KB_BUCKET_LIST_HIGH = 10'000;
    // No growth fee initially to make sure fees are accessible
    static constexpr uint32_t STATE_SIZE_RENT_FEE_GROWTH_FACTOR = 1;

    static constexpr uint64_t BUCKET_LIST_SIZE_WINDOW_SAMPLE_SIZE = 30;

    static constexpr uint32_t BUCKET_LIST_WINDOW_SAMPLE_PERIOD = 64;

    // Historical data settings
    static constexpr int64_t FEE_HISTORICAL_1KB = 100;

    // Bandwidth settings
    static constexpr uint32_t TX_MAX_SIZE_BYTES =
        MinimumSorobanNetworkConfig::TX_MAX_SIZE_BYTES;
    static constexpr uint32_t LEDGER_MAX_TRANSACTION_SIZES_BYTES =
        TX_MAX_SIZE_BYTES;
    static constexpr int64_t FEE_TRANSACTION_SIZE_1KB = 2'000;

    // Contract events settings
    static constexpr uint32_t TX_MAX_CONTRACT_EVENTS_SIZE_BYTES =
        MinimumSorobanNetworkConfig::TX_MAX_CONTRACT_EVENTS_SIZE_BYTES;
    static constexpr int64_t FEE_CONTRACT_EVENTS_SIZE_1KB = 200;

    // State archival settings
    static constexpr uint32_t MAXIMUM_ENTRY_LIFETIME =
        MinimumSorobanNetworkConfig::MAXIMUM_ENTRY_LIFETIME;

    // Note that the initial MINIMUM_PERSISTENT_ENTRY_LIFETIME is greater
    // than the minimum to allow for reductions during testing.
    static constexpr uint32_t MINIMUM_PERSISTENT_ENTRY_LIFETIME =
        4'096; // Live until level 6
    static constexpr uint32_t MINIMUM_TEMP_ENTRY_LIFETIME = 16;

    static constexpr uint64_t EVICTION_SCAN_SIZE = 100'000; // 100 kb
    static constexpr uint32_t MAX_ENTRIES_TO_ARCHIVE = 100;
    static constexpr uint32_t STARTING_EVICTION_SCAN_LEVEL = 6;

    // Rent payment of a write fee per ~25 days.
    static constexpr int64_t PERSISTENT_RENT_RATE_DENOMINATOR = 252'480;
    // Rent payment of a write fee per ~250 days.
    static constexpr int64_t TEMP_RENT_RATE_DENOMINATOR = 2'524'800;

    // General execution settings
    static constexpr uint32_t LEDGER_MAX_TX_COUNT = 1;

    // Parallel execution settings
    static constexpr uint32_t LEDGER_MAX_DEPENDENT_TX_CLUSTERS = 1;

    // Ledger cost extension settings
    static constexpr int64_t FEE_LEDGER_WRITE_1KB = 3'500;

    // SCP timing settings
    static constexpr uint32_t LEDGER_TARGET_CLOSE_TIME_MILLISECONDS = 5000;
    static constexpr uint32_t NOMINATION_TIMEOUT_INITIAL_MILLISECONDS = 1000;
    static constexpr uint32_t NOMINATION_TIMEOUT_INCREMENT_MILLISECONDS = 1000;
    static constexpr uint32_t BALLOT_TIMEOUT_INITIAL_MILLISECONDS = 1000;
    static constexpr uint32_t BALLOT_TIMEOUT_INCREMENT_MILLISECONDS = 1000;
};

// The setting values that have to be updated during the protocol 23 upgrade.
// These values are calibrated for the pubnet, but they are also suitable for
// the smaller test networks, so we don't provide a way to override these on
// any network.
struct Protcol23UpgradedConfig
{

    static constexpr int64_t SOROBAN_STATE_TARGET_SIZE_BYTES =
        3'000'000'000LL; // 3 GB
    static constexpr int64_t RENT_FEE_1KB_SOROBAN_STATE_SIZE_LOW = -17'000;
    static constexpr int64_t RENT_FEE_1KB_SOROBAN_STATE_SIZE_HIGH = 10'000;
    static constexpr int64_t PERSISTENT_RENT_RATE_DENOMINATOR = 1'215;
    static constexpr int64_t TEMP_RENT_RATE_DENOMINATOR = 2'430;
};

// Defines the subset of the `InitialSorobanNetworkConfig` to be overridden for
// testing, enabled by `Config::TESTING_SOROBAN_HIGH_LIMIT_OVERRIDE`.
struct TestOverrideSorobanNetworkConfig
{
    static uint32_t const LEDGER_LIMIT_MULTIPLIER = 100;

    // Contract size settings
    static constexpr uint32_t MAX_CONTRACT_SIZE =
        InitialSorobanNetworkConfig::MAX_CONTRACT_SIZE * 64;

    // Contract data settings
    static constexpr uint32_t MAX_CONTRACT_DATA_KEY_SIZE_BYTES =
        InitialSorobanNetworkConfig::MAX_CONTRACT_DATA_KEY_SIZE_BYTES * 1000;
    static constexpr uint32_t MAX_CONTRACT_DATA_ENTRY_SIZE_BYTES =
        InitialSorobanNetworkConfig::MAX_CONTRACT_DATA_ENTRY_SIZE_BYTES * 1000;

    // Compute settings
    static constexpr int64_t TX_MAX_INSTRUCTIONS =
        InitialSorobanNetworkConfig::TX_MAX_INSTRUCTIONS * 100;
    static constexpr int64_t LEDGER_MAX_INSTRUCTIONS =
        TX_MAX_INSTRUCTIONS * LEDGER_LIMIT_MULTIPLIER;
    static constexpr uint32_t MEMORY_LIMIT =
        InitialSorobanNetworkConfig::MEMORY_LIMIT * 100;

    // Ledger access settings
    static constexpr uint32_t TX_MAX_READ_LEDGER_ENTRIES =
        InitialSorobanNetworkConfig::TX_MAX_READ_LEDGER_ENTRIES * 100;
    static constexpr uint32_t TX_MAX_READ_BYTES =
        InitialSorobanNetworkConfig::TX_MAX_READ_BYTES * 1000;
    static constexpr uint32_t TX_MAX_WRITE_LEDGER_ENTRIES =
        InitialSorobanNetworkConfig::TX_MAX_WRITE_LEDGER_ENTRIES * 100;
    static constexpr uint32_t TX_MAX_WRITE_BYTES =
        InitialSorobanNetworkConfig::TX_MAX_WRITE_BYTES * 1000;
    static constexpr uint32_t LEDGER_MAX_READ_LEDGER_ENTRIES =
        TX_MAX_READ_LEDGER_ENTRIES * LEDGER_LIMIT_MULTIPLIER;
    static constexpr uint32_t LEDGER_MAX_READ_BYTES =
        TX_MAX_READ_BYTES * LEDGER_LIMIT_MULTIPLIER;
    static constexpr uint32_t LEDGER_MAX_WRITE_LEDGER_ENTRIES =
        TX_MAX_WRITE_LEDGER_ENTRIES * LEDGER_LIMIT_MULTIPLIER;
    static constexpr uint32_t LEDGER_MAX_WRITE_BYTES =
        TX_MAX_WRITE_BYTES * LEDGER_LIMIT_MULTIPLIER;

    // Bandwidth settings
    static constexpr uint32_t TX_MAX_SIZE_BYTES =
        InitialSorobanNetworkConfig::TX_MAX_SIZE_BYTES * 50;
    static constexpr uint32_t LEDGER_MAX_TRANSACTION_SIZES_BYTES =
        TX_MAX_SIZE_BYTES * LEDGER_LIMIT_MULTIPLIER;

    // Contract events settings
    static constexpr uint32_t TX_MAX_CONTRACT_EVENTS_SIZE_BYTES =
        InitialSorobanNetworkConfig::TX_MAX_CONTRACT_EVENTS_SIZE_BYTES * 20;

    // State archival settings
    static constexpr uint32_t MAXIMUM_ENTRY_LIFETIME = 6307200; // 1 year

    // General execution settings
    static constexpr uint32_t LEDGER_MAX_TX_COUNT =
        InitialSorobanNetworkConfig::LEDGER_MAX_TX_COUNT *
        LEDGER_LIMIT_MULTIPLIER;
};

// Wrapper for the contract-related network configuration.
class SorobanNetworkConfig
{
  public:
    // Static factory function to create a SorobanNetworkConfig from ledger
    static SorobanNetworkConfig loadFromLedger(LedgerSnapshot const& ls);
    static SorobanNetworkConfig
    loadFromLedger(SearchableSnapshotConstPtr snapshot);
    static SorobanNetworkConfig loadFromLedger(AbstractLedgerTxn& ltx);

#ifdef BUILD_TESTS
    // Helper for a few tests that manually create a config.
    static SorobanNetworkConfig emptyConfig();
#endif

    // Creates the initial contract configuration entries for protocol v20.
    // This should happen once during the correspondent protocol version
    // upgrade.
    static void createLedgerEntriesForV20(AbstractLedgerTxn& ltx,
                                          Application& app);

    // Creates the new cost types introduced in v21.
    // This should happen once during the correspondent protocol version
    // upgrade.
    static void createCostTypesForV21(AbstractLedgerTxn& ltx, Application& app);

    // Creates the new cost types introduced in v22.
    // This should happen once during the correspondent protocol version
    // upgrade.
    static void createCostTypesForV22(AbstractLedgerTxn& ltx, Application& app);

    // Creates the new ledger entries introduced in v23 and updates the existing
    // entries.
    // This should happen once during the correspondent protocol version
    // upgrade.
    static void createAndUpdateLedgerEntriesForV23(AbstractLedgerTxn& ltx,
                                                   Application& app);
    // Test-only function that initializes contract network configuration
    // bypassing the normal upgrade process (i.e. when genesis ledger starts not
    // at v1)
    static void
    initializeGenesisLedgerForTesting(uint32_t genesisLedgerProtocol,
                                      AbstractLedgerTxn& ltx, Application& app);

    // If currLedger is a ledger when we should snapshot, add a new snapshot to
    // the sliding window config ledger entry.
    static void maybeSnapshotSorobanStateSize(uint32_t currLedger,
                                              uint64_t inMemoryStateSize,
                                              AbstractLedgerTxn& ltxRoot,
                                              Application& app);

    // Rewrite all the the Soroban live state size snapshots with the newSize.
    // This should be used after recomputing the Soroban state size due to
    // configuration or protocol upgrade.
    static void updateRecomputedSorobanStateSize(uint64_t newSize,
                                                 AbstractLedgerTxn& ltx);

    // If the Soroban state size snapshot window size setting has been changed,
    // update the contents of the snapshot window. This should be called when
    // the window size setting is changed via a config or protocol upgrade.
    // If newSize < currSize, pop entries off window. If newSize > currSize,
    // add the oldest snapshotted size to the window until it has newSize
    // entries.
    static void maybeUpdateSorobanStateSizeWindowSize(AbstractLedgerTxn& ltx);

    static void updateEvictionIterator(AbstractLedgerTxn& ltxRoot,
                                       EvictionIterator const& newIter);

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
    uint32_t ledgerMaxDiskReadEntries() const;
    // Maximum number of bytes that can be read per ledger
    uint32_t ledgerMaxDiskReadBytes() const;
    // Maximum number of ledger entry write operations per ledger
    uint32_t ledgerMaxWriteLedgerEntries() const;
    // Maximum number of bytes that can be written per ledger
    uint32_t ledgerMaxWriteBytes() const;
    // Maximum number of ledger entry read operations per transaction
    uint32_t txMaxDiskReadEntries() const;
    // Maximum number of ledger entries allowed across readOnly and readWrite
    // footprints
    uint32_t txMaxFootprintEntries() const;
    // Maximum number of bytes that can be read per transaction
    uint32_t txMaxDiskReadBytes() const;
    // Maximum number of ledger entry write operations per transaction
    uint32_t txMaxWriteLedgerEntries() const;
    // Maximum number of bytes that can be written per transaction
    uint32_t txMaxWriteBytes() const;
    // Fee per ledger entry read
    int64_t feeDiskReadLedgerEntry() const;
    // Fee per ledger entry write
    int64_t feeWriteLedgerEntry() const;
    // Fee for reading 1KB
    int64_t feeDiskRead1KB() const;
    // Fee for writing 1KB to ledger (no matter if it is a new entry or a
    // modification of an existing entry).
    // This is always 0 prior to protocol 23.
    int64_t feeFlatRateWrite1KB() const;
    // Fee for renting 1KB of ledger space per rent period.
    int64_t feeRent1KB() const;
    // Bucket list target size (in bytes)
    int64_t sorobanStateTargetSizeBytes() const;
    int64_t rentFee1KBSorobanStateSizeLow() const;
    int64_t rentFee1KBSorobanStateSizeHigh() const;
    uint32_t sorobanStateRentFeeGrowthFactor() const;

    // Historical data (pushed to core archives) settings for contracts.
    // Fee for storing 1KB in archives
    int64_t feeHistorical1KB() const;

    // Contract events settings.
    // Maximum size of contract events produced by a transaction.
    uint32_t txMaxContractEventsSizeBytes() const;
    // Fee for generating 1KB of contract events.
    int64_t feeContractEventsSize1KB() const;

    // Bandwidth related data settings for contracts
    // Maximum size in bytes to propagate per ledger
    uint32_t ledgerMaxTransactionSizesBytes() const;
    // Maximum size in bytes for a transaction
    uint32_t txMaxSizeBytes() const;
    // Fee for propagating 1KB of data
    int64_t feeTransactionSize1KB() const;

    // General execution ledger settings
    uint32_t ledgerMaxTxCount() const;

    // Returns the average of all BucketList size snapshots in the sliding
    // window.
    uint64_t getAverageSorobanStateSize() const;

    static bool isValidConfigSettingEntry(ConfigSettingEntry const& cfg,
                                          uint32_t ledgerVersion);
    static bool
    isNonUpgradeableConfigSettingEntry(ConfigSettingEntry const& cfg);

    static bool isNonUpgradeableConfigSettingEntry(ConfigSettingID const& cfg);

    // Cost model parameters of the Soroban host
    ContractCostParams const& cpuCostParams() const;
    ContractCostParams const& memCostParams() const;

    static bool isValidCostParams(ContractCostParams const& params,
                                  uint32_t ledgerVersion);

    CxxFeeConfiguration
    rustBridgeFeeConfiguration(uint32_t ledgerVersion) const;
    CxxRentFeeConfiguration rustBridgeRentFeeConfiguration() const;

    // State archival settings
    StateArchivalSettings const& stateArchivalSettings() const;
    EvictionIterator const& evictionIterator() const;

    // Parallel execution settings
    uint32_t ledgerMaxDependentTxClusters() const;

    Resource maxLedgerResources() const;

    // SCP timing settings
    uint32_t ledgerTargetCloseTimeMilliseconds() const;
    uint32_t nominationTimeoutInitialMilliseconds() const;
    uint32_t nominationTimeoutIncrementMilliseconds() const;
    uint32_t ballotTimeoutInitialMilliseconds() const;
    uint32_t ballotTimeoutIncrementMilliseconds() const;

#ifdef BUILD_TESTS
    // Update the protocol 20 cost types to match the real network
    // configuration.
    // Protocol 20 cost types were imprecise and were re-calibrated shortly
    // after the release. This should be called when initializing the test with
    // protocol 20+.
    // Future recalibrations should be accounted for in a similar way in order
    // to have more accurage modelled CPU and memory costs in tests and
    // especially the benchmarks.
    static void updateRecalibratedCostTypesForV20(AbstractLedgerTxn& ltx);

    bool operator==(SorobanNetworkConfig const& other) const;
#endif

  private:
    SorobanNetworkConfig() = default;

    void loadMaxContractSize(LedgerSnapshot const& ls);
    void loadMaxContractDataKeySize(LedgerSnapshot const& ls);
    void loadMaxContractDataEntrySize(LedgerSnapshot const& ls);
    void loadComputeSettings(LedgerSnapshot const& ls);
    void loadLedgerAccessSettings(LedgerSnapshot const& ls);
    void loadHistoricalSettings(LedgerSnapshot const& ls);
    void loadContractEventsSettings(LedgerSnapshot const& ls);
    void loadBandwidthSettings(LedgerSnapshot const& ls);
    void loadCpuCostParams(LedgerSnapshot const& ls);
    void loadMemCostParams(LedgerSnapshot const& ls);
    void loadStateArchivalSettings(LedgerSnapshot const& ls);
    void loadExecutionLanesSettings(LedgerSnapshot const& ls);
    void loadLiveSorobanStateSizeWindow(LedgerSnapshot const& ls);
    void loadEvictionIterator(LedgerSnapshot const& ls);
    void loadParallelComputeConfig(LedgerSnapshot const& ls);
    void loadLedgerCostExtConfig(LedgerSnapshot const& ls);
    void loadSCPTimingConfig(LedgerSnapshot const& ls);
    void computeRentWriteFee(uint32_t protocolVersion);

#ifdef BUILD_TESTS
  public:
#endif
    // Expose all the fields for testing overrides in order to avoid using
    // special test-only field setters.
    // Access this via
    // `app.getLedgerManager().getMutableSorobanNetworkConfig()`.
    // Important: any manual updates to this will be overwritten in case of
    // **any** network upgrade - tests that perform updates should only update
    // settings via upgrades as well.
    uint32_t mMaxContractSizeBytes{};
    uint32_t mMaxContractDataKeySizeBytes{};
    uint32_t mMaxContractDataEntrySizeBytes{};

    // Compute settings for contracts (instructions and memory).
    int64_t mLedgerMaxInstructions{};
    int64_t mTxMaxInstructions{};
    int64_t mFeeRatePerInstructionsIncrement{};
    uint32_t mTxMemoryLimit{};

    // Ledger access settings for contracts.
    uint32_t mLedgerMaxDiskReadEntries{};
    uint32_t mLedgerMaxDiskReadBytes{};
    uint32_t mLedgerMaxWriteLedgerEntries{};
    uint32_t mLedgerMaxWriteBytes{};
    uint32_t mLedgerMaxTxCount{};
    uint32_t mTxMaxDiskReadEntries{};
    uint32_t mTxMaxDiskReadBytes{};
    uint32_t mTxMaxWriteLedgerEntries{};
    uint32_t mTxMaxWriteBytes{};
    int64_t mFeeDiskReadLedgerEntry{};
    int64_t mFeeWriteLedgerEntry{};
    int64_t mFeeDiskRead1KB{};
    int64_t mFeeRent1KB{};
    int64_t mSorobanStateTargetSizeBytes{};
    int64_t mRentFee1KBSorobanStateSizeLow{};
    int64_t mRentFee1KBSorobanStateSizeHigh{};
    uint32_t mSorobanStateRentFeeGrowthFactor{};

    // Historical data (pushed to core archives) settings for contracts.
    int64_t mFeeHistorical1KB{};

    // Contract events settings.
    uint32_t mTxMaxContractEventsSizeBytes{};
    int64_t mFeeContractEvents1KB{};

    // Bandwidth related data settings for contracts
    uint32_t mLedgerMaxTransactionsSizeBytes{};
    uint32_t mTxMaxSizeBytes{};
    int64_t mFeeTransactionSize1KB{};

    int64_t mAverageSorobanStateSize{};

    // Host cost params
    ContractCostParams mCpuCostParams{};
    ContractCostParams mMemCostParams{};

    // State archival settings
    StateArchivalSettings mStateArchivalSettings{};
    EvictionIterator mEvictionIterator{};

    // Parallel execution settings
    uint32_t mLedgerMaxDependentTxClusters{};

    // Ledger cost extension settings
    uint32_t mTxMaxFootprintEntries{};

    // Flat rate fee for writing 1KB applies only post protocol 23
    int64_t mFeeFlatRateWrite1KB{};

    // SCP timing settings
    uint32_t mLedgerTargetCloseTimeMilliseconds{};
    uint32_t mNominationTimeoutInitialMilliseconds{};
    uint32_t mNominationTimeoutIncrementMilliseconds{};
    uint32_t mBallotTimeoutInitialMilliseconds{};
    uint32_t mBallotTimeoutIncrementMilliseconds{};
};

#ifdef BUILD_TESTS
void updateStateSizeWindowSetting(
    AbstractLedgerTxn& ltxRoot,
    std::function<void(xdr::xvector<uint64>& window)> updateFn);
#endif

}
