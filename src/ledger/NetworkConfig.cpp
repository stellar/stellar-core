// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/NetworkConfig.h"
#include "bucket/BucketList.h"
#include "bucket/BucketManager.h"
#include "main/Application.h"
#include "util/ProtocolVersion.h"

namespace stellar
{
namespace
{
void
createConfigSettingEntry(ConfigSettingEntry const& configSetting,
                         AbstractLedgerTxn& ltxRoot)
{
    ZoneScoped;

    if (!SorobanNetworkConfig::isValidConfigSettingEntry(configSetting))
    {
        throw std::runtime_error("Invalid configSettingEntry");
    }

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
    if (cfg.TESTING_SOROBAN_HIGH_LIMIT_OVERRIDE)
    {
        entry.contractMaxSizeBytes() =
            TestOverrideSorobanNetworkConfig::MAX_CONTRACT_SIZE;
    }

    return entry;
}

ConfigSettingEntry
initialMaxContractDataKeySizeEntry(Config const& cfg)
{
    ConfigSettingEntry entry(CONFIG_SETTING_CONTRACT_DATA_KEY_SIZE_BYTES);

    entry.contractDataKeySizeBytes() =
        InitialSorobanNetworkConfig::MAX_CONTRACT_DATA_KEY_SIZE_BYTES;
    if (cfg.TESTING_SOROBAN_HIGH_LIMIT_OVERRIDE)
    {
        entry.contractDataKeySizeBytes() =
            TestOverrideSorobanNetworkConfig::MAX_CONTRACT_DATA_KEY_SIZE_BYTES;
    }

    return entry;
}

ConfigSettingEntry
initialMaxContractDataEntrySizeEntry(Config const& cfg)
{
    ConfigSettingEntry entry(CONFIG_SETTING_CONTRACT_DATA_ENTRY_SIZE_BYTES);

    entry.contractDataEntrySizeBytes() =
        InitialSorobanNetworkConfig::MAX_CONTRACT_DATA_ENTRY_SIZE_BYTES;
    if (cfg.TESTING_SOROBAN_HIGH_LIMIT_OVERRIDE)
    {
        entry.contractDataEntrySizeBytes() = TestOverrideSorobanNetworkConfig::
            MAX_CONTRACT_DATA_ENTRY_SIZE_BYTES;
    }

    return entry;
}

ConfigSettingEntry
initialContractComputeSettingsEntry(Config const& cfg)
{
    ConfigSettingEntry entry(CONFIG_SETTING_CONTRACT_COMPUTE_V0);
    auto& e = entry.contractCompute();

    e.ledgerMaxInstructions =
        InitialSorobanNetworkConfig::LEDGER_MAX_INSTRUCTIONS;

    e.txMaxInstructions = InitialSorobanNetworkConfig::TX_MAX_INSTRUCTIONS;
    e.feeRatePerInstructionsIncrement =
        InitialSorobanNetworkConfig::FEE_RATE_PER_INSTRUCTIONS_INCREMENT;
    e.txMemoryLimit = InitialSorobanNetworkConfig::MEMORY_LIMIT;

    if (cfg.TESTING_SOROBAN_HIGH_LIMIT_OVERRIDE)
    {
        e.ledgerMaxInstructions =
            TestOverrideSorobanNetworkConfig::LEDGER_MAX_INSTRUCTIONS;
        e.txMaxInstructions =
            TestOverrideSorobanNetworkConfig::TX_MAX_INSTRUCTIONS;
        e.txMemoryLimit = TestOverrideSorobanNetworkConfig::MEMORY_LIMIT;
    }

    return entry;
}

ConfigSettingEntry
initialContractLedgerAccessSettingsEntry(Config const& cfg)
{
    ConfigSettingEntry entry(CONFIG_SETTING_CONTRACT_LEDGER_COST_V0);
    auto& e = entry.contractLedgerCost();

    e.ledgerMaxReadLedgerEntries =
        InitialSorobanNetworkConfig::LEDGER_MAX_READ_LEDGER_ENTRIES;

    e.ledgerMaxReadBytes = InitialSorobanNetworkConfig::LEDGER_MAX_READ_BYTES;

    e.ledgerMaxWriteLedgerEntries =
        InitialSorobanNetworkConfig::LEDGER_MAX_WRITE_LEDGER_ENTRIES;

    e.ledgerMaxWriteBytes = InitialSorobanNetworkConfig::LEDGER_MAX_WRITE_BYTES;

    e.txMaxReadLedgerEntries =
        InitialSorobanNetworkConfig::TX_MAX_READ_LEDGER_ENTRIES;
    e.txMaxReadBytes = InitialSorobanNetworkConfig::TX_MAX_READ_BYTES;
    e.txMaxWriteLedgerEntries =
        InitialSorobanNetworkConfig::TX_MAX_WRITE_LEDGER_ENTRIES;
    e.txMaxWriteBytes = InitialSorobanNetworkConfig::TX_MAX_WRITE_BYTES;
    e.feeReadLedgerEntry = InitialSorobanNetworkConfig::FEE_READ_LEDGER_ENTRY;
    e.feeWriteLedgerEntry = InitialSorobanNetworkConfig::FEE_WRITE_LEDGER_ENTRY;
    e.feeRead1KB = InitialSorobanNetworkConfig::FEE_READ_1KB;
    e.bucketListTargetSizeBytes =
        InitialSorobanNetworkConfig::BUCKET_LIST_TARGET_SIZE_BYTES;
    e.writeFee1KBBucketListLow =
        InitialSorobanNetworkConfig::BUCKET_LIST_FEE_1KB_BUCKET_LIST_LOW;
    e.writeFee1KBBucketListHigh =
        InitialSorobanNetworkConfig::BUCKET_LIST_FEE_1KB_BUCKET_LIST_HIGH;
    e.bucketListWriteFeeGrowthFactor =
        InitialSorobanNetworkConfig::BUCKET_LIST_WRITE_FEE_GROWTH_FACTOR;

    if (cfg.TESTING_SOROBAN_HIGH_LIMIT_OVERRIDE)
    {
        e.ledgerMaxReadLedgerEntries =
            TestOverrideSorobanNetworkConfig::LEDGER_MAX_READ_LEDGER_ENTRIES;
        e.ledgerMaxReadBytes =
            TestOverrideSorobanNetworkConfig::LEDGER_MAX_READ_BYTES;
        e.ledgerMaxWriteLedgerEntries =
            TestOverrideSorobanNetworkConfig::LEDGER_MAX_WRITE_LEDGER_ENTRIES;
        e.ledgerMaxWriteBytes =
            TestOverrideSorobanNetworkConfig::LEDGER_MAX_WRITE_BYTES;
        e.txMaxReadLedgerEntries =
            TestOverrideSorobanNetworkConfig::TX_MAX_READ_LEDGER_ENTRIES;
        e.txMaxReadBytes = TestOverrideSorobanNetworkConfig::TX_MAX_READ_BYTES;
        e.txMaxWriteLedgerEntries =
            TestOverrideSorobanNetworkConfig::TX_MAX_WRITE_LEDGER_ENTRIES;
        e.txMaxWriteBytes =
            TestOverrideSorobanNetworkConfig::TX_MAX_WRITE_BYTES;
    }

    return entry;
}

ConfigSettingEntry
initialContractHistoricalDataSettingsEntry()
{
    ConfigSettingEntry entry(CONFIG_SETTING_CONTRACT_HISTORICAL_DATA_V0);
    auto& e = entry.contractHistoricalData();

    e.feeHistorical1KB = InitialSorobanNetworkConfig::FEE_HISTORICAL_1KB;

    return entry;
}

ConfigSettingEntry
initialContractEventsSettingsEntry(Config const& cfg)
{
    ConfigSettingEntry entry(CONFIG_SETTING_CONTRACT_EVENTS_V0);
    auto& e = entry.contractEvents();

    e.txMaxContractEventsSizeBytes =
        InitialSorobanNetworkConfig::TX_MAX_CONTRACT_EVENTS_SIZE_BYTES;
    e.feeContractEvents1KB =
        InitialSorobanNetworkConfig::FEE_CONTRACT_EVENTS_SIZE_1KB;

    if (cfg.TESTING_SOROBAN_HIGH_LIMIT_OVERRIDE)
    {
        e.txMaxContractEventsSizeBytes =
            TestOverrideSorobanNetworkConfig::TX_MAX_CONTRACT_EVENTS_SIZE_BYTES;
    }
    return entry;
}

ConfigSettingEntry
initialContractBandwidthSettingsEntry(Config const& cfg)
{
    ConfigSettingEntry entry(CONFIG_SETTING_CONTRACT_BANDWIDTH_V0);
    auto& e = entry.contractBandwidth();

    e.ledgerMaxTxsSizeBytes =
        InitialSorobanNetworkConfig::LEDGER_MAX_TRANSACTION_SIZES_BYTES;
    e.txMaxSizeBytes = InitialSorobanNetworkConfig::TX_MAX_SIZE_BYTES;

    e.feeTxSize1KB = InitialSorobanNetworkConfig::FEE_TRANSACTION_SIZE_1KB;

    if (cfg.TESTING_SOROBAN_HIGH_LIMIT_OVERRIDE)
    {
        e.ledgerMaxTxsSizeBytes = TestOverrideSorobanNetworkConfig::
            LEDGER_MAX_TRANSACTION_SIZES_BYTES;
        e.txMaxSizeBytes = TestOverrideSorobanNetworkConfig::TX_MAX_SIZE_BYTES;
    }

    return entry;
}

ConfigSettingEntry
initialContractExecutionLanesSettingsEntry(Config const& cfg)
{
    ConfigSettingEntry entry(CONFIG_SETTING_CONTRACT_EXECUTION_LANES);
    auto& e = entry.contractExecutionLanes();
    e.ledgerMaxTxCount = InitialSorobanNetworkConfig::LEDGER_MAX_TX_COUNT;

    if (cfg.TESTING_SOROBAN_HIGH_LIMIT_OVERRIDE)
    {
        e.ledgerMaxTxCount =
            TestOverrideSorobanNetworkConfig::LEDGER_MAX_TX_COUNT;
    }

    return entry;
}

ConfigSettingEntry
initialCpuCostParamsEntry()
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
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 4, 0};
            break;
        case MemAlloc:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 434, 16};
            break;
        case MemCpy:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 42, 16};
            break;
        case MemCmp:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 44, 16};
            break;
        case DispatchHostFunction:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 310, 0};
            break;
        case VisitObject:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 61, 0};
            break;
        case ValSer:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 230, 29};
            break;
        case ValDeser:
            params[val] =
                ContractCostParamEntry{ExtensionPoint{0}, 59052, 4001};
            break;
        case ComputeSha256Hash:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 3738, 7012};
            break;
        case ComputeEd25519PubKey:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 40253, 0};
            break;
        case VerifyEd25519Sig:
            params[val] =
                ContractCostParamEntry{ExtensionPoint{0}, 377524, 4068};
            break;
        case VmInstantiation:
            params[val] =
                ContractCostParamEntry{ExtensionPoint{0}, 451626, 45405};
            break;
        case VmCachedInstantiation:
            params[val] =
                ContractCostParamEntry{ExtensionPoint{0}, 451626, 45405};
            break;
        case InvokeVmFunction:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 1948, 0};
            break;
        case ComputeKeccak256Hash:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 3766, 5969};
            break;
        case ComputeEcdsaSecp256k1Sig:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 710, 0};
            break;
        case RecoverEcdsaSecp256k1Key:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 2315295, 0};
            break;
        case Int256AddSub:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 4404, 0};
            break;
        case Int256Mul:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 4947, 0};
            break;
        case Int256Div:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 4911, 0};
            break;
        case Int256Pow:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 4286, 0};
            break;
        case Int256Shift:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 913, 0};
            break;
        case ChaCha20DrawBytes:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 1058, 501};
            break;
        }
    }

    return entry;
}

