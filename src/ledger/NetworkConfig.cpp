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
initialMaxContractSizeEntry(Config const& cfg)
{
    ConfigSettingEntry entry(CONFIG_SETTING_CONTRACT_MAX_SIZE_BYTES);

    entry.contractMaxSizeBytes() =
        InitialSorobanNetworkConfig::MAX_CONTRACT_SIZE;

    return entry;
}

ConfigSettingEntry
initialMaxContractDataKeySizeEntry(Config const& cfg)
{
    ConfigSettingEntry entry(CONFIG_SETTING_CONTRACT_DATA_KEY_SIZE_BYTES);

    entry.contractDataKeySizeBytes() =
        InitialSorobanNetworkConfig::MAX_CONTRACT_DATA_KEY_SIZE_BYTES;

    return entry;
}

ConfigSettingEntry
initialMaxContractDataEntrySizeEntry(Config const& cfg)
{
    ConfigSettingEntry entry(CONFIG_SETTING_CONTRACT_DATA_ENTRY_SIZE_BYTES);

    entry.contractDataEntrySizeBytes() =
        InitialSorobanNetworkConfig::MAX_CONTRACT_DATA_ENTRY_SIZE_BYTES;

    return entry;
}

ConfigSettingEntry
initialContractComputeSettingsEntry(Config const& cfg)
{
    ConfigSettingEntry entry(CONFIG_SETTING_CONTRACT_COMPUTE_V0);
    auto& e = entry.contractCompute();

    if (cfg.USE_CONFIG_FOR_GENESIS)
    {
        e.ledgerMaxInstructions = cfg.TESTING_LEDGER_MAX_INSTRUCTIONS;
    }
    else
    {
        e.ledgerMaxInstructions =
            InitialSorobanNetworkConfig::LEDGER_MAX_INSTRUCTIONS;
    }
    e.txMaxInstructions = InitialSorobanNetworkConfig::TX_MAX_INSTRUCTIONS;
    e.feeRatePerInstructionsIncrement =
        InitialSorobanNetworkConfig::FEE_RATE_PER_INSTRUCTIONS_INCREMENT;
    e.txMemoryLimit = InitialSorobanNetworkConfig::MEMORY_LIMIT;

    return entry;
}

ConfigSettingEntry
initialContractLedgerAccessSettingsEntry(Config const& cfg)
{
    ConfigSettingEntry entry(CONFIG_SETTING_CONTRACT_LEDGER_COST_V0);
    auto& e = entry.contractLedgerCost();

    if (cfg.USE_CONFIG_FOR_GENESIS)
    {
        e.ledgerMaxReadLedgerEntries =
            cfg.TESTING_LEDGER_MAX_READ_LEDGER_ENTRIES;
    }
    else
    {
        e.ledgerMaxReadLedgerEntries =
            InitialSorobanNetworkConfig::LEDGER_MAX_READ_LEDGER_ENTRIES;
    }
    if (cfg.USE_CONFIG_FOR_GENESIS)
    {
        e.ledgerMaxReadBytes = cfg.TESTING_LEDGER_MAX_READ_BYTES;
    }
    else
    {
        e.ledgerMaxReadBytes =
            InitialSorobanNetworkConfig::LEDGER_MAX_READ_BYTES;
    }
    if (cfg.USE_CONFIG_FOR_GENESIS)
    {
        e.ledgerMaxWriteLedgerEntries =
            cfg.TESTING_LEDGER_MAX_WRITE_LEDGER_ENTRIES;
    }
    else
    {
        e.ledgerMaxWriteLedgerEntries =
            InitialSorobanNetworkConfig::LEDGER_MAX_WRITE_LEDGER_ENTRIES;
    }
    if (cfg.USE_CONFIG_FOR_GENESIS)
    {
        e.ledgerMaxWriteBytes = cfg.TESTING_LEDGER_MAX_WRITE_BYTES;
    }
    else
    {
        e.ledgerMaxWriteBytes =
            InitialSorobanNetworkConfig::LEDGER_MAX_WRITE_BYTES;
    }
    e.txMaxReadLedgerEntries =
        InitialSorobanNetworkConfig::TX_MAX_READ_LEDGER_ENTRIES;
    e.txMaxReadBytes = InitialSorobanNetworkConfig::TX_MAX_READ_BYTES;
    e.txMaxWriteLedgerEntries =
        InitialSorobanNetworkConfig::TX_MAX_WRITE_LEDGER_ENTRIES;
    e.txMaxWriteBytes = InitialSorobanNetworkConfig::TX_MAX_WRITE_BYTES;
    e.feeReadLedgerEntry = InitialSorobanNetworkConfig::FEE_READ_LEDGER_ENTRY;
    e.feeWriteLedgerEntry = InitialSorobanNetworkConfig::FEE_WRITE_LEDGER_ENTRY;
    e.feeRead1KB = InitialSorobanNetworkConfig::FEE_READ_1KB;
    e.feeWrite1KB = InitialSorobanNetworkConfig::FEE_WRITE_1KB;
    e.bucketListSizeBytes = InitialSorobanNetworkConfig::BUCKET_LIST_SIZE_BYTES;
    e.bucketListFeeRateLow =
        InitialSorobanNetworkConfig::BUCKET_LIST_FEE_RATE_LOW;
    e.bucketListFeeRateHigh =
        InitialSorobanNetworkConfig::BUCKET_LIST_FEE_RATE_HIGH;
    e.bucketListGrowthFactor =
        InitialSorobanNetworkConfig::BUCKET_LIST_GROWTH_FACTOR;

    return entry;
}

