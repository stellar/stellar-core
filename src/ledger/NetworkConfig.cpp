// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/NetworkConfig.h"
#include "util/ProtocolVersion.h"

namespace stellar
{
namespace
{
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
void
createConfigSettingEntry(ConfigSettingEntry const& configSetting,
                         AbstractLedgerTxn& ltxRoot)
{
    LedgerEntry e;
    e.data.type(CONFIG_SETTING);
    e.data.configSetting() = configSetting;
    LedgerTxn ltx(ltxRoot);
    ltx.create(e);
    ltx.commit();
}

ConfigSettingEntry
initialMaxContractSizeEntry()
{
    ConfigSettingEntry entry(CONFIG_SETTING_CONTRACT_MAX_SIZE_BYTES);

    entry.contractMaxSizeBytes() = InitialNetworkConfig::MAX_CONTRACT_SIZE;

    return entry;
}

ConfigSettingEntry
initialContractComputeSettingsEntry()
{
    ConfigSettingEntry entry(CONFIG_SETTING_CONTRACT_COMPUTE_V0);
    auto& e = entry.contractCompute();

    e.ledgerMaxInstructions = InitialNetworkConfig::LEDGER_MAX_INSTRUCTIONS;
    e.txMaxInstructions = InitialNetworkConfig::TX_MAX_INSTRUCTIONS;
    e.feeRatePerInstructionsIncrement =
        InitialNetworkConfig::FEE_RATE_PER_INSTRUCTIONS_INCREMENT;
    e.memoryLimit = InitialNetworkConfig::MEMORY_LIMIT;

    return entry;
}

ConfigSettingEntry
initialContractLedgerAccessSettingsEntry()
{
    ConfigSettingEntry entry(CONFIG_SETTING_CONTRACT_LEDGER_COST_V0);
    auto& e = entry.contractLedgerCost();

    e.ledgerMaxReadLedgerEntries =
        InitialNetworkConfig::LEDGER_MAX_READ_LEDGER_ENTRIES;
    e.ledgerMaxReadBytes = InitialNetworkConfig::LEDGER_MAX_READ_BYTES;
    e.ledgerMaxWriteLedgerEntries =
        InitialNetworkConfig::LEDGER_MAX_WRITE_LEDGER_ENTRIES;
    e.ledgerMaxWriteBytes = InitialNetworkConfig::LEDGER_MAX_WRITE_BYTES;
    e.txMaxReadLedgerEntries = InitialNetworkConfig::TX_MAX_READ_LEDGER_ENTRIES;
    e.txMaxReadBytes = InitialNetworkConfig::TX_MAX_READ_BYTES;
    e.txMaxWriteLedgerEntries =
        InitialNetworkConfig::TX_MAX_WRITE_LEDGER_ENTRIES;
    e.txMaxWriteBytes = InitialNetworkConfig::TX_MAX_WRITE_BYTES;
    e.feeReadLedgerEntry = InitialNetworkConfig::FEE_READ_LEDGER_ENTRY;
    e.feeWriteLedgerEntry = InitialNetworkConfig::FEE_WRITE_LEDGER_ENTRY;
    e.feeRead1KB = InitialNetworkConfig::FEE_READ_1KB;
    e.feeWrite1KB = InitialNetworkConfig::FEE_WRITE_1KB;
    e.bucketListSizeBytes = InitialNetworkConfig::BUCKET_LIST_SIZE_BYTES;
    e.bucketListFeeRateLow = InitialNetworkConfig::BUCKET_LIST_FEE_RATE_LOW;
    e.bucketListFeeRateHigh = InitialNetworkConfig::BUCKET_LIST_FEE_RATE_HIGH;
    e.bucketListGrowthFactor = InitialNetworkConfig::BUCKET_LIST_GROWTH_FACTOR;

    return entry;
}

ConfigSettingEntry
initialContractHistoricalDataSettingsEntry()
{
    ConfigSettingEntry entry(CONFIG_SETTING_CONTRACT_HISTORICAL_DATA_V0);
    auto& e = entry.contractHistoricalData();

    e.feeHistorical1KB = InitialNetworkConfig::FEE_HISTORICAL_1KB;

    return entry;
}

ConfigSettingEntry
initialContractMetaDataSettingsEntry()
{
    ConfigSettingEntry entry(CONFIG_SETTING_CONTRACT_META_DATA_V0);
    auto& e = entry.contractMetaData();

    e.txMaxExtendedMetaDataSizeBytes =
        InitialNetworkConfig::TX_MAX_EXTENDED_META_DATA_SIZE_BYTES;
    e.feeExtendedMetaData1KB = InitialNetworkConfig::FEE_EXTENDED_META_DATA_1KB;

    return entry;
}

ConfigSettingEntry
initialContractBandwidthSettingsEntry()
{
    ConfigSettingEntry entry(CONFIG_SETTING_CONTRACT_BANDWIDTH_V0);
    auto& e = entry.contractBandwidth();

    e.ledgerMaxPropagateSizeBytes =
        InitialNetworkConfig::LEDGER_MAX_PROPAGATE_SIZE_BYTES;
    e.txMaxSizeBytes = InitialNetworkConfig::TX_MAX_SIZE_BYTES;
    e.feePropagateData1KB = InitialNetworkConfig::FEE_PROPAGATE_DATA_1KB;

    return entry;
}

ConfigSettingEntry
initialHostLogicVersionEntry()
{
    ConfigSettingEntry entry(CONFIG_SETTING_CONTRACT_HOST_LOGIC_VERSION);

    entry.contractHostLogicVersion() = InitialNetworkConfig::HOST_LOGIC_VERSION;

    return entry;
}

#endif
}

void
ContractNetworkConfig::createLedgerEntriesForV20(AbstractLedgerTxn& ltx)
{
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    createConfigSettingEntry(initialMaxContractSizeEntry(), ltx);
    createConfigSettingEntry(initialContractComputeSettingsEntry(), ltx);
    createConfigSettingEntry(initialContractLedgerAccessSettingsEntry(), ltx);
    createConfigSettingEntry(initialContractHistoricalDataSettingsEntry(), ltx);
    createConfigSettingEntry(initialContractMetaDataSettingsEntry(), ltx);
    createConfigSettingEntry(initialContractBandwidthSettingsEntry(), ltx);
    createConfigSettingEntry(initialHostLogicVersionEntry(), ltx);
#endif
}

void
ContractNetworkConfig::initializeGenesisLedgerForTesting(
    uint32_t genesisLedgerProtocol, AbstractLedgerTxn& ltx)
{
    if (protocolVersionStartsFrom(genesisLedgerProtocol, ProtocolVersion::V_20))
    {
        ContractNetworkConfig::createLedgerEntriesForV20(ltx);
    }
}

void
ContractNetworkConfig::loadFromLedger(AbstractLedgerTxn& ltxRoot)
{
    LedgerTxn ltx(ltxRoot);
    loadMaxContractSize(ltx);
    loadComputeSettings(ltx);
    loadLedgerAccessSettings(ltx);
    loadHistoricalSettings(ltx);
    loadMetaDataSettings(ltx);
    loadBandwidthSettings(ltx);
    loadHostLogicVersion(ltx);
}

uint32_t
ContractNetworkConfig::maxContractSizeBytes() const
{
    return mMaxContractSizeBytes;
}

uint32_t
ContractNetworkConfig::hostLogicVersion() const
{
    return mHostLogicVersion;
}

void
ContractNetworkConfig::loadMaxContractSize(AbstractLedgerTxn& ltx)
{
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_MAX_SIZE_BYTES;
    auto le = ltx.load(key).current();
    mMaxContractSizeBytes = le.data.configSetting().contractMaxSizeBytes();
#endif
}

void
ContractNetworkConfig::loadComputeSettings(AbstractLedgerTxn& ltx)
{
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_COMPUTE_V0;
    auto le = ltx.load(key).current();
    auto const& configSetting = le.data.configSetting().contractCompute();
    mLedgerMaxInstructions = configSetting.ledgerMaxInstructions;
    mTxMaxInstructions = configSetting.txMaxInstructions;
    mFeeRatePerInstructionsIncrement =
        configSetting.feeRatePerInstructionsIncrement;
    mMemoryLimit = configSetting.memoryLimit;
#endif
}

void
ContractNetworkConfig::loadLedgerAccessSettings(AbstractLedgerTxn& ltx)
{
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_LEDGER_COST_V0;
    auto le = ltx.load(key).current();
    auto const& configSetting = le.data.configSetting().contractLedgerCost();
    mLedgerMaxReadLedgerEntries = configSetting.ledgerMaxReadLedgerEntries;
    mLedgerMaxReadBytes = configSetting.ledgerMaxReadBytes;
    mLedgerMaxWriteLedgerEntries = configSetting.ledgerMaxWriteLedgerEntries;
    mLedgerMaxWriteBytes = configSetting.ledgerMaxWriteBytes;
    mTxMaxReadLedgerEntries = configSetting.txMaxReadLedgerEntries;
    mTxMaxReadBytes = configSetting.txMaxReadBytes;
    mTxMaxWriteLedgerEntries = configSetting.txMaxWriteLedgerEntries;
    mTxMaxWriteBytes = configSetting.txMaxWriteBytes;
    mFeeReadLedgerEntry = configSetting.feeReadLedgerEntry;
    mFeeWriteLedgerEntry = configSetting.feeWriteLedgerEntry;
    mFeeRead1KB = configSetting.feeRead1KB;
    mFeeWrite1KB = configSetting.feeWrite1KB;
    mBucketListSizeBytes = configSetting.bucketListSizeBytes;
    mBucketListFeeRateLow = configSetting.bucketListFeeRateLow;
    mBucketListFeeRateHigh = configSetting.bucketListFeeRateHigh;
    mBucketListGrowthFactor = configSetting.bucketListGrowthFactor;
#endif
}

void
ContractNetworkConfig::loadHistoricalSettings(AbstractLedgerTxn& ltx)
{
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_HISTORICAL_DATA_V0;
    auto le = ltx.load(key).current();
    auto const& configSetting =
        le.data.configSetting().contractHistoricalData();
    mFeeHistorical1KB = configSetting.feeHistorical1KB;
#endif
}

void
ContractNetworkConfig::loadMetaDataSettings(AbstractLedgerTxn& ltx)
{
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_HOST_LOGIC_VERSION;
    auto le = ltx.load(key).current();
    mHostLogicVersion = le.data.configSetting().contractHostLogicVersion();
#endif
}

void
ContractNetworkConfig::loadBandwidthSettings(AbstractLedgerTxn& ltx)
{
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_BANDWIDTH_V0;
    auto le = ltx.load(key).current();
    auto const& configSetting = le.data.configSetting().contractBandwidth();
    mLedgerMaxPropagateSizeBytes = configSetting.ledgerMaxPropagateSizeBytes;
    mTxMaxSizeBytes = configSetting.txMaxSizeBytes;
    mFeePropagateData1KB = configSetting.feePropagateData1KB;
#endif
}

void
ContractNetworkConfig::loadHostLogicVersion(AbstractLedgerTxn& ltx)
{
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_HOST_LOGIC_VERSION;
    auto le = ltx.load(key).current();
    mHostLogicVersion = le.data.configSetting().contractHostLogicVersion();

#endif
}

// Compute settings for contracts (instructions and memory).
int64_t
ContractNetworkConfig::ledgerMaxInstructions() const
{
    return mLedgerMaxInstructions;
}

int64_t
ContractNetworkConfig::txMaxInstructions() const
{
    return mTxMaxInstructions;
}

int64_t
ContractNetworkConfig::feeRatePerInstructionsIncrement() const
{
    return mFeeRatePerInstructionsIncrement;
}

uint32_t
ContractNetworkConfig::memoryLimit() const
{
    return mMemoryLimit;
}

// Ledger access settings for contracts.
uint32_t
ContractNetworkConfig::ledgerMaxReadLedgerEntries() const
{
    return mLedgerMaxReadLedgerEntries;
}

uint32_t
ContractNetworkConfig::ledgerMaxReadBytes() const
{
    return mLedgerMaxReadBytes;
}

uint32_t
ContractNetworkConfig::ledgerMaxWriteLedgerEntries() const
{
    return mLedgerMaxWriteLedgerEntries;
}

uint32_t
ContractNetworkConfig::ledgerMaxWriteBytes() const
{
    return mLedgerMaxWriteBytes;
}

uint32_t
ContractNetworkConfig::txMaxReadLedgerEntries() const
{
    return mTxMaxReadLedgerEntries;
}

uint32_t
ContractNetworkConfig::txMaxReadBytes() const
{
    return mTxMaxReadBytes;
}

uint32_t
ContractNetworkConfig::txMaxWriteLedgerEntries() const
{
    return mTxMaxWriteLedgerEntries;
}

uint32_t
ContractNetworkConfig::txMaxWriteBytes() const
{
    return mTxMaxWriteBytes;
}

int64_t
ContractNetworkConfig::feeReadLedgerEntry() const
{
    return mFeeReadLedgerEntry;
}

int64_t
ContractNetworkConfig::feeWriteLedgerEntry() const
{
    return mFeeWriteLedgerEntry;
}

int64_t
ContractNetworkConfig::feeRead1KB() const
{
    return mFeeRead1KB;
}

int64_t
ContractNetworkConfig::feeWrite1KB() const
{
    return mFeeWrite1KB;
}

int64_t
ContractNetworkConfig::bucketListSizeBytes() const
{
    return mBucketListSizeBytes;
}

int64_t
ContractNetworkConfig::bucketListFeeRateLow() const
{
    return mBucketListFeeRateLow;
}

int64_t
ContractNetworkConfig::bucketListFeeRateHigh() const
{
    return mBucketListFeeRateHigh;
}

uint32_t
ContractNetworkConfig::bucketListGrowthFactor() const
{
    return mBucketListGrowthFactor;
}

// Historical data (pushed to core archives) settings for contracts.
int64_t
ContractNetworkConfig::feeHistorical1KB() const
{
    return mFeeHistorical1KB;
}

// Meta data (pushed to downstream systems) settings for contracts.
uint32_t
ContractNetworkConfig::txMaxExtendedMetaDataSizeBytes() const
{
    return mTxMaxExtendedMetaDataSizeBytes;
}

int64_t
ContractNetworkConfig::feeExtendedMetaData1KB() const
{
    return mFeeExtendedMetaData1KB;
}

// Bandwidth related data settings for contracts
uint32_t
ContractNetworkConfig::ledgerMaxPropagateSizeBytes() const
{
    return mLedgerMaxPropagateSizeBytes;
}

uint32_t
ContractNetworkConfig::txMaxSizeBytes() const
{
    return mTxMaxSizeBytes;
}

int64_t
ContractNetworkConfig::feePropagateData1KB() const
{
    return mFeePropagateData1KB;
}

}