ConfigSettingEntry
initialStateArchivalSettings(Config const& cfg)
{
    ConfigSettingEntry entry(CONFIG_SETTING_STATE_ARCHIVAL);

    entry.stateArchivalSettings().maxEntryTTL =
        InitialSorobanNetworkConfig::MAXIMUM_ENTRY_LIFETIME;

    entry.stateArchivalSettings().minPersistentTTL =
        InitialSorobanNetworkConfig::MINIMUM_PERSISTENT_ENTRY_LIFETIME;
    if (cfg.TESTING_MINIMUM_PERSISTENT_ENTRY_LIFETIME != 0)
    {
        entry.stateArchivalSettings().minPersistentTTL =
            cfg.TESTING_MINIMUM_PERSISTENT_ENTRY_LIFETIME;
    }

    entry.stateArchivalSettings().minTemporaryTTL =
        InitialSorobanNetworkConfig::MINIMUM_TEMP_ENTRY_LIFETIME;
    entry.stateArchivalSettings().bucketListSizeWindowSampleSize =
        InitialSorobanNetworkConfig::BUCKET_LIST_SIZE_WINDOW_SAMPLE_SIZE;

    entry.stateArchivalSettings().bucketListSizeWindowSampleSize =
        InitialSorobanNetworkConfig::BUCKET_LIST_SIZE_WINDOW_SAMPLE_SIZE;

    if (cfg.OVERRIDE_EVICTION_PARAMS_FOR_TESTING)
    {
        entry.stateArchivalSettings().evictionScanSize =
            cfg.TESTING_EVICTION_SCAN_SIZE;
        entry.stateArchivalSettings().maxEntriesToArchive =
            cfg.TESTING_MAX_ENTRIES_TO_ARCHIVE;
        entry.stateArchivalSettings().startingEvictionScanLevel =
            cfg.TESTING_STARTING_EVICTION_SCAN_LEVEL;
    }
    else
    {
        entry.stateArchivalSettings().evictionScanSize =
            InitialSorobanNetworkConfig::EVICTION_SCAN_SIZE;
        entry.stateArchivalSettings().maxEntriesToArchive =
            InitialSorobanNetworkConfig::MAX_ENTRIES_TO_ARCHIVE;
        entry.stateArchivalSettings().startingEvictionScanLevel =
            InitialSorobanNetworkConfig::STARTING_EVICTION_SCAN_LEVEL;
    }

    entry.stateArchivalSettings().persistentRentRateDenominator =
        InitialSorobanNetworkConfig::PERSISTENT_RENT_RATE_DENOMINATOR;
    entry.stateArchivalSettings().tempRentRateDenominator =
        InitialSorobanNetworkConfig::TEMP_RENT_RATE_DENOMINATOR;

    if (cfg.TESTING_SOROBAN_HIGH_LIMIT_OVERRIDE)
    {
        entry.stateArchivalSettings().maxEntryTTL =
            TestOverrideSorobanNetworkConfig::MAXIMUM_ENTRY_LIFETIME;
    }
    return entry;
}