ConfigSettingEntry
initialContractHistoricalDataSettingsEntry(Config const& cfg)
{
    ConfigSettingEntry entry(CONFIG_SETTING_CONTRACT_HISTORICAL_DATA_V0);
    auto& e = entry.contractHistoricalData();

    e.feeHistorical1KB = InitialSorobanNetworkConfig::FEE_HISTORICAL_1KB;

    return entry;
}

ConfigSettingEntry
initialContractMetaDataSettingsEntry(Config const& cfg)
{
    ConfigSettingEntry entry(CONFIG_SETTING_CONTRACT_META_DATA_V0);
    auto& e = entry.contractMetaData();

    e.txMaxExtendedMetaDataSizeBytes =
        InitialSorobanNetworkConfig::TX_MAX_EXTENDED_META_DATA_SIZE_BYTES;
    e.feeExtendedMetaData1KB =
        InitialSorobanNetworkConfig::FEE_EXTENDED_META_DATA_1KB;

    return entry;
}

ConfigSettingEntry
initialContractBandwidthSettingsEntry(Config const& cfg)
{
    ConfigSettingEntry entry(CONFIG_SETTING_CONTRACT_BANDWIDTH_V0);
    auto& e = entry.contractBandwidth();

    if (cfg.USE_CONFIG_FOR_GENESIS)
    {
        e.ledgerMaxPropagateSizeBytes =
            cfg.TESTING_LEDGER_MAX_PROPAGATE_SIZE_BYTES;
    }
    else
    {
        e.ledgerMaxPropagateSizeBytes =
            InitialSorobanNetworkConfig::LEDGER_MAX_PROPAGATE_SIZE_BYTES;
    }
    e.txMaxSizeBytes = InitialSorobanNetworkConfig::TX_MAX_SIZE_BYTES;
    e.feePropagateData1KB = InitialSorobanNetworkConfig::FEE_PROPAGATE_DATA_1KB;

    return entry;
}

ConfigSettingEntry
initialContractExecutionLanesSettingsEntry(Config const& cfg)
{
    ConfigSettingEntry entry(CONFIG_SETTING_CONTRACT_EXECUTION_LANES);
    auto& e = entry.contractExecutionLanes();

    if (cfg.USE_CONFIG_FOR_GENESIS)
    {
        e.ledgerMaxTxCount = cfg.TESTING_LEDGER_MAX_SOROBAN_TX_COUNT;
    }
    else
    {
        e.ledgerMaxTxCount = InitialSorobanNetworkConfig::LEDGER_MAX_TX_COUNT;
    }

    return entry;
}