ConfigSettingEntry
initialMemCostParamsEntry()
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
        case MemAlloc:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 16, 128};
            break;
        case MemCpy:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        case MemCmp:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        case DispatchHostFunction:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        case VisitObject:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        case ValSer:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 242, 384};
            break;
        case ValDeser:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 384};
            break;
        case ComputeSha256Hash:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        case ComputeEd25519PubKey:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        case VerifyEd25519Sig:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        case VmInstantiation:
            params[val] =
                ContractCostParamEntry{ExtensionPoint{0}, 130065, 5064};
            break;
        case VmCachedInstantiation:
            params[val] =
                ContractCostParamEntry{ExtensionPoint{0}, 130065, 5064};
            break;
        case InvokeVmFunction:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 14, 0};
            break;
        case ComputeKeccak256Hash:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        case ComputeEcdsaSecp256k1Sig:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        case RecoverEcdsaSecp256k1Key:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 181, 0};
            break;
        case Int256AddSub:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 99, 0};
            break;
        case Int256Mul:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 99, 0};
            break;
        case Int256Div:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 99, 0};
            break;
        case Int256Pow:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 99, 0};
            break;
        case Int256Shift:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 99, 0};
            break;
        case ChaCha20DrawBytes:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        }
    }
    return entry;
}

ConfigSettingEntry
initialBucketListSizeWindow(Application& app)
{
    ConfigSettingEntry entry(CONFIG_SETTING_BUCKETLIST_SIZE_WINDOW);

    // Populate 30 day sliding window of BucketList size snapshots with 30
    // copies of the current BL size. If the bucketlist is disabled for
    // testing, just fill with ones to avoid triggering asserts.
    auto blSize = app.getConfig().MODE_ENABLES_BUCKETLIST
                      ? app.getBucketManager().getBucketList().getSize()
                      : 1;
    for (uint64_t i = 0;
         i < InitialSorobanNetworkConfig::BUCKET_LIST_SIZE_WINDOW_SAMPLE_SIZE;
         ++i)
    {
        entry.bucketListSizeWindow().push_back(blSize);
    }

    return entry;
}

ConfigSettingEntry
initialEvictionIterator(Config const& cfg)
{
    ConfigSettingEntry entry(CONFIG_SETTING_EVICTION_ITERATOR);
    entry.evictionIterator().bucketFileOffset = 0;
    entry.evictionIterator().bucketListLevel =
        cfg.OVERRIDE_EVICTION_PARAMS_FOR_TESTING
            ? cfg.TESTING_STARTING_EVICTION_SCAN_LEVEL
            : InitialSorobanNetworkConfig::STARTING_EVICTION_SCAN_LEVEL;
    entry.evictionIterator().isCurrBucket = true;
    return entry;
}
}