ConfigSettingEntry
initialCpuCostParamsEntry(Config const& cfg)
{
    ConfigSettingEntry entry(
        CONFIG_SETTING_CONTRACT_COST_PARAMS_CPU_INSTRUCTIONS);

    auto& params = entry.contractCostParamsCpuInsns();
    auto const& vals = xdr::xdr_traits<ContractCostType>::enum_values();
    params.resize(static_cast<uint32>(vals.size()));
    for (auto val : vals)
    {
        switch (val)
        {
        case WasmInsnExec:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 22, 0};
            break;
        case WasmMemAlloc:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 521, 0};
            break;
        case HostMemAlloc:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 883, 0};
            break;
        case HostMemCpy:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 24, 0};
            break;
        case HostMemCmp:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 42, 1};
            break;
        case InvokeHostFunction:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 759, 0};
            break;
        case VisitObject:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 29, 0};
            break;
        case ValXdrConv:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 177, 0};
            break;
        case ValSer:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 741, 0};
            break;
        case ValDeser:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 846, 0};
            break;
        case ComputeSha256Hash:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 1912, 32};
            break;
        case ComputeEd25519PubKey:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 25766, 0};
            break;
        case MapEntry:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 59, 0};
            break;
        case VecEntry:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 14, 0};
            break;
        case GuardFrame:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 4512, 0};
            break;
        case VerifyEd25519Sig:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 368361, 20};
            break;
        case VmMemRead:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 95, 0};
            break;
        case VmMemWrite:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 97, 0};
            break;
        case VmInstantiation:
            params[val] =
                ContractCostParamEntry{ExtensionPoint{0}, 1'000'000, 0};
            break;
        case InvokeVmFunction:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 6212, 0};
            break;
        case ChargeBudget:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 198, 0};
            break;
        }
    }

    return entry;
}

ConfigSettingEntry
initialStateExpirationSettings()
{
    ConfigSettingEntry entry(CONFIG_SETTING_STATE_EXPIRATION);

    entry.stateExpirationSettings().autoBumpLedgers =
        InitialSorobanNetworkConfig::AUTO_BUMP_NUM_LEDGERS;
    entry.stateExpirationSettings().maxEntryExpiration =
        InitialSorobanNetworkConfig::MAXIMUM_ENTRY_LIFETIME;
    entry.stateExpirationSettings().minPersistentEntryExpiration =
        InitialSorobanNetworkConfig::MINIMUM_PERSISTENT_ENTRY_LIFETIME;
    entry.stateExpirationSettings().minTempEntryExpiration =
        InitialSorobanNetworkConfig::MINIMUM_TEMP_ENTRY_LIFETIME;

    return entry;
}

ConfigSettingEntry
initialMemCostParamsEntry(Config const& cfg)
{
    ConfigSettingEntry entry(CONFIG_SETTING_CONTRACT_COST_PARAMS_MEMORY_BYTES);

    auto& params = entry.contractCostParamsMemBytes();
    auto const& vals = xdr::xdr_traits<ContractCostType>::enum_values();
    params.resize(static_cast<uint32>(vals.size()));
    for (auto val : vals)
    {
        switch (val)
        {
        case WasmInsnExec:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        case WasmMemAlloc:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 66136, 1};
            break;
        case HostMemAlloc:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 8, 1};
            break;
        case HostMemCpy:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        case HostMemCmp:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        case InvokeHostFunction:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        case VisitObject:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        case ValXdrConv:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        case ValSer:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 9, 3};
            break;
        case ValDeser:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 4, 1};
            break;
        case ComputeSha256Hash:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 40, 0};
            break;
        case ComputeEd25519PubKey:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        case MapEntry:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        case VecEntry:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        case GuardFrame:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 267, 0};
            break;
        case VerifyEd25519Sig:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        case VmMemRead:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        case VmMemWrite:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        case VmInstantiation:
            params[val] =
                ContractCostParamEntry{ExtensionPoint{0}, 1'100'000, 0};
            break;
        case InvokeVmFunction:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 267, 0};
            break;
        case ChargeBudget:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        }
    }

    return entry;
}

#endif
}