bool
SorobanNetworkConfig::isValidConfigSettingEntry(ConfigSettingEntry const& cfg)
{
    bool valid = false;
    switch (cfg.configSettingID())
    {
    case ConfigSettingID::CONFIG_SETTING_CONTRACT_MAX_SIZE_BYTES:
        valid = cfg.contractMaxSizeBytes() >=
                MinimumSorobanNetworkConfig::MAX_CONTRACT_SIZE;
        break;
    case ConfigSettingID::CONFIG_SETTING_CONTRACT_COST_PARAMS_CPU_INSTRUCTIONS:
        valid = SorobanNetworkConfig::isValidCostParams(
            cfg.contractCostParamsCpuInsns());
        break;
    case ConfigSettingID::CONFIG_SETTING_CONTRACT_COST_PARAMS_MEMORY_BYTES:
        valid = SorobanNetworkConfig::isValidCostParams(
            cfg.contractCostParamsMemBytes());
        break;
    case ConfigSettingID::CONFIG_SETTING_CONTRACT_DATA_KEY_SIZE_BYTES:
        valid = cfg.contractDataKeySizeBytes() >=
                MinimumSorobanNetworkConfig::MAX_CONTRACT_DATA_KEY_SIZE_BYTES;
        break;
    case ConfigSettingID::CONFIG_SETTING_CONTRACT_DATA_ENTRY_SIZE_BYTES:
        valid = cfg.contractDataEntrySizeBytes() >=
                MinimumSorobanNetworkConfig::MAX_CONTRACT_DATA_ENTRY_SIZE_BYTES;
        break;
    case ConfigSettingID::CONFIG_SETTING_CONTRACT_EXECUTION_LANES:
        valid = true;
        break;
    case ConfigSettingID::CONFIG_SETTING_CONTRACT_BANDWIDTH_V0:
        valid = cfg.contractBandwidth().feeTxSize1KB >= 0 &&
                cfg.contractBandwidth().txMaxSizeBytes >=
                    MinimumSorobanNetworkConfig::TX_MAX_SIZE_BYTES &&
                cfg.contractBandwidth().ledgerMaxTxsSizeBytes >=
                    cfg.contractBandwidth().txMaxSizeBytes;
        break;
    case ConfigSettingID::CONFIG_SETTING_CONTRACT_COMPUTE_V0:
        valid = cfg.contractCompute().feeRatePerInstructionsIncrement >= 0 &&
                cfg.contractCompute().txMaxInstructions >=
                    MinimumSorobanNetworkConfig::TX_MAX_INSTRUCTIONS &&
                cfg.contractCompute().ledgerMaxInstructions >=
                    cfg.contractCompute().txMaxInstructions &&
                cfg.contractCompute().txMemoryLimit >=
                    MinimumSorobanNetworkConfig::MEMORY_LIMIT;
        break;
    case ConfigSettingID::CONFIG_SETTING_CONTRACT_HISTORICAL_DATA_V0:
        valid = cfg.contractHistoricalData().feeHistorical1KB >= 0;
        break;
    case ConfigSettingID::CONFIG_SETTING_CONTRACT_LEDGER_COST_V0:
        valid = cfg.contractLedgerCost().txMaxReadLedgerEntries >=
                    MinimumSorobanNetworkConfig::TX_MAX_READ_LEDGER_ENTRIES &&
                cfg.contractLedgerCost().ledgerMaxReadLedgerEntries >=
                    cfg.contractLedgerCost().txMaxReadLedgerEntries &&
                cfg.contractLedgerCost().txMaxReadBytes >=
                    MinimumSorobanNetworkConfig::TX_MAX_READ_BYTES &&
                cfg.contractLedgerCost().ledgerMaxReadBytes >=
                    cfg.contractLedgerCost().txMaxReadBytes &&
                cfg.contractLedgerCost().txMaxWriteLedgerEntries >=
                    MinimumSorobanNetworkConfig::TX_MAX_WRITE_LEDGER_ENTRIES &&
                cfg.contractLedgerCost().ledgerMaxWriteLedgerEntries >=
                    cfg.contractLedgerCost().txMaxWriteLedgerEntries &&
                cfg.contractLedgerCost().txMaxWriteBytes >=
                    MinimumSorobanNetworkConfig::TX_MAX_WRITE_BYTES &&
                cfg.contractLedgerCost().ledgerMaxWriteBytes >=
                    cfg.contractLedgerCost().txMaxWriteBytes &&
                cfg.contractLedgerCost().feeReadLedgerEntry >= 0 &&
                cfg.contractLedgerCost().feeWriteLedgerEntry >= 0 &&
                cfg.contractLedgerCost().feeRead1KB >= 0 &&
                cfg.contractLedgerCost().bucketListTargetSizeBytes > 0 &&
                cfg.contractLedgerCost().writeFee1KBBucketListLow >= 0 &&
                cfg.contractLedgerCost().writeFee1KBBucketListHigh >= 0 &&
                cfg.contractLedgerCost().bucketListWriteFeeGrowthFactor >= 0;
        break;
    case ConfigSettingID::CONFIG_SETTING_CONTRACT_EVENTS_V0:
        valid = cfg.contractEvents().txMaxContractEventsSizeBytes >=
                    MinimumSorobanNetworkConfig::
                        TX_MAX_CONTRACT_EVENTS_SIZE_BYTES &&
                cfg.contractEvents().feeContractEvents1KB >= 0;
        break;
    case ConfigSettingID::CONFIG_SETTING_STATE_ARCHIVAL:
        valid = cfg.stateArchivalSettings().maxEntryTTL >=
                    MinimumSorobanNetworkConfig::MAXIMUM_ENTRY_LIFETIME &&
                cfg.stateArchivalSettings().minTemporaryTTL >=
                    MinimumSorobanNetworkConfig::MINIMUM_TEMP_ENTRY_LIFETIME &&
                cfg.stateArchivalSettings().minPersistentTTL >=
                    MinimumSorobanNetworkConfig::
                        MINIMUM_PERSISTENT_ENTRY_LIFETIME &&
                cfg.stateArchivalSettings().persistentRentRateDenominator > 0 &&
                cfg.stateArchivalSettings().tempRentRateDenominator > 0 &&
                cfg.stateArchivalSettings().maxEntriesToArchive >=
                    MinimumSorobanNetworkConfig::MAX_ENTRIES_TO_ARCHIVE &&
                cfg.stateArchivalSettings().bucketListSizeWindowSampleSize >=
                    MinimumSorobanNetworkConfig::
                        BUCKETLIST_SIZE_WINDOW_SAMPLE_SIZE &&
                cfg.stateArchivalSettings().evictionScanSize >=
                    MinimumSorobanNetworkConfig::EVICTION_SCAN_SIZE &&
                cfg.stateArchivalSettings().startingEvictionScanLevel >=
                    MinimumSorobanNetworkConfig::STARTING_EVICTION_LEVEL &&
                cfg.stateArchivalSettings().startingEvictionScanLevel <
                    BucketList::kNumLevels;

        valid = valid && cfg.stateArchivalSettings().maxEntryTTL >
                             cfg.stateArchivalSettings().minPersistentTTL;
        valid = valid && cfg.stateArchivalSettings().maxEntryTTL >
                             cfg.stateArchivalSettings().minTemporaryTTL;
        break;
    case ConfigSettingID::CONFIG_SETTING_BUCKETLIST_SIZE_WINDOW:
    case ConfigSettingID::CONFIG_SETTING_EVICTION_ITERATOR:
        valid = true;
        break;
    }
    return valid;
}

bool
SorobanNetworkConfig::isNonUpgradeableConfigSettingEntry(
    ConfigSettingEntry const& cfg)
{
    // While the BucketList size window and eviction iterator are stored in a
    // ConfigSetting entry, the BucketList defines these values, they should
    // never be changed via upgrade
    return cfg.configSettingID() ==
               ConfigSettingID::CONFIG_SETTING_BUCKETLIST_SIZE_WINDOW ||
           cfg.configSettingID() ==
               ConfigSettingID::CONFIG_SETTING_EVICTION_ITERATOR;
}

void
SorobanNetworkConfig::createLedgerEntriesForV20(AbstractLedgerTxn& ltx,
                                                Application& app)
{
    ZoneScoped;

    auto const& cfg = app.getConfig();
    createConfigSettingEntry(initialMaxContractSizeEntry(cfg), ltx);
    createConfigSettingEntry(initialMaxContractDataKeySizeEntry(cfg), ltx);
    createConfigSettingEntry(initialMaxContractDataEntrySizeEntry(cfg), ltx);
    createConfigSettingEntry(initialContractComputeSettingsEntry(cfg), ltx);
    createConfigSettingEntry(initialContractLedgerAccessSettingsEntry(cfg),
                             ltx);
    createConfigSettingEntry(initialContractHistoricalDataSettingsEntry(), ltx);
    createConfigSettingEntry(initialContractEventsSettingsEntry(cfg), ltx);
    createConfigSettingEntry(initialContractBandwidthSettingsEntry(cfg), ltx);
    createConfigSettingEntry(initialContractExecutionLanesSettingsEntry(cfg),
                             ltx);
    createConfigSettingEntry(initialCpuCostParamsEntry(), ltx);
    createConfigSettingEntry(initialMemCostParamsEntry(), ltx);
    createConfigSettingEntry(initialStateArchivalSettings(cfg), ltx);
    createConfigSettingEntry(initialBucketListSizeWindow(app), ltx);
    createConfigSettingEntry(initialEvictionIterator(cfg), ltx);
}

void
SorobanNetworkConfig::initializeGenesisLedgerForTesting(
    uint32_t genesisLedgerProtocol, AbstractLedgerTxn& ltx, Application& app)
{
    if (protocolVersionStartsFrom(genesisLedgerProtocol,
                                  SOROBAN_PROTOCOL_VERSION))
    {
        SorobanNetworkConfig::createLedgerEntriesForV20(ltx, app);
    }
}

void
SorobanNetworkConfig::loadFromLedger(AbstractLedgerTxn& ltxRoot,
                                     uint32_t configMaxProtocol,
                                     uint32_t protocolVersion)
{
    ZoneScoped;

    LedgerTxn ltx(ltxRoot, false, TransactionMode::READ_ONLY_WITHOUT_SQL_TXN);
    loadMaxContractSize(ltx);
    loadMaxContractDataKeySize(ltx);
    loadMaxContractDataEntrySize(ltx);
    loadComputeSettings(ltx);
    loadLedgerAccessSettings(ltx);
    loadHistoricalSettings(ltx);
    loadContractEventsSettings(ltx);
    loadBandwidthSettings(ltx);
    loadCpuCostParams(ltx);
    loadMemCostParams(ltx);
    loadStateArchivalSettings(ltx);
    loadExecutionLanesSettings(ltx);
    loadBucketListSizeWindow(ltx);
    loadEvictionIterator(ltx);
    // NB: this should follow loading state archival settings
    maybeUpdateBucketListWindowSize(ltx);
    // NB: this should follow loading/updating bucket list window
    // size and state archival settings
    computeWriteFee(configMaxProtocol, protocolVersion);
}

void
SorobanNetworkConfig::loadMaxContractSize(AbstractLedgerTxn& ltx)
{
    ZoneScoped;

    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_MAX_SIZE_BYTES;
    auto le = ltx.loadWithoutRecord(key).current();
    mMaxContractSizeBytes = le.data.configSetting().contractMaxSizeBytes();
}

void
SorobanNetworkConfig::loadMaxContractDataKeySize(AbstractLedgerTxn& ltx)
{
    ZoneScoped;

    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_DATA_KEY_SIZE_BYTES;
    auto le = ltx.loadWithoutRecord(key).current();
    mMaxContractDataKeySizeBytes =
        le.data.configSetting().contractDataKeySizeBytes();
}

void
SorobanNetworkConfig::loadMaxContractDataEntrySize(AbstractLedgerTxn& ltx)
{
    ZoneScoped;

    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_DATA_ENTRY_SIZE_BYTES;
    auto le = ltx.loadWithoutRecord(key).current();
    mMaxContractDataEntrySizeBytes =
        le.data.configSetting().contractDataEntrySizeBytes();
}

void
SorobanNetworkConfig::loadComputeSettings(AbstractLedgerTxn& ltx)
{
    ZoneScoped;

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
}

void
SorobanNetworkConfig::loadLedgerAccessSettings(AbstractLedgerTxn& ltx)
{
    ZoneScoped;

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
    mBucketListTargetSizeBytes = configSetting.bucketListTargetSizeBytes;
    mWriteFee1KBBucketListLow = configSetting.writeFee1KBBucketListLow;
    mWriteFee1KBBucketListHigh = configSetting.writeFee1KBBucketListHigh;
    mBucketListWriteFeeGrowthFactor =
        configSetting.bucketListWriteFeeGrowthFactor;
}

void
SorobanNetworkConfig::loadHistoricalSettings(AbstractLedgerTxn& ltx)
{
    ZoneScoped;

    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_HISTORICAL_DATA_V0;
    auto le = ltx.loadWithoutRecord(key).current();
    auto const& configSetting =
        le.data.configSetting().contractHistoricalData();
    mFeeHistorical1KB = configSetting.feeHistorical1KB;
}