void
SorobanNetworkConfig::createLedgerEntriesForV20(AbstractLedgerTxn& ltx,
                                                Config const& cfg)
{
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    createConfigSettingEntry(initialMaxContractSizeEntry(cfg), ltx);
    createConfigSettingEntry(initialMaxContractDataKeySizeEntry(cfg), ltx);
    createConfigSettingEntry(initialMaxContractDataEntrySizeEntry(cfg), ltx);
    createConfigSettingEntry(initialContractComputeSettingsEntry(cfg), ltx);
    createConfigSettingEntry(initialContractLedgerAccessSettingsEntry(cfg),
                             ltx);
    createConfigSettingEntry(initialContractHistoricalDataSettingsEntry(cfg),
                             ltx);
    createConfigSettingEntry(initialContractMetaDataSettingsEntry(cfg), ltx);
    createConfigSettingEntry(initialContractBandwidthSettingsEntry(cfg), ltx);
    createConfigSettingEntry(initialContractExecutionLanesSettingsEntry(cfg),
                             ltx);
    createConfigSettingEntry(initialCpuCostParamsEntry(cfg), ltx);
    createConfigSettingEntry(initialMemCostParamsEntry(cfg), ltx);
    createConfigSettingEntry(initialStateExpirationSettings(), ltx);
#endif
}

void
SorobanNetworkConfig::initializeGenesisLedgerForTesting(
    uint32_t genesisLedgerProtocol, AbstractLedgerTxn& ltx, Config const& cfg)
{
    if (protocolVersionStartsFrom(genesisLedgerProtocol, ProtocolVersion::V_20))
    {
        SorobanNetworkConfig::createLedgerEntriesForV20(ltx, cfg);
    }
}

void
SorobanNetworkConfig::loadFromLedger(AbstractLedgerTxn& ltxRoot)
{
    LedgerTxn ltx(ltxRoot, false, TransactionMode::READ_ONLY_WITHOUT_SQL_TXN);
    loadMaxContractSize(ltx);
    loadMaxContractDataKeySize(ltx);
    loadMaxContractDataEntrySize(ltx);
    loadComputeSettings(ltx);
    loadLedgerAccessSettings(ltx);
    loadHistoricalSettings(ltx);
    loadMetaDataSettings(ltx);
    loadBandwidthSettings(ltx);
    loadCpuCostParams(ltx);
    loadMemCostParams(ltx);
    loadStateExpirationSettings(ltx);
    loadExecutionLanesSettings(ltx);
}

void
SorobanNetworkConfig::loadMaxContractSize(AbstractLedgerTxn& ltx)
{
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_MAX_SIZE_BYTES;
    auto le = ltx.loadWithoutRecord(key).current();
    mMaxContractSizeBytes = le.data.configSetting().contractMaxSizeBytes();
#endif
}

void
SorobanNetworkConfig::loadMaxContractDataKeySize(AbstractLedgerTxn& ltx)
{
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_DATA_KEY_SIZE_BYTES;
    auto le = ltx.loadWithoutRecord(key).current();
    mMaxContractDataKeySizeBytes =
        le.data.configSetting().contractDataKeySizeBytes();
#endif
}

void
SorobanNetworkConfig::loadMaxContractDataEntrySize(AbstractLedgerTxn& ltx)
{
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_DATA_ENTRY_SIZE_BYTES;
    auto le = ltx.loadWithoutRecord(key).current();
    mMaxContractDataEntrySizeBytes =
        le.data.configSetting().contractDataEntrySizeBytes();
#endif
}

void
SorobanNetworkConfig::loadComputeSettings(AbstractLedgerTxn& ltx)
{
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_COMPUTE_V0;
    auto le = ltx.loadWithoutRecord(key).current();
    auto const& configSetting = le.data.configSetting().contractCompute();
    mLedgerMaxInstructions = configSetting.ledgerMaxInstructions;
    mTxMaxInstructions = configSetting.txMaxInstructions;
    mFeeRatePerInstructionsIncrement =
        configSetting.feeRatePerInstructionsIncrement;
    mTxMemoryLimit = configSetting.txMemoryLimit;
#endif
}

void
SorobanNetworkConfig::loadLedgerAccessSettings(AbstractLedgerTxn& ltx)
{
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_LEDGER_COST_V0;
    auto le = ltx.loadWithoutRecord(key).current();
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
SorobanNetworkConfig::loadHistoricalSettings(AbstractLedgerTxn& ltx)
{
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_HISTORICAL_DATA_V0;
    auto le = ltx.loadWithoutRecord(key).current();
    auto const& configSetting =
        le.data.configSetting().contractHistoricalData();
    mFeeHistorical1KB = configSetting.feeHistorical1KB;
#endif
}

void
SorobanNetworkConfig::loadMetaDataSettings(AbstractLedgerTxn& ltx)
{
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_META_DATA_V0;
    auto le = ltx.loadWithoutRecord(key).current();
    auto const& configSetting = le.data.configSetting().contractMetaData();
    mFeeExtendedMetaData1KB = configSetting.feeExtendedMetaData1KB;
    mTxMaxExtendedMetaDataSizeBytes =
        configSetting.txMaxExtendedMetaDataSizeBytes;
#endif
}

void
SorobanNetworkConfig::loadBandwidthSettings(AbstractLedgerTxn& ltx)
{
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_BANDWIDTH_V0;
    auto le = ltx.loadWithoutRecord(key).current();
    auto const& configSetting = le.data.configSetting().contractBandwidth();
    mLedgerMaxPropagateSizeBytes = configSetting.ledgerMaxPropagateSizeBytes;
    mTxMaxSizeBytes = configSetting.txMaxSizeBytes;
    mFeePropagateData1KB = configSetting.feePropagateData1KB;
#endif
}

void
SorobanNetworkConfig::loadCpuCostParams(AbstractLedgerTxn& ltx)
{
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_COST_PARAMS_CPU_INSTRUCTIONS;
    auto le = ltx.loadWithoutRecord(key).current();
    mCpuCostParams = le.data.configSetting().contractCostParamsCpuInsns();
#endif
}

void
SorobanNetworkConfig::loadMemCostParams(AbstractLedgerTxn& ltx)
{
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_COST_PARAMS_MEMORY_BYTES;
    auto le = ltx.loadWithoutRecord(key).current();
    mMemCostParams = le.data.configSetting().contractCostParamsMemBytes();
#endif
}

void
SorobanNetworkConfig::loadExecutionLanesSettings(AbstractLedgerTxn& ltx)
{
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_EXECUTION_LANES;
    auto le = ltx.loadWithoutRecord(key).current();
    auto const& configSetting =
        le.data.configSetting().contractExecutionLanes();
    mLedgerMaxTxCount = configSetting.ledgerMaxTxCount;
#endif
}

uint32_t
SorobanNetworkConfig::maxContractSizeBytes() const
{
    return mMaxContractSizeBytes;
}

uint32_t
SorobanNetworkConfig::maxContractDataKeySizeBytes() const
{
    return mMaxContractDataKeySizeBytes;
}

uint32_t
SorobanNetworkConfig::maxContractDataEntrySizeBytes() const
{
    return mMaxContractDataEntrySizeBytes;
}

void
SorobanNetworkConfig::loadStateExpirationSettings(AbstractLedgerTxn& ltx)
{
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_STATE_EXPIRATION;
    auto le = ltx.loadWithoutRecord(key).current();
    mStateExpirationSettings =
        le.data.configSetting().stateExpirationSettings();
#endif
}

// Compute settings for contracts (instructions and memory).
int64_t
SorobanNetworkConfig::ledgerMaxInstructions() const
{
    return mLedgerMaxInstructions;
}

int64_t
SorobanNetworkConfig::txMaxInstructions() const
{
    return mTxMaxInstructions;
}

int64_t
SorobanNetworkConfig::feeRatePerInstructionsIncrement() const
{
    return mFeeRatePerInstructionsIncrement;
}

uint32_t
SorobanNetworkConfig::txMemoryLimit() const
{
    return mTxMemoryLimit;
}

// Ledger access settings for contracts.
uint32_t
SorobanNetworkConfig::ledgerMaxReadLedgerEntries() const
{
    return mLedgerMaxReadLedgerEntries;
}

uint32_t
SorobanNetworkConfig::ledgerMaxReadBytes() const
{
    return mLedgerMaxReadBytes;
}

uint32_t
SorobanNetworkConfig::ledgerMaxWriteLedgerEntries() const
{
    return mLedgerMaxWriteLedgerEntries;
}

uint32_t
SorobanNetworkConfig::ledgerMaxWriteBytes() const
{
    return mLedgerMaxWriteBytes;
}

uint32_t
SorobanNetworkConfig::txMaxReadLedgerEntries() const
{
    return mTxMaxReadLedgerEntries;
}

uint32_t
SorobanNetworkConfig::txMaxReadBytes() const
{
    return mTxMaxReadBytes;
}

uint32_t
SorobanNetworkConfig::txMaxWriteLedgerEntries() const
{
    return mTxMaxWriteLedgerEntries;
}

uint32_t
SorobanNetworkConfig::txMaxWriteBytes() const
{
    return mTxMaxWriteBytes;
}

int64_t
SorobanNetworkConfig::feeReadLedgerEntry() const
{
    return mFeeReadLedgerEntry;
}

int64_t
SorobanNetworkConfig::feeWriteLedgerEntry() const
{
    return mFeeWriteLedgerEntry;
}

int64_t
SorobanNetworkConfig::feeRead1KB() const
{
    return mFeeRead1KB;
}

int64_t
SorobanNetworkConfig::feeWrite1KB() const
{
    return mFeeWrite1KB;
}

int64_t
SorobanNetworkConfig::bucketListSizeBytes() const
{
    return mBucketListSizeBytes;
}

int64_t
SorobanNetworkConfig::bucketListFeeRateLow() const
{
    return mBucketListFeeRateLow;
}

int64_t
SorobanNetworkConfig::bucketListFeeRateHigh() const
{
    return mBucketListFeeRateHigh;
}

uint32_t
SorobanNetworkConfig::bucketListGrowthFactor() const
{
    return mBucketListGrowthFactor;
}

// Historical data (pushed to core archives) settings for contracts.
int64_t
SorobanNetworkConfig::feeHistorical1KB() const
{
    return mFeeHistorical1KB;
}

// Meta data (pushed to downstream systems) settings for contracts.
uint32_t
SorobanNetworkConfig::txMaxExtendedMetaDataSizeBytes() const
{
    return mTxMaxExtendedMetaDataSizeBytes;
}

int64_t
SorobanNetworkConfig::feeExtendedMetaData1KB() const
{
    return mFeeExtendedMetaData1KB;
}

// Bandwidth related data settings for contracts
uint32_t
SorobanNetworkConfig::ledgerMaxPropagateSizeBytes() const
{
    return mLedgerMaxPropagateSizeBytes;
}

uint32_t
SorobanNetworkConfig::txMaxSizeBytes() const
{
    return mTxMaxSizeBytes;
}

int64_t
SorobanNetworkConfig::feePropagateData1KB() const
{
    return mFeePropagateData1KB;
}

// General execution lanes settings for contracts
uint32_t
SorobanNetworkConfig::ledgerMaxTxCount() const
{
    return mLedgerMaxTxCount;
}

#ifdef BUILD_TESTS
uint32_t&
SorobanNetworkConfig::maxContractDataKeySizeBytes()
{
    return mMaxContractDataKeySizeBytes;
}

uint32_t&
SorobanNetworkConfig::maxContractDataEntrySizeBytes()
{
    return mMaxContractDataEntrySizeBytes;
}
#endif

#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION

ContractCostParams const&
SorobanNetworkConfig::cpuCostParams() const
{
    return mCpuCostParams;
}

ContractCostParams const&
SorobanNetworkConfig::memCostParams() const
{
    return mMemCostParams;
}

StateExpirationSettings const&
SorobanNetworkConfig::stateExpirationSettings() const
{
    return mStateExpirationSettings;
}

bool
SorobanNetworkConfig::isValidCostParams(ContractCostParams const& params)
{
    if (params.size() !=
        xdr::xdr_traits<ContractCostType>::enum_values().size())
    {
        return false;
    }

    for (auto const& param : params)
    {
        if (param.constTerm < 0 || param.linearTerm < 0)
        {
            return false;
        }
    }

    return true;
}

CxxFeeConfiguration
SorobanNetworkConfig::rustBridgeFeeConfiguration() const
{
    CxxFeeConfiguration res;
    res.fee_per_instruction_increment = feeRatePerInstructionsIncrement();

    res.fee_per_read_entry = feeReadLedgerEntry();
    res.fee_per_write_entry = feeWriteLedgerEntry();
    res.fee_per_read_1kb = feeRead1KB();
    // This should be dependent on the ledger size, but initially
    // we'll just use the flat rate here.
    res.fee_per_write_1kb = feeWrite1KB();

    res.fee_per_propagate_1kb = feePropagateData1KB();

    res.fee_per_metadata_1kb = feeExtendedMetaData1KB();

    res.fee_per_historical_1kb = feeHistorical1KB();

    return res;
}
#endif
} // namespace stellar