void
SorobanNetworkConfig::loadContractEventsSettings(AbstractLedgerTxn& ltx)
{
    ZoneScoped;

    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_EVENTS_V0;
    auto le = ltx.loadWithoutRecord(key).current();
    auto const& configSetting = le.data.configSetting().contractEvents();
    mFeeContractEvents1KB = configSetting.feeContractEvents1KB;
    mTxMaxContractEventsSizeBytes = configSetting.txMaxContractEventsSizeBytes;
}

void
SorobanNetworkConfig::loadBandwidthSettings(AbstractLedgerTxn& ltx)
{
    ZoneScoped;

    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_BANDWIDTH_V0;
    auto le = ltx.loadWithoutRecord(key).current();
    auto const& configSetting = le.data.configSetting().contractBandwidth();
    mLedgerMaxTransactionsSizeBytes = configSetting.ledgerMaxTxsSizeBytes;
    mTxMaxSizeBytes = configSetting.txMaxSizeBytes;
    mFeeTransactionSize1KB = configSetting.feeTxSize1KB;
}

void
SorobanNetworkConfig::loadCpuCostParams(AbstractLedgerTxn& ltx)
{
    ZoneScoped;

    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_COST_PARAMS_CPU_INSTRUCTIONS;
    auto le = ltx.loadWithoutRecord(key).current();
    mCpuCostParams = le.data.configSetting().contractCostParamsCpuInsns();
}

void
SorobanNetworkConfig::loadMemCostParams(AbstractLedgerTxn& ltx)
{
    ZoneScoped;

    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_COST_PARAMS_MEMORY_BYTES;
    auto le = ltx.loadWithoutRecord(key).current();
    mMemCostParams = le.data.configSetting().contractCostParamsMemBytes();
}

void
SorobanNetworkConfig::loadExecutionLanesSettings(AbstractLedgerTxn& ltx)
{
    ZoneScoped;

    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_EXECUTION_LANES;
    auto le = ltx.loadWithoutRecord(key).current();
    auto const& configSetting =
        le.data.configSetting().contractExecutionLanes();
    mLedgerMaxTxCount = configSetting.ledgerMaxTxCount;
}

void
SorobanNetworkConfig::loadBucketListSizeWindow(AbstractLedgerTxn& ltx)
{
    ZoneScoped;

    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_BUCKETLIST_SIZE_WINDOW;
    auto txle = ltx.loadWithoutRecord(key);
    releaseAssert(txle);
    auto const& leVector =
        txle.current().data.configSetting().bucketListSizeWindow();
    mBucketListSizeSnapshots.clear();
    for (auto e : leVector)
    {
        mBucketListSizeSnapshots.push_back(e);
    }

    updateBucketListSizeAverage();
}

void
SorobanNetworkConfig::loadEvictionIterator(AbstractLedgerTxn& ltx)
{
    ZoneScoped;

    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_EVICTION_ITERATOR;
    auto txle = ltx.loadWithoutRecord(key);
    releaseAssert(txle);
    mEvictionIterator = txle.current().data.configSetting().evictionIterator();
}

void
SorobanNetworkConfig::writeBucketListSizeWindow(
    AbstractLedgerTxn& ltxRoot) const
{
    ZoneScoped;

    // Check that the window is loaded and the number of snapshots is correct
    releaseAssert(mBucketListSizeSnapshots.size() ==
                  mStateArchivalSettings.bucketListSizeWindowSampleSize);

    // Load outdated snapshot entry from DB
    LedgerTxn ltx(ltxRoot);
    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_BUCKETLIST_SIZE_WINDOW;
    auto txle = ltx.load(key);
    releaseAssert(txle);

    // Copy in-memory snapshots to ledger entry
    auto& leVector = txle.current().data.configSetting().bucketListSizeWindow();
    leVector.clear();
    for (auto e : mBucketListSizeSnapshots)
    {
        leVector.push_back(e);
    }

    ltx.commit();
}

void
SorobanNetworkConfig::updateBucketListSizeAverage()
{
    ZoneScoped;

    releaseAssert(!mBucketListSizeSnapshots.empty());
    uint64_t sizeSum = 0;
    for (auto const& size : mBucketListSizeSnapshots)
    {
        sizeSum += size;
    }

    mAverageBucketListSize = sizeSum / mBucketListSizeSnapshots.size();
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
SorobanNetworkConfig::loadStateArchivalSettings(AbstractLedgerTxn& ltx)
{
    ZoneScoped;

    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_STATE_ARCHIVAL;
    auto le = ltx.loadWithoutRecord(key).current();
    mStateArchivalSettings = le.data.configSetting().stateArchivalSettings();
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

// Historical data (pushed to core archives) settings for contracts.
int64_t
SorobanNetworkConfig::feeHistorical1KB() const
{
    return mFeeHistorical1KB;
}

// Maximum size of the emitted contract events.
uint32_t
SorobanNetworkConfig::txMaxContractEventsSizeBytes() const
{
    return mTxMaxContractEventsSizeBytes;
}

int64_t
SorobanNetworkConfig::feeContractEventsSize1KB() const
{
    return mFeeContractEvents1KB;
}

// Bandwidth related data settings for contracts
uint32_t
SorobanNetworkConfig::ledgerMaxTransactionSizesBytes() const
{
    return mLedgerMaxTransactionsSizeBytes;
}

uint32_t
SorobanNetworkConfig::txMaxSizeBytes() const
{
    return mTxMaxSizeBytes;
}

int64_t
SorobanNetworkConfig::feeTransactionSize1KB() const
{
    return mFeeTransactionSize1KB;
}

// General execution lanes settings for contracts
uint32_t
SorobanNetworkConfig::ledgerMaxTxCount() const
{
    return mLedgerMaxTxCount;
}

uint32_t
SorobanNetworkConfig::getBucketListSizeSnapshotPeriod() const
{
#ifdef BUILD_TESTS
    if (mBucketListSnapshotPeriodForTesting)
    {
        return *mBucketListSnapshotPeriodForTesting;
    }
#endif

    return BUCKETLIST_SIZE_SNAPSHOT_PERIOD;
}

void
SorobanNetworkConfig::maybeUpdateBucketListWindowSize(AbstractLedgerTxn& ltx)
{
    ZoneScoped;

    // // Check if BucketList size window should exist
    if (protocolVersionIsBefore(ltx.loadHeader().current().ledgerVersion,
                                SOROBAN_PROTOCOL_VERSION))
    {
        return;
    }
    auto currSize = mBucketListSizeSnapshots.size();
    auto newSize = stateArchivalSettings().bucketListSizeWindowSampleSize;
    if (newSize == currSize)
    {
        // No size change, nothing to update
        return;
    }

    if (newSize < currSize)
    {
        while (mBucketListSizeSnapshots.size() != newSize)
        {
            mBucketListSizeSnapshots.pop_front();
        }
    }
    // If newSize > currSize, backfill new slots with oldest value in window
    // such that they are the first to get replaced by new values
    else
    {
        auto oldestSize = mBucketListSizeSnapshots.front();
        while (mBucketListSizeSnapshots.size() != newSize)
        {
            mBucketListSizeSnapshots.push_front(oldestSize);
        }
    }

    updateBucketListSizeAverage();
    writeBucketListSizeWindow(ltx);
}

void
SorobanNetworkConfig::maybeSnapshotBucketListSize(uint32_t currLedger,
                                                  AbstractLedgerTxn& ltx,
                                                  Application& app)
{
    ZoneScoped;

    auto ledgerVersion = ltx.loadHeader().current().ledgerVersion;
    // // Check if BucketList size window should exist
    if (protocolVersionIsBefore(ledgerVersion, SOROBAN_PROTOCOL_VERSION) ||
        !app.getConfig().MODE_ENABLES_BUCKETLIST)
    {
        return;
    }

    if (currLedger % getBucketListSizeSnapshotPeriod() == 0)
    {
        // Update in memory snapshots
        mBucketListSizeSnapshots.pop_front();
        mBucketListSizeSnapshots.push_back(
            app.getBucketManager().getBucketList().getSize());

        writeBucketListSizeWindow(ltx);
        updateBucketListSizeAverage();
        computeWriteFee(app.getConfig().CURRENT_LEDGER_PROTOCOL_VERSION,
                        ledgerVersion);
    }
}

uint64_t
SorobanNetworkConfig::getAverageBucketListSize() const
{
    return mAverageBucketListSize;
}

#ifdef BUILD_TESTS
void
SorobanNetworkConfig::setBucketListSnapshotPeriodForTesting(uint32_t period)
{
    mBucketListSnapshotPeriodForTesting = period;
}

void
writeConfigSettingEntry(ConfigSettingEntry const& configSetting,
                        AbstractLedgerTxn& ltxRoot)
{
    ZoneScoped;

    LedgerEntry e;
    e.data.type(CONFIG_SETTING);
    e.data.configSetting() = configSetting;

    LedgerTxn ltx(ltxRoot);
    auto ltxe = ltx.load(LedgerEntryKey(e));
    releaseAssert(ltxe);
    ltxe.current() = e;
    ltx.commit();
}

void
SorobanNetworkConfig::writeAllSettings(AbstractLedgerTxn& ltx) const
{
    ZoneScoped;

    ConfigSettingEntry maxContractSizeEntry(
        CONFIG_SETTING_CONTRACT_MAX_SIZE_BYTES);
    maxContractSizeEntry.contractMaxSizeBytes() = mMaxContractSizeBytes;
    writeConfigSettingEntry(maxContractSizeEntry, ltx);

    ConfigSettingEntry maxContractDataKeySizeEntry(
        CONFIG_SETTING_CONTRACT_DATA_KEY_SIZE_BYTES);
    maxContractDataKeySizeEntry.contractDataKeySizeBytes() =
        mMaxContractDataKeySizeBytes;
    writeConfigSettingEntry(maxContractDataKeySizeEntry, ltx);

    ConfigSettingEntry maxContractDataEntrySizeEntry(
        CONFIG_SETTING_CONTRACT_DATA_ENTRY_SIZE_BYTES);
    maxContractDataEntrySizeEntry.contractDataEntrySizeBytes() =
        mMaxContractDataEntrySizeBytes;
    writeConfigSettingEntry(maxContractDataEntrySizeEntry, ltx);

    ConfigSettingEntry computeSettingsEntry(CONFIG_SETTING_CONTRACT_COMPUTE_V0);
    computeSettingsEntry.contractCompute().ledgerMaxInstructions =
        mLedgerMaxInstructions;
    computeSettingsEntry.contractCompute().txMaxInstructions =
        mTxMaxInstructions;
    computeSettingsEntry.contractCompute().feeRatePerInstructionsIncrement =
        mFeeRatePerInstructionsIncrement;
    computeSettingsEntry.contractCompute().txMemoryLimit = mTxMemoryLimit;
    writeConfigSettingEntry(computeSettingsEntry, ltx);

    ConfigSettingEntry ledgerAccessSettingsEntry(
        CONFIG_SETTING_CONTRACT_LEDGER_COST_V0);
    auto& cost = ledgerAccessSettingsEntry.contractLedgerCost();
    cost.ledgerMaxReadBytes = mLedgerMaxReadBytes;
    cost.ledgerMaxReadLedgerEntries = mLedgerMaxReadLedgerEntries;
    cost.ledgerMaxWriteBytes = mLedgerMaxWriteBytes;
    cost.ledgerMaxWriteLedgerEntries = mLedgerMaxWriteLedgerEntries;
    cost.txMaxReadBytes = mTxMaxReadBytes;
    cost.txMaxReadLedgerEntries = mTxMaxReadLedgerEntries;
    cost.txMaxWriteBytes = mTxMaxWriteBytes;
    cost.txMaxWriteLedgerEntries = mTxMaxWriteLedgerEntries;
    cost.feeReadLedgerEntry = mFeeReadLedgerEntry;
    cost.feeWriteLedgerEntry = mFeeWriteLedgerEntry;
    cost.feeRead1KB = mFeeRead1KB;
    cost.bucketListTargetSizeBytes = mBucketListTargetSizeBytes;
    cost.writeFee1KBBucketListLow = mWriteFee1KBBucketListLow;
    cost.writeFee1KBBucketListHigh = mWriteFee1KBBucketListHigh;
    cost.bucketListWriteFeeGrowthFactor = mBucketListWriteFeeGrowthFactor;
    writeConfigSettingEntry(ledgerAccessSettingsEntry, ltx);

    ConfigSettingEntry historicalSettingsEntry(
        CONFIG_SETTING_CONTRACT_HISTORICAL_DATA_V0);
    historicalSettingsEntry.contractHistoricalData().feeHistorical1KB =
        mFeeHistorical1KB;
    writeConfigSettingEntry(historicalSettingsEntry, ltx);

    ConfigSettingEntry contractEventsSettingsEntry(
        CONFIG_SETTING_CONTRACT_EVENTS_V0);
    contractEventsSettingsEntry.contractEvents().feeContractEvents1KB =
        mFeeContractEvents1KB;
    contractEventsSettingsEntry.contractEvents().txMaxContractEventsSizeBytes =
        mTxMaxContractEventsSizeBytes;
    writeConfigSettingEntry(contractEventsSettingsEntry, ltx);

    ConfigSettingEntry bandwidthSettingsEntry(
        CONFIG_SETTING_CONTRACT_BANDWIDTH_V0);
    bandwidthSettingsEntry.contractBandwidth().ledgerMaxTxsSizeBytes =
        mLedgerMaxTransactionsSizeBytes;
    bandwidthSettingsEntry.contractBandwidth().txMaxSizeBytes = mTxMaxSizeBytes;
    bandwidthSettingsEntry.contractBandwidth().feeTxSize1KB =
        mFeeTransactionSize1KB;
    writeConfigSettingEntry(bandwidthSettingsEntry, ltx);

    ConfigSettingEntry executionLanesSettingsEntry(
        CONFIG_SETTING_CONTRACT_EXECUTION_LANES);
    executionLanesSettingsEntry.contractExecutionLanes().ledgerMaxTxCount =
        mLedgerMaxTxCount;
    writeConfigSettingEntry(executionLanesSettingsEntry, ltx);

    ConfigSettingEntry cpuCostParamsEntry(
        CONFIG_SETTING_CONTRACT_COST_PARAMS_CPU_INSTRUCTIONS);
    cpuCostParamsEntry.contractCostParamsCpuInsns() = mCpuCostParams;
    writeConfigSettingEntry(cpuCostParamsEntry, ltx);

    ConfigSettingEntry memCostParamsEntry(
        CONFIG_SETTING_CONTRACT_COST_PARAMS_MEMORY_BYTES);
    memCostParamsEntry.contractCostParamsMemBytes() = mMemCostParams;
    writeConfigSettingEntry(memCostParamsEntry, ltx);

    ConfigSettingEntry stateArchivalSettingsEntry(
        CONFIG_SETTING_STATE_ARCHIVAL);
    stateArchivalSettingsEntry.stateArchivalSettings() = mStateArchivalSettings;
    writeConfigSettingEntry(stateArchivalSettingsEntry, ltx);

    writeBucketListSizeWindow(ltx);
    updateEvictionIterator(ltx, mEvictionIterator);
}
#endif

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

StateArchivalSettings const&
SorobanNetworkConfig::stateArchivalSettings() const
{
    return mStateArchivalSettings;
}

EvictionIterator const&
SorobanNetworkConfig::evictionIterator() const
{
    return mEvictionIterator;
}

void
SorobanNetworkConfig::updateEvictionIterator(
    AbstractLedgerTxn& ltxRoot, EvictionIterator const& newIter) const
{
    ZoneScoped;

    mEvictionIterator = newIter;

    LedgerTxn ltx(ltxRoot);
    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_EVICTION_ITERATOR;
    auto txle = ltx.load(key);
    releaseAssert(txle);

    txle.current().data.configSetting().evictionIterator() = mEvictionIterator;
    ltx.commit();
}

#ifdef BUILD_TESTS
StateArchivalSettings&
SorobanNetworkConfig::stateArchivalSettings()
{
    return mStateArchivalSettings;
}

EvictionIterator&
SorobanNetworkConfig::evictionIterator()
{
    return mEvictionIterator;
}
#endif

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
    CxxFeeConfiguration res{};
    res.fee_per_instruction_increment = feeRatePerInstructionsIncrement();

    res.fee_per_read_entry = feeReadLedgerEntry();
    res.fee_per_write_entry = feeWriteLedgerEntry();
    res.fee_per_read_1kb = feeRead1KB();
    // This should be dependent on the ledger size, but initially
    // we'll just use the flat rate here.
    res.fee_per_write_1kb = feeWrite1KB();

    res.fee_per_transaction_size_1kb = feeTransactionSize1KB();

    res.fee_per_contract_event_1kb = feeContractEventsSize1KB();

    res.fee_per_historical_1kb = feeHistorical1KB();

    return res;
}

CxxRentFeeConfiguration
SorobanNetworkConfig::rustBridgeRentFeeConfiguration() const
{
    CxxRentFeeConfiguration res{};
    auto const& cfg = stateArchivalSettings();
    res.fee_per_write_1kb = feeWrite1KB();
    res.fee_per_write_entry = feeWriteLedgerEntry();
    res.persistent_rent_rate_denominator = cfg.persistentRentRateDenominator;
    res.temporary_rent_rate_denominator = cfg.tempRentRateDenominator;
    return res;
}

void
SorobanNetworkConfig::computeWriteFee(uint32_t configMaxProtocol,
                                      uint32_t protocolVersion)
{
    ZoneScoped;

    CxxWriteFeeConfiguration feeConfig{};
    feeConfig.bucket_list_target_size_bytes = mBucketListTargetSizeBytes;
    feeConfig.bucket_list_write_fee_growth_factor =
        mBucketListWriteFeeGrowthFactor;
    feeConfig.write_fee_1kb_bucket_list_low = mWriteFee1KBBucketListLow;
    feeConfig.write_fee_1kb_bucket_list_high = mWriteFee1KBBucketListHigh;
    // This may throw, but only if core is mis-configured.
    mFeeWrite1KB = rust_bridge::compute_write_fee_per_1kb(
        configMaxProtocol, protocolVersion, mAverageBucketListSize, feeConfig);
}
} // namespace stellar
