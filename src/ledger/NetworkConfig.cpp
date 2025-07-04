// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/NetworkConfig.h"
#include "bucket/BucketManager.h"
#include "bucket/LiveBucketList.h"
#include "bucket/test/BucketTestUtils.h"
#include "main/Application.h"
#include "util/ProtocolVersion.h"
#include <Tracy.hpp>

#ifdef BUILD_TESTS
#include "ledger/LedgerManager.h"
#endif

namespace stellar
{
namespace
{
void
createConfigSettingEntry(ConfigSettingEntry const& configSetting,
                         AbstractLedgerTxn& ltxRoot,
                         uint32_t versionToValidateAgainst)
{
    ZoneScoped;

    if (!SorobanNetworkConfig::isValidConfigSettingEntry(
            configSetting, versionToValidateAgainst))
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

    e.ledgerMaxDiskReadEntries =
        InitialSorobanNetworkConfig::LEDGER_MAX_READ_LEDGER_ENTRIES;

    e.ledgerMaxDiskReadBytes =
        InitialSorobanNetworkConfig::LEDGER_MAX_READ_BYTES;

    e.ledgerMaxWriteLedgerEntries =
        InitialSorobanNetworkConfig::LEDGER_MAX_WRITE_LEDGER_ENTRIES;

    e.ledgerMaxWriteBytes = InitialSorobanNetworkConfig::LEDGER_MAX_WRITE_BYTES;

    e.txMaxDiskReadEntries =
        InitialSorobanNetworkConfig::TX_MAX_READ_LEDGER_ENTRIES;
    e.txMaxDiskReadBytes = InitialSorobanNetworkConfig::TX_MAX_READ_BYTES;
    e.txMaxWriteLedgerEntries =
        InitialSorobanNetworkConfig::TX_MAX_WRITE_LEDGER_ENTRIES;
    e.txMaxWriteBytes = InitialSorobanNetworkConfig::TX_MAX_WRITE_BYTES;
    e.feeDiskReadLedgerEntry =
        InitialSorobanNetworkConfig::FEE_READ_LEDGER_ENTRY;
    e.feeWriteLedgerEntry = InitialSorobanNetworkConfig::FEE_WRITE_LEDGER_ENTRY;
    e.feeDiskRead1KB = InitialSorobanNetworkConfig::FEE_READ_1KB;
    e.sorobanStateTargetSizeBytes =
        InitialSorobanNetworkConfig::BUCKET_LIST_TARGET_SIZE_BYTES;
    e.rentFee1KBSorobanStateSizeLow =
        InitialSorobanNetworkConfig::BUCKET_LIST_FEE_1KB_BUCKET_LIST_LOW;
    e.rentFee1KBSorobanStateSizeHigh =
        InitialSorobanNetworkConfig::BUCKET_LIST_FEE_1KB_BUCKET_LIST_HIGH;
    e.sorobanStateRentFeeGrowthFactor =
        InitialSorobanNetworkConfig::STATE_SIZE_RENT_FEE_GROWTH_FACTOR;

    if (cfg.TESTING_SOROBAN_HIGH_LIMIT_OVERRIDE)
    {
        e.ledgerMaxDiskReadEntries =
            TestOverrideSorobanNetworkConfig::LEDGER_MAX_READ_LEDGER_ENTRIES;
        e.ledgerMaxDiskReadBytes =
            TestOverrideSorobanNetworkConfig::LEDGER_MAX_READ_BYTES;
        e.ledgerMaxWriteLedgerEntries =
            TestOverrideSorobanNetworkConfig::LEDGER_MAX_WRITE_LEDGER_ENTRIES;
        e.ledgerMaxWriteBytes =
            TestOverrideSorobanNetworkConfig::LEDGER_MAX_WRITE_BYTES;
        e.txMaxDiskReadEntries =
            TestOverrideSorobanNetworkConfig::TX_MAX_READ_LEDGER_ENTRIES;
        e.txMaxDiskReadBytes =
            TestOverrideSorobanNetworkConfig::TX_MAX_READ_BYTES;
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
initialCpuCostParamsEntryForV20()
{
    ConfigSettingEntry entry(
        CONFIG_SETTING_CONTRACT_COST_PARAMS_CPU_INSTRUCTIONS);

    auto& params = entry.contractCostParamsCpuInsns();
    auto max_index = static_cast<uint32>(ContractCostType::ChaCha20DrawBytes);
    params.resize(max_index + 1);

    for (size_t val = 0; val <= max_index; ++val)
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
        case DecodeEcdsaCurve256Sig:
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
        default:
            break;
        }
    }

    return entry;
}

void
updateCpuCostParamsEntryForV21(AbstractLedgerTxn& ltxRoot)
{
    LedgerTxn ltx(ltxRoot);

    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_COST_PARAMS_CPU_INSTRUCTIONS;

    auto& params = ltx.load(key)
                       .current()
                       .data.configSetting()
                       .contractCostParamsCpuInsns();

    // Resize to fit the last cost type added in v21
    params.resize(
        static_cast<uint32>(ContractCostType::VerifyEcdsaSecp256r1Sig) + 1);

    auto const& vals = xdr::xdr_traits<ContractCostType>::enum_values();

    // While we loop over the full ContractCostType enum, we only set the range
    for (auto val : vals)
    {
        switch (val)
        {
        // VmCachedInstantiation is the only one we're updating. The rest are
        // new.
        case VmCachedInstantiation:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 41142, 634};
            break;
        case ParseWasmInstructions:
            params[val] =
                ContractCostParamEntry{ExtensionPoint{0}, 73077, 25410};
            break;
        case ParseWasmFunctions:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 540752};
            break;
        case ParseWasmGlobals:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 176363};
            break;
        case ParseWasmTableEntries:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 29989};
            break;
        case ParseWasmTypes:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 1061449};
            break;
        case ParseWasmDataSegments:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 237336};
            break;
        case ParseWasmElemSegments:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 328476};
            break;
        case ParseWasmImports:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 701845};
            break;
        case ParseWasmExports:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 429383};
            break;
        case ParseWasmDataSegmentBytes:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 28};
            break;
        case InstantiateWasmInstructions:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 43030, 0};
            break;
        case InstantiateWasmFunctions:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 7556};
            break;
        case InstantiateWasmGlobals:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 10711};
            break;
        case InstantiateWasmTableEntries:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 3300};
            break;
        case InstantiateWasmTypes:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        case InstantiateWasmDataSegments:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 23038};
            break;
        case InstantiateWasmElemSegments:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 42488};
            break;
        case InstantiateWasmImports:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 828974};
            break;
        case InstantiateWasmExports:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 297100};
            break;
        case InstantiateWasmDataSegmentBytes:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 14};
            break;
        case Sec1DecodePointUncompressed:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 1882, 0};
            break;
        case VerifyEcdsaSecp256r1Sig:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 3000906, 0};
            break;
        default:
            break;
        }
    }
    ltx.commit();
}

void
updateCpuCostParamsEntryForV22(AbstractLedgerTxn& ltxRoot)
{
    LedgerTxn ltx(ltxRoot);

    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_COST_PARAMS_CPU_INSTRUCTIONS;

    auto& params = ltx.load(key)
                       .current()
                       .data.configSetting()
                       .contractCostParamsCpuInsns();

    auto const& vals = xdr::xdr_traits<ContractCostType>::enum_values();

    // Resize to fit the last cost type added in v22
    params.resize(static_cast<uint32>(ContractCostType::Bls12381FrInv) + 1);

    // While we loop over the full ContractCostType enum, we only set the
    // entries that have either been updated, or newly created in p22
    for (auto val : vals)
    {
        switch (val)
        {
        // adding new cost types introduced in p22
        case Bls12381EncodeFp:
            params[val] = ContractCostParamEntry(ExtensionPoint{0}, 661, 0);
            break;
        case Bls12381DecodeFp:
            params[val] = ContractCostParamEntry(ExtensionPoint{0}, 985, 0);
            break;
        case Bls12381G1CheckPointOnCurve:
            params[val] = ContractCostParamEntry(ExtensionPoint{0}, 1934, 0);
            break;
        case Bls12381G1CheckPointInSubgroup:
            params[val] = ContractCostParamEntry(ExtensionPoint{0}, 730510, 0);
            break;
        case Bls12381G2CheckPointOnCurve:
            params[val] = ContractCostParamEntry(ExtensionPoint{0}, 5921, 0);
            break;
        case Bls12381G2CheckPointInSubgroup:
            params[val] = ContractCostParamEntry(ExtensionPoint{0}, 1057822, 0);
            break;
        case Bls12381G1ProjectiveToAffine:
            params[val] = ContractCostParamEntry(ExtensionPoint{0}, 92642, 0);
            break;
        case Bls12381G2ProjectiveToAffine:
            params[val] = ContractCostParamEntry(ExtensionPoint{0}, 100742, 0);
            break;
        case Bls12381G1Add:
            params[val] = ContractCostParamEntry(ExtensionPoint{0}, 7689, 0);
            break;
        case Bls12381G1Mul:
            params[val] = ContractCostParamEntry(ExtensionPoint{0}, 2458985, 0);
            break;
        case Bls12381G1Msm:
            params[val] =
                ContractCostParamEntry(ExtensionPoint{0}, 2426722, 96397671);
            break;
        case Bls12381MapFpToG1:
            params[val] = ContractCostParamEntry(ExtensionPoint{0}, 1541554, 0);
            break;
        case Bls12381HashToG1:
            params[val] =
                ContractCostParamEntry(ExtensionPoint{0}, 3211191, 6713);
            break;
        case Bls12381G2Add:
            params[val] = ContractCostParamEntry(ExtensionPoint{0}, 25207, 0);
            break;
        case Bls12381G2Mul:
            params[val] = ContractCostParamEntry(ExtensionPoint{0}, 7873219, 0);
            break;
        case Bls12381G2Msm:
            params[val] =
                ContractCostParamEntry(ExtensionPoint{0}, 8035968, 309667335);
            break;
        case Bls12381MapFp2ToG2:
            params[val] = ContractCostParamEntry(ExtensionPoint{0}, 2420202, 0);
            break;
        case Bls12381HashToG2:
            params[val] =
                ContractCostParamEntry(ExtensionPoint{0}, 7050564, 6797);
            break;
        case Bls12381Pairing:
            params[val] =
                ContractCostParamEntry(ExtensionPoint{0}, 10558948, 632860943);
            break;
        case Bls12381FrFromU256:
            params[val] = ContractCostParamEntry(ExtensionPoint{0}, 1994, 0);
            break;
        case Bls12381FrToU256:
            params[val] = ContractCostParamEntry(ExtensionPoint{0}, 1155, 0);
            break;
        case Bls12381FrAddSub:
            params[val] = ContractCostParamEntry(ExtensionPoint{0}, 74, 0);
            break;
        case Bls12381FrMul:
            params[val] = ContractCostParamEntry(ExtensionPoint{0}, 332, 0);
            break;
        case Bls12381FrPow:
            params[val] = ContractCostParamEntry(ExtensionPoint{0}, 691, 74558);
            break;
        case Bls12381FrInv:
            params[val] = ContractCostParamEntry(ExtensionPoint{0}, 35421, 0);
            break;
        default:
            break;
        }
    }
    ltx.commit();
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
    entry.stateArchivalSettings().liveSorobanStateSizeWindowSampleSize =
        InitialSorobanNetworkConfig::BUCKET_LIST_SIZE_WINDOW_SAMPLE_SIZE;
    entry.stateArchivalSettings().liveSorobanStateSizeWindowSamplePeriod =
        InitialSorobanNetworkConfig::BUCKET_LIST_WINDOW_SAMPLE_PERIOD;

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
initialMemCostParamsEntryForV20()
{
    ConfigSettingEntry entry(CONFIG_SETTING_CONTRACT_COST_PARAMS_MEMORY_BYTES);

    auto& params = entry.contractCostParamsMemBytes();
    auto max_index = static_cast<uint32>(ContractCostType::ChaCha20DrawBytes);
    params.resize(max_index + 1);

    for (size_t val = 0; val <= max_index; ++val)
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
        case DecodeEcdsaCurve256Sig:
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
        default:
            break;
        }
    }
    return entry;
}

void
updateMemCostParamsEntryForV21(AbstractLedgerTxn& ltxRoot)
{
    LedgerTxn ltx(ltxRoot);

    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_COST_PARAMS_MEMORY_BYTES;

    auto& params = ltx.load(key)
                       .current()
                       .data.configSetting()
                       .contractCostParamsMemBytes();

    // Resize to fit the last cost type added in v21
    params.resize(
        static_cast<uint32>(ContractCostType::VerifyEcdsaSecp256r1Sig) + 1);

    auto const& vals = xdr::xdr_traits<ContractCostType>::enum_values();

    for (auto val : vals)
    {
        switch (val)
        {
        // VmCachedInstantiation is the only one we're updating. The rest are
        // new.
        case VmCachedInstantiation:
            params[val] =
                ContractCostParamEntry{ExtensionPoint{0}, 69472, 1217};
            break;
        case ParseWasmInstructions:
            params[val] =
                ContractCostParamEntry{ExtensionPoint{0}, 17564, 6457};
            break;
        case ParseWasmFunctions:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 47464};
            break;
        case ParseWasmGlobals:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 13420};
            break;
        case ParseWasmTableEntries:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 6285};
            break;
        case ParseWasmTypes:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 64670};
            break;
        case ParseWasmDataSegments:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 29074};
            break;
        case ParseWasmElemSegments:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 48095};
            break;
        case ParseWasmImports:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 103229};
            break;
        case ParseWasmExports:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 36394};
            break;
        case ParseWasmDataSegmentBytes:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 257};
            break;
        case InstantiateWasmInstructions:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 70704, 0};
            break;
        case InstantiateWasmFunctions:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 14613};
            break;
        case InstantiateWasmGlobals:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 6833};
            break;
        case InstantiateWasmTableEntries:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 1025};
            break;
        case InstantiateWasmTypes:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        case InstantiateWasmDataSegments:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 129632};
            break;
        case InstantiateWasmElemSegments:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 13665};
            break;
        case InstantiateWasmImports:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 97637};
            break;
        case InstantiateWasmExports:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 9176};
            break;
        case InstantiateWasmDataSegmentBytes:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 126};
            break;
        case Sec1DecodePointUncompressed:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        case VerifyEcdsaSecp256r1Sig:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        default:
            break;
        }
    }

    ltx.commit();
}

void
updateMemCostParamsEntryForV22(AbstractLedgerTxn& ltxRoot)
{
    LedgerTxn ltx(ltxRoot);

    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_COST_PARAMS_MEMORY_BYTES;

    auto& params = ltx.load(key)
                       .current()
                       .data.configSetting()
                       .contractCostParamsMemBytes();

    auto const& vals = xdr::xdr_traits<ContractCostType>::enum_values();

    // Resize to fit the last cost type added in v22
    params.resize(static_cast<uint32>(ContractCostType::Bls12381FrInv) + 1);

    for (auto val : vals)
    {
        switch (val)
        {
        // adding new cost types introduced in p22
        case Bls12381EncodeFp:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        case Bls12381DecodeFp:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        case Bls12381G1CheckPointOnCurve:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        case Bls12381G1CheckPointInSubgroup:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        case Bls12381G2CheckPointOnCurve:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        case Bls12381G2CheckPointInSubgroup:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        case Bls12381G1ProjectiveToAffine:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        case Bls12381G2ProjectiveToAffine:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        case Bls12381G1Add:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        case Bls12381G1Mul:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        case Bls12381G1Msm:
            params[val] =
                ContractCostParamEntry{ExtensionPoint{0}, 109494, 354667};
            break;
        case Bls12381MapFpToG1:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 5552, 0};
            break;
        case Bls12381HashToG1:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 9424, 0};
            break;
        case Bls12381G2Add:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        case Bls12381G2Mul:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        case Bls12381G2Msm:
            params[val] =
                ContractCostParamEntry{ExtensionPoint{0}, 219654, 354667};
            break;
        case Bls12381MapFp2ToG2:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 3344, 0};
            break;
        case Bls12381HashToG2:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 6816, 0};
            break;
        case Bls12381Pairing:
            params[val] =
                ContractCostParamEntry{ExtensionPoint{0}, 2204, 9340474};
            break;
        case Bls12381FrFromU256:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        case Bls12381FrToU256:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 248, 0};
            break;
        case Bls12381FrAddSub:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        case Bls12381FrMul:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;
        case Bls12381FrPow:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 128};
            break;
        case Bls12381FrInv:
            params[val] = ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
            break;

        default:
            break;
        }
    }

    ltx.commit();
}

ConfigSettingEntry
initialParallelComputeEntry()
{
    ConfigSettingEntry entry(CONFIG_SETTING_CONTRACT_PARALLEL_COMPUTE_V0);
    entry.contractParallelCompute().ledgerMaxDependentTxClusters =
        InitialSorobanNetworkConfig::LEDGER_MAX_DEPENDENT_TX_CLUSTERS;
    return entry;
}

ConfigSettingEntry
initialLedgerCostExtEntry(uint32_t txMaxDiskReadEntries)
{
    ConfigSettingEntry entry(CONFIG_SETTING_CONTRACT_LEDGER_COST_EXT_V0);
    // Initialize `txMaxFootprintEntries` with the value of the older
    // `txMaxDiskReadEntries` setting that used to apply to the whole
    // transaction footprint, so that we ensure that no transactions
    // will become invalid due to this limit.
    entry.contractLedgerCostExt().txMaxFootprintEntries = txMaxDiskReadEntries;
    entry.contractLedgerCostExt().feeWrite1KB =
        InitialSorobanNetworkConfig::FEE_LEDGER_WRITE_1KB;
    return entry;
}

ConfigSettingEntry
initialScpTimingEntry()
{
    ConfigSettingEntry entry(CONFIG_SETTING_SCP_TIMING);
    entry.contractSCPTiming().ledgerTargetCloseTimeMilliseconds =
        InitialSorobanNetworkConfig::LEDGER_TARGET_CLOSE_TIME_MILLISECONDS;
    entry.contractSCPTiming().nominationTimeoutInitialMilliseconds =
        InitialSorobanNetworkConfig::NOMINATION_TIMEOUT_INITIAL_MILLISECONDS;
    entry.contractSCPTiming().nominationTimeoutIncrementMilliseconds =
        InitialSorobanNetworkConfig::NOMINATION_TIMEOUT_INCREMENT_MILLISECONDS;
    entry.contractSCPTiming().ballotTimeoutInitialMilliseconds =
        InitialSorobanNetworkConfig::BALLOT_TIMEOUT_INITIAL_MILLISECONDS;
    entry.contractSCPTiming().ballotTimeoutIncrementMilliseconds =
        InitialSorobanNetworkConfig::BALLOT_TIMEOUT_INCREMENT_MILLISECONDS;

    return entry;
}

ConfigSettingEntry
initialliveSorobanStateSizeWindow(Application& app)
{
    ConfigSettingEntry entry(CONFIG_SETTING_LIVE_SOROBAN_STATE_SIZE_WINDOW);

    // Populate 30 day sliding window of BucketList size snapshots with 30
    // copies of the current BL size.
    auto blSize = app.getBucketManager().getLiveBucketList().getSize();
    for (uint64_t i = 0;
         i < InitialSorobanNetworkConfig::BUCKET_LIST_SIZE_WINDOW_SAMPLE_SIZE;
         ++i)
    {
        entry.liveSorobanStateSizeWindow().push_back(blSize);
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

void
updateRentCostParamsForV23(AbstractLedgerTxn& ltxRoot)
{
    LedgerTxn ltx(ltxRoot);

    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_LEDGER_COST_V0;

    auto& ledgerCostSettings =
        ltx.load(key).current().data.configSetting().contractLedgerCost();
    ledgerCostSettings.sorobanStateTargetSizeBytes =
        Protcol23UpgradedConfig::SOROBAN_STATE_TARGET_SIZE_BYTES;
    ledgerCostSettings.rentFee1KBSorobanStateSizeLow =
        Protcol23UpgradedConfig::RENT_FEE_1KB_SOROBAN_STATE_SIZE_LOW;
    ledgerCostSettings.rentFee1KBSorobanStateSizeHigh =
        Protcol23UpgradedConfig::RENT_FEE_1KB_SOROBAN_STATE_SIZE_HIGH;

    LedgerKey archivalKey(CONFIG_SETTING);
    archivalKey.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_STATE_ARCHIVAL;
    auto& archivalSettings = ltx.load(archivalKey)
                                 .current()
                                 .data.configSetting()
                                 .stateArchivalSettings();
    archivalSettings.persistentRentRateDenominator =
        Protcol23UpgradedConfig::PERSISTENT_RENT_RATE_DENOMINATOR;
    archivalSettings.tempRentRateDenominator =
        Protcol23UpgradedConfig::TEMP_RENT_RATE_DENOMINATOR;
    ltx.commit();
}
} // namespace

bool
SorobanNetworkConfig::isValidConfigSettingEntry(ConfigSettingEntry const& cfg,
                                                uint32_t ledgerVersion)
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
            cfg.contractCostParamsCpuInsns(), ledgerVersion);
        break;
    case ConfigSettingID::CONFIG_SETTING_CONTRACT_COST_PARAMS_MEMORY_BYTES:
        valid = SorobanNetworkConfig::isValidCostParams(
            cfg.contractCostParamsMemBytes(), ledgerVersion);
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
        valid = cfg.contractLedgerCost().txMaxDiskReadEntries >=
                    MinimumSorobanNetworkConfig::TX_MAX_READ_LEDGER_ENTRIES &&
                cfg.contractLedgerCost().ledgerMaxDiskReadEntries >=
                    cfg.contractLedgerCost().txMaxDiskReadEntries &&
                cfg.contractLedgerCost().txMaxDiskReadBytes >=
                    MinimumSorobanNetworkConfig::TX_MAX_READ_BYTES &&
                cfg.contractLedgerCost().ledgerMaxDiskReadBytes >=
                    cfg.contractLedgerCost().txMaxDiskReadBytes &&
                cfg.contractLedgerCost().txMaxWriteLedgerEntries >=
                    MinimumSorobanNetworkConfig::TX_MAX_WRITE_LEDGER_ENTRIES &&
                cfg.contractLedgerCost().ledgerMaxWriteLedgerEntries >=
                    cfg.contractLedgerCost().txMaxWriteLedgerEntries &&
                cfg.contractLedgerCost().txMaxWriteBytes >=
                    MinimumSorobanNetworkConfig::TX_MAX_WRITE_BYTES &&
                cfg.contractLedgerCost().ledgerMaxWriteBytes >=
                    cfg.contractLedgerCost().txMaxWriteBytes &&
                cfg.contractLedgerCost().feeDiskReadLedgerEntry >= 0 &&
                cfg.contractLedgerCost().feeWriteLedgerEntry >= 0 &&
                cfg.contractLedgerCost().feeDiskRead1KB >= 0 &&
                cfg.contractLedgerCost().sorobanStateTargetSizeBytes > 0 &&
                cfg.contractLedgerCost().rentFee1KBSorobanStateSizeHigh >= 0 &&
                cfg.contractLedgerCost().sorobanStateRentFeeGrowthFactor >= 0;
        break;
    case ConfigSettingID::CONFIG_SETTING_CONTRACT_EVENTS_V0:
        valid = cfg.contractEvents().txMaxContractEventsSizeBytes >=
                    MinimumSorobanNetworkConfig::
                        TX_MAX_CONTRACT_EVENTS_SIZE_BYTES &&
                cfg.contractEvents().feeContractEvents1KB >= 0;
        break;
    case ConfigSettingID::CONFIG_SETTING_STATE_ARCHIVAL:
        valid =
            cfg.stateArchivalSettings().maxEntryTTL >=
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
            cfg.stateArchivalSettings().liveSorobanStateSizeWindowSampleSize >=
                MinimumSorobanNetworkConfig::
                    BUCKETLIST_SIZE_WINDOW_SAMPLE_SIZE &&
            cfg.stateArchivalSettings().evictionScanSize >=
                MinimumSorobanNetworkConfig::EVICTION_SCAN_SIZE &&
            cfg.stateArchivalSettings().startingEvictionScanLevel >=
                MinimumSorobanNetworkConfig::STARTING_EVICTION_LEVEL &&
            cfg.stateArchivalSettings().startingEvictionScanLevel <
                LiveBucketList::kNumLevels &&
            cfg.stateArchivalSettings()
                    .liveSorobanStateSizeWindowSamplePeriod >=
                MinimumSorobanNetworkConfig::BUCKETLIST_WINDOW_SAMPLE_PERIOD;

        valid = valid && cfg.stateArchivalSettings().maxEntryTTL >
                             cfg.stateArchivalSettings().minPersistentTTL;
        valid = valid && cfg.stateArchivalSettings().maxEntryTTL >
                             cfg.stateArchivalSettings().minTemporaryTTL;
        break;
    case ConfigSettingID::CONFIG_SETTING_LIVE_SOROBAN_STATE_SIZE_WINDOW:
    case ConfigSettingID::CONFIG_SETTING_EVICTION_ITERATOR:
        valid = true;
        break;
    case ConfigSettingID::CONFIG_SETTING_CONTRACT_PARALLEL_COMPUTE_V0:
        valid =
            protocolVersionStartsFrom(
                ledgerVersion, PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION) &&
            cfg.contractParallelCompute().ledgerMaxDependentTxClusters > 0 &&
            cfg.contractParallelCompute().ledgerMaxDependentTxClusters <
                MAX_LEDGER_DEPENDENT_TX_CLUSTERS;
        break;
    case ConfigSettingID::CONFIG_SETTING_CONTRACT_LEDGER_COST_EXT_V0:
        valid =
            protocolVersionStartsFrom(ledgerVersion, ProtocolVersion::V_23) &&
            cfg.contractLedgerCostExt().txMaxFootprintEntries >=
                MinimumSorobanNetworkConfig::TX_MAX_READ_LEDGER_ENTRIES &&
            cfg.contractLedgerCostExt().feeWrite1KB >= 0;
        break;
    case ConfigSettingID::CONFIG_SETTING_SCP_TIMING:
        valid =
            protocolVersionStartsFrom(ledgerVersion, ProtocolVersion::V_23) &&
            cfg.contractSCPTiming().ledgerTargetCloseTimeMilliseconds >=
                MinimumSorobanNetworkConfig::
                    LEDGER_TARGET_CLOSE_TIME_MILLISECONDS &&
            cfg.contractSCPTiming().ledgerTargetCloseTimeMilliseconds <=
                MaximumSorobanNetworkConfig::
                    LEDGER_TARGET_CLOSE_TIME_MILLISECONDS &&
            cfg.contractSCPTiming().nominationTimeoutInitialMilliseconds >=
                MinimumSorobanNetworkConfig::
                    NOMINATION_TIMEOUT_INITIAL_MILLISECONDS &&
            cfg.contractSCPTiming().nominationTimeoutInitialMilliseconds <=
                MaximumSorobanNetworkConfig::
                    NOMINATION_TIMEOUT_INITIAL_MILLISECONDS &&
            cfg.contractSCPTiming().nominationTimeoutIncrementMilliseconds >=
                MinimumSorobanNetworkConfig::
                    NOMINATION_TIMEOUT_INCREMENT_MILLISECONDS &&
            cfg.contractSCPTiming().nominationTimeoutIncrementMilliseconds <=
                MaximumSorobanNetworkConfig::
                    NOMINATION_TIMEOUT_INCREMENT_MILLISECONDS &&
            cfg.contractSCPTiming().ballotTimeoutInitialMilliseconds >=
                MinimumSorobanNetworkConfig::
                    BALLOT_TIMEOUT_INITIAL_MILLISECONDS &&
            cfg.contractSCPTiming().ballotTimeoutInitialMilliseconds <=
                MaximumSorobanNetworkConfig::
                    BALLOT_TIMEOUT_INITIAL_MILLISECONDS &&
            cfg.contractSCPTiming().ballotTimeoutIncrementMilliseconds >=
                MinimumSorobanNetworkConfig::
                    BALLOT_TIMEOUT_INCREMENT_MILLISECONDS &&
            cfg.contractSCPTiming().ballotTimeoutIncrementMilliseconds <=
                MaximumSorobanNetworkConfig::
                    BALLOT_TIMEOUT_INCREMENT_MILLISECONDS;
        break;
    default:
        break;
    }
    return valid;
}

bool
SorobanNetworkConfig::isNonUpgradeableConfigSettingEntry(
    ConfigSettingEntry const& cfg)
{
    return isNonUpgradeableConfigSettingEntry(cfg.configSettingID());
}

bool
SorobanNetworkConfig::isNonUpgradeableConfigSettingEntry(
    ConfigSettingID const& cfg)
{
    // While the BucketList size window and eviction iterator are stored in a
    // ConfigSetting entry, the BucketList defines these values, they should
    // never be changed via upgrade
    return cfg ==
               ConfigSettingID::CONFIG_SETTING_LIVE_SOROBAN_STATE_SIZE_WINDOW ||
           cfg == ConfigSettingID::CONFIG_SETTING_EVICTION_ITERATOR;
}

void
SorobanNetworkConfig::createLedgerEntriesForV20(AbstractLedgerTxn& ltx,
                                                Application& app)
{
    ZoneScoped;

    // The validation needs to be pinned to 20 and not ltx's ledgerVersion
    // because a protocol bump from less than 19 to after 20 will result in a
    // failed check because we expect most cost types in v21. Those new cost
    // types will be created in this case, but that'll happen later in
    // createCostTypesForV21.
    static const uint32_t versionToValidateAgainst = 20;

    auto const& cfg = app.getConfig();
    createConfigSettingEntry(initialMaxContractSizeEntry(cfg), ltx,
                             versionToValidateAgainst);
    createConfigSettingEntry(initialMaxContractDataKeySizeEntry(cfg), ltx,
                             versionToValidateAgainst);
    createConfigSettingEntry(initialMaxContractDataEntrySizeEntry(cfg), ltx,
                             versionToValidateAgainst);
    createConfigSettingEntry(initialContractComputeSettingsEntry(cfg), ltx,
                             versionToValidateAgainst);
    createConfigSettingEntry(initialContractLedgerAccessSettingsEntry(cfg), ltx,
                             versionToValidateAgainst);
    createConfigSettingEntry(initialContractHistoricalDataSettingsEntry(), ltx,
                             versionToValidateAgainst);
    createConfigSettingEntry(initialContractEventsSettingsEntry(cfg), ltx,
                             versionToValidateAgainst);
    createConfigSettingEntry(initialContractBandwidthSettingsEntry(cfg), ltx,
                             versionToValidateAgainst);
    createConfigSettingEntry(initialContractExecutionLanesSettingsEntry(cfg),
                             ltx, versionToValidateAgainst);
    createConfigSettingEntry(initialCpuCostParamsEntryForV20(), ltx,
                             versionToValidateAgainst);
    createConfigSettingEntry(initialMemCostParamsEntryForV20(), ltx,
                             versionToValidateAgainst);
    createConfigSettingEntry(initialStateArchivalSettings(cfg), ltx,
                             versionToValidateAgainst);
    createConfigSettingEntry(initialliveSorobanStateSizeWindow(app), ltx,
                             versionToValidateAgainst);
    createConfigSettingEntry(initialEvictionIterator(cfg), ltx,
                             versionToValidateAgainst);
}

void
SorobanNetworkConfig::createCostTypesForV21(AbstractLedgerTxn& ltx,
                                            Application& app)
{
    ZoneScoped;
    updateCpuCostParamsEntryForV21(ltx);
    updateMemCostParamsEntryForV21(ltx);
}

void
SorobanNetworkConfig::createCostTypesForV22(AbstractLedgerTxn& ltx,
                                            Application& app)
{
    ZoneScoped;
    updateCpuCostParamsEntryForV22(ltx);
    updateMemCostParamsEntryForV22(ltx);
}

void
SorobanNetworkConfig::createAndUpdateLedgerEntriesForV23(AbstractLedgerTxn& ltx,
                                                         Application& app)
{
    ZoneScoped;
    createConfigSettingEntry(initialParallelComputeEntry(), ltx,
                             static_cast<uint32_t>(ProtocolVersion::V_23));
    createConfigSettingEntry(initialScpTimingEntry(), ltx,
                             static_cast<uint32_t>(ProtocolVersion::V_23));

    // We expect CONFIG_SETTING_CONTRACT_LEDGER_COST_V0 to exist when upgrading
    // to protocol 23 as it must be created during the protocol 20 upgrade.
    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_LEDGER_COST_V0;
    auto le = ltx.loadWithoutRecord(key).current();
    auto const& ledgerCostSetting =
        le.data.configSetting().contractLedgerCost();
    createConfigSettingEntry(
        initialLedgerCostExtEntry(ledgerCostSetting.txMaxDiskReadEntries), ltx,
        static_cast<uint32_t>(ProtocolVersion::V_23));

    updateRentCostParamsForV23(ltx);
}

void
SorobanNetworkConfig::initializeGenesisLedgerForTesting(
    uint32_t genesisLedgerProtocol, AbstractLedgerTxn& ltx, Application& app)
{
    if (protocolVersionStartsFrom(genesisLedgerProtocol,
                                  SOROBAN_PROTOCOL_VERSION))
    {
        // This sandwich is required because createLedgerEntriesForV20 (due to
        // the validation in isValidCostParams) is only valid in v20 because the
        // size of the cost types in the LedgerEntry matters.
        ltx.loadHeader().current().ledgerVersion =
            static_cast<uint32_t>(SOROBAN_PROTOCOL_VERSION);
        SorobanNetworkConfig::createLedgerEntriesForV20(ltx, app);
        // Protocol 20 released with somewhat incorrect costs and has been
        // re-calibrated short after the release. We catch up here to the more
        // correct costs that exist on the network.
#ifdef BUILD_TESTS
        updateRecalibratedCostTypesForV20(ltx);
#endif
        ltx.loadHeader().current().ledgerVersion = genesisLedgerProtocol;
    }

    if (protocolVersionStartsFrom(genesisLedgerProtocol, ProtocolVersion::V_21))
    {
        SorobanNetworkConfig::createCostTypesForV21(ltx, app);
    }

    if (protocolVersionStartsFrom(genesisLedgerProtocol, ProtocolVersion::V_22))
    {
        SorobanNetworkConfig::createCostTypesForV22(ltx, app);
    }
    if (protocolVersionStartsFrom(genesisLedgerProtocol, ProtocolVersion::V_23))
    {
        SorobanNetworkConfig::createAndUpdateLedgerEntriesForV23(ltx, app);
    }
}

void
SorobanNetworkConfig::loadFromLedger(LedgerTxnReadOnly const& roLtx)
{
    ZoneScoped;
    loadMaxContractSize(roLtx);
    loadMaxContractDataKeySize(roLtx);
    loadMaxContractDataEntrySize(roLtx);
    loadComputeSettings(roLtx);
    loadLedgerAccessSettings(roLtx);
    loadHistoricalSettings(roLtx);
    loadContractEventsSettings(roLtx);
    loadBandwidthSettings(roLtx);
    loadCpuCostParams(roLtx);
    loadMemCostParams(roLtx);
    loadStateArchivalSettings(roLtx);
    loadExecutionLanesSettings(roLtx);
    loadliveSorobanStateSizeWindow(roLtx);
    loadEvictionIterator(roLtx);

    auto protocolVersion = roLtx.getLedgerHeader().current().ledgerVersion;
    if (protocolVersionStartsFrom(protocolVersion, ProtocolVersion::V_23))
    {
        loadParallelComputeConfig(roLtx);
        loadLedgerCostExtConfig(roLtx);
        loadSCPTimingConfig(roLtx);
    }
    // NB: this should follow loading/updating state size window
    // size and state archival settings
    computeRentWriteFee(protocolVersion);
}

void
SorobanNetworkConfig::loadMaxContractSize(LedgerTxnReadOnly const& roLtx)
{
    ZoneScoped;

    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_MAX_SIZE_BYTES;
    auto le = roLtx.load(key).current();
    mMaxContractSizeBytes = le.data.configSetting().contractMaxSizeBytes();
}

void
SorobanNetworkConfig::loadMaxContractDataKeySize(LedgerTxnReadOnly const& roLtx)
{
    ZoneScoped;

    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_DATA_KEY_SIZE_BYTES;
    auto le = roLtx.load(key).current();
    mMaxContractDataKeySizeBytes =
        le.data.configSetting().contractDataKeySizeBytes();
}

void
SorobanNetworkConfig::loadMaxContractDataEntrySize(
    LedgerTxnReadOnly const& roLtx)
{
    ZoneScoped;

    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_DATA_ENTRY_SIZE_BYTES;
    auto le = roLtx.load(key).current();
    mMaxContractDataEntrySizeBytes =
        le.data.configSetting().contractDataEntrySizeBytes();
}

void
SorobanNetworkConfig::loadComputeSettings(LedgerTxnReadOnly const& roLtx)
{
    ZoneScoped;

    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_COMPUTE_V0;
    auto le = roLtx.load(key).current();
    auto const& configSetting = le.data.configSetting().contractCompute();
    mLedgerMaxInstructions = configSetting.ledgerMaxInstructions;
    mTxMaxInstructions = configSetting.txMaxInstructions;
    mFeeRatePerInstructionsIncrement =
        configSetting.feeRatePerInstructionsIncrement;
    mTxMemoryLimit = configSetting.txMemoryLimit;
}

void
SorobanNetworkConfig::loadLedgerAccessSettings(LedgerTxnReadOnly const& roLtx)
{
    ZoneScoped;

    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_LEDGER_COST_V0;
    auto le = roLtx.load(key).current();
    auto const& configSetting = le.data.configSetting().contractLedgerCost();
    mLedgerMaxDiskReadEntries = configSetting.ledgerMaxDiskReadEntries;
    mLedgerMaxDiskReadBytes = configSetting.ledgerMaxDiskReadBytes;
    mLedgerMaxWriteLedgerEntries = configSetting.ledgerMaxWriteLedgerEntries;
    mLedgerMaxWriteBytes = configSetting.ledgerMaxWriteBytes;
    mTxMaxDiskReadEntries = configSetting.txMaxDiskReadEntries;
    mTxMaxDiskReadBytes = configSetting.txMaxDiskReadBytes;
    mTxMaxWriteLedgerEntries = configSetting.txMaxWriteLedgerEntries;
    mTxMaxWriteBytes = configSetting.txMaxWriteBytes;
    mFeeDiskReadLedgerEntry = configSetting.feeDiskReadLedgerEntry;
    mFeeWriteLedgerEntry = configSetting.feeWriteLedgerEntry;
    mFeeDiskRead1KB = configSetting.feeDiskRead1KB;
    mSorobanStateTargetSizeBytes = configSetting.sorobanStateTargetSizeBytes;
    mRentFee1KBSorobanStateSizeLow =
        configSetting.rentFee1KBSorobanStateSizeLow;
    mRentFee1KBSorobanStateSizeHigh =
        configSetting.rentFee1KBSorobanStateSizeHigh;
    mSorobanStateRentFeeGrowthFactor =
        configSetting.sorobanStateRentFeeGrowthFactor;

    if (protocolVersionStartsFrom(
            roLtx.getLedgerHeader().current().ledgerVersion,
            ProtocolVersion::V_23))
    {
        LedgerKey key(CONFIG_SETTING);
        key.configSetting().configSettingID =
            ConfigSettingID::CONFIG_SETTING_CONTRACT_LEDGER_COST_EXT_V0;
        auto le = roLtx.load(key).current();
        auto const& configSetting =
            le.data.configSetting().contractLedgerCostExt();
        mTxMaxFootprintEntries = configSetting.txMaxFootprintEntries;
        mFeeFlatRateWrite1KB = configSetting.feeWrite1KB;
    }
}

void
SorobanNetworkConfig::loadHistoricalSettings(LedgerTxnReadOnly const& roLtx)
{
    ZoneScoped;

    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_HISTORICAL_DATA_V0;
    auto le = roLtx.load(key).current();
    auto const& configSetting =
        le.data.configSetting().contractHistoricalData();
    mFeeHistorical1KB = configSetting.feeHistorical1KB;
}

void
SorobanNetworkConfig::loadContractEventsSettings(LedgerTxnReadOnly const& roLtx)
{
    ZoneScoped;

    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_EVENTS_V0;
    auto le = roLtx.load(key).current();
    auto const& configSetting = le.data.configSetting().contractEvents();
    mFeeContractEvents1KB = configSetting.feeContractEvents1KB;
    mTxMaxContractEventsSizeBytes = configSetting.txMaxContractEventsSizeBytes;
}

void
SorobanNetworkConfig::loadBandwidthSettings(LedgerTxnReadOnly const& roLtx)
{
    ZoneScoped;

    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_BANDWIDTH_V0;
    auto le = roLtx.load(key).current();
    auto const& configSetting = le.data.configSetting().contractBandwidth();
    mLedgerMaxTransactionsSizeBytes = configSetting.ledgerMaxTxsSizeBytes;
    mTxMaxSizeBytes = configSetting.txMaxSizeBytes;
    mFeeTransactionSize1KB = configSetting.feeTxSize1KB;
}

void
SorobanNetworkConfig::loadCpuCostParams(LedgerTxnReadOnly const& roLtx)
{
    ZoneScoped;

    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_COST_PARAMS_CPU_INSTRUCTIONS;
    auto le = roLtx.load(key).current();
    mCpuCostParams = le.data.configSetting().contractCostParamsCpuInsns();
}

void
SorobanNetworkConfig::loadMemCostParams(LedgerTxnReadOnly const& roLtx)
{
    ZoneScoped;

    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_COST_PARAMS_MEMORY_BYTES;
    auto le = roLtx.load(key).current();
    mMemCostParams = le.data.configSetting().contractCostParamsMemBytes();
}

void
SorobanNetworkConfig::loadExecutionLanesSettings(LedgerTxnReadOnly const& roLtx)
{
    ZoneScoped;

    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_EXECUTION_LANES;
    auto le = roLtx.load(key).current();
    auto const& configSetting =
        le.data.configSetting().contractExecutionLanes();
    mLedgerMaxTxCount = configSetting.ledgerMaxTxCount;
}

void
SorobanNetworkConfig::loadliveSorobanStateSizeWindow(
    LedgerTxnReadOnly const& roLtx)
{
    ZoneScoped;

    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_LIVE_SOROBAN_STATE_SIZE_WINDOW;
    auto txle = roLtx.load(key);
    releaseAssert(txle);
    auto const& leVector =
        txle.current().data.configSetting().liveSorobanStateSizeWindow();
    mSorobanStateSizeSnapshots.clear();
    for (auto e : leVector)
    {
        mSorobanStateSizeSnapshots.push_back(e);
    }
    updateSorobanStateSizeAverage();
}

void
SorobanNetworkConfig::loadEvictionIterator(LedgerTxnReadOnly const& roLtx)
{
    ZoneScoped;

    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_EVICTION_ITERATOR;
    auto txle = roLtx.load(key);
    releaseAssert(txle);
    mEvictionIterator = txle.current().data.configSetting().evictionIterator();
}

void
SorobanNetworkConfig::loadParallelComputeConfig(LedgerTxnReadOnly const& roLtx)
{
    ZoneScoped;
    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_PARALLEL_COMPUTE_V0;
    auto le = roLtx.load(key).current();
    auto const& configSetting =
        le.data.configSetting().contractParallelCompute();
    mLedgerMaxDependentTxClusters = configSetting.ledgerMaxDependentTxClusters;
}

void
SorobanNetworkConfig::loadLedgerCostExtConfig(LedgerTxnReadOnly const& roLtx)
{
    ZoneScoped;
    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_LEDGER_COST_EXT_V0;
    auto le = roLtx.load(key).current();
    auto const& configSetting = le.data.configSetting().contractLedgerCostExt();
    mTxMaxFootprintEntries = configSetting.txMaxFootprintEntries;
    mFeeFlatRateWrite1KB = configSetting.feeWrite1KB;
}

void
SorobanNetworkConfig::loadSCPTimingConfig(LedgerTxnReadOnly const& roLtx)
{
    ZoneScoped;
    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_SCP_TIMING;
    auto le = roLtx.load(key).current();
    auto const& configSetting = le.data.configSetting().contractSCPTiming();
    mLedgerTargetCloseTimeMilliseconds =
        configSetting.ledgerTargetCloseTimeMilliseconds;
    mNominationTimeoutInitialMilliseconds =
        configSetting.nominationTimeoutInitialMilliseconds;
    mNominationTimeoutIncrementMilliseconds =
        configSetting.nominationTimeoutIncrementMilliseconds;
    mBallotTimeoutInitialMilliseconds =
        configSetting.ballotTimeoutInitialMilliseconds;
    mBallotTimeoutIncrementMilliseconds =
        configSetting.ballotTimeoutIncrementMilliseconds;
}

void
SorobanNetworkConfig::writeLiveSorobanStateSizeWindow(
    AbstractLedgerTxn& ltxRoot) const
{
    ZoneScoped;

    // Check that the window is loaded and the number of snapshots is correct
    releaseAssert(mSorobanStateSizeSnapshots.size() ==
                  mStateArchivalSettings.liveSorobanStateSizeWindowSampleSize);

    // Load outdated snapshot entry from DB
    LedgerTxn ltx(ltxRoot);
    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_LIVE_SOROBAN_STATE_SIZE_WINDOW;
    auto txle = ltx.load(key);
    releaseAssert(txle);

    // Copy in-memory snapshots to ledger entry
    auto& leVector =
        txle.current().data.configSetting().liveSorobanStateSizeWindow();
    leVector.clear();
    for (auto e : mSorobanStateSizeSnapshots)
    {
        leVector.push_back(e);
    }

    ltx.commit();
}

void
SorobanNetworkConfig::updateSorobanStateSizeAverage()
{
    ZoneScoped;

    releaseAssert(!mSorobanStateSizeSnapshots.empty());
    uint64_t sizeSum = 0;
    for (uint64_t size : mSorobanStateSizeSnapshots)
    {
        // This is just a sanity check, as both the number of the snapshots
        // and the value of every snapshotted size are rather small.
        if (sizeSum >= std::numeric_limits<uint64_t>::max() - size)
        {
            sizeSum = std::numeric_limits<uint64_t>::max();
        }
        else
        {
            sizeSum += size;
        }
    }

    mAverageSorobanStateSize = sizeSum / mSorobanStateSizeSnapshots.size();
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
SorobanNetworkConfig::loadStateArchivalSettings(LedgerTxnReadOnly const& roLtx)
{
    ZoneScoped;

    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_STATE_ARCHIVAL;
    auto le = roLtx.load(key).current();
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
SorobanNetworkConfig::ledgerMaxDiskReadEntries() const
{
    return mLedgerMaxDiskReadEntries;
}

uint32_t
SorobanNetworkConfig::ledgerMaxDiskReadBytes() const
{
    return mLedgerMaxDiskReadBytes;
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
SorobanNetworkConfig::txMaxDiskReadEntries() const
{
    return mTxMaxDiskReadEntries;
}

uint32_t
SorobanNetworkConfig::txMaxDiskReadBytes() const
{
    return mTxMaxDiskReadBytes;
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
SorobanNetworkConfig::feeDiskReadLedgerEntry() const
{
    return mFeeDiskReadLedgerEntry;
}

int64_t
SorobanNetworkConfig::feeWriteLedgerEntry() const
{
    return mFeeWriteLedgerEntry;
}

int64_t
SorobanNetworkConfig::feeDiskRead1KB() const
{
    return mFeeDiskRead1KB;
}

int64_t
SorobanNetworkConfig::feeRent1KB() const
{
    return mFeeRent1KB;
}

int64_t
SorobanNetworkConfig::sorobanStateTargetSizeBytes() const
{
    return mSorobanStateTargetSizeBytes;
}

int64_t
SorobanNetworkConfig::rentFee1KBSorobanStateSizeLow() const
{
    return mRentFee1KBSorobanStateSizeLow;
}
int64_t
SorobanNetworkConfig::rentFee1KBSorobanStateSizeHigh() const
{
    return mRentFee1KBSorobanStateSizeHigh;
}
uint32_t
SorobanNetworkConfig::sorobanStateRentFeeGrowthFactor() const
{
    return mSorobanStateRentFeeGrowthFactor;
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

void
SorobanNetworkConfig::maybeUpdateSorobanStateSizeWindowSize(
    AbstractLedgerTxn& ltx)
{
    ZoneScoped;
    // We may only call this past Soroban protocol, because the config setting
    // upgrades are not possible before that.
    releaseAssertOrThrow(protocolVersionStartsFrom(
        ltx.loadHeader().current().ledgerVersion, SOROBAN_PROTOCOL_VERSION));

    auto currSize = mSorobanStateSizeSnapshots.size();
    auto newSize = stateArchivalSettings().liveSorobanStateSizeWindowSampleSize;
    if (newSize == currSize)
    {
        // No size change, nothing to update
        return;
    }

    if (newSize < currSize)
    {
        while (mSorobanStateSizeSnapshots.size() != newSize)
        {
            mSorobanStateSizeSnapshots.pop_front();
        }
    }
    // If newSize > currSize, backfill new slots with oldest value in window
    // such that they are the first to get replaced by new values
    else
    {
        auto oldestSize = mSorobanStateSizeSnapshots.front();
        while (mSorobanStateSizeSnapshots.size() != newSize)
        {
            mSorobanStateSizeSnapshots.push_front(oldestSize);
        }
    }

    updateSorobanStateSizeAverage();
    writeLiveSorobanStateSizeWindow(ltx);
}

void
SorobanNetworkConfig::maybeSnapshotSorobanStateSize(uint32_t currLedger,
                                                    uint64_t inMemoryStateSize,
                                                    AbstractLedgerTxn& ltx,
                                                    Application& app)
{
    ZoneScoped;

    auto ledgerVersion = ltx.loadHeader().current().ledgerVersion;
    // // Check if BucketList size window should exist
    if (protocolVersionIsBefore(ledgerVersion, SOROBAN_PROTOCOL_VERSION))
    {
        return;
    }

    if (currLedger %
            mStateArchivalSettings.liveSorobanStateSizeWindowSamplePeriod !=
        0)
    {
        return;
    }
    uint64_t sorobanStateSize = 0;
    if (protocolVersionIsBefore(ledgerVersion, ProtocolVersion::V_23))
    {
        sorobanStateSize = app.getBucketManager().getLiveBucketList().getSize();
    }
    else
    {
        sorobanStateSize = inMemoryStateSize;
    }

    // Update in memory snapshots
    mSorobanStateSizeSnapshots.pop_front();
    mSorobanStateSizeSnapshots.push_back(sorobanStateSize);

    writeLiveSorobanStateSizeWindow(ltx);
    updateSorobanStateSizeAverage();
    computeRentWriteFee(ledgerVersion);
}

void
SorobanNetworkConfig::updateRecomputedSorobanStateSize(uint64_t newSize,
                                                       AbstractLedgerTxn& ltx)
{
    ZoneScoped;
    for (auto& size : mSorobanStateSizeSnapshots)
    {
        size = newSize;
    }
    writeLiveSorobanStateSizeWindow(ltx);
}

uint64_t
SorobanNetworkConfig::getAverageSorobanStateSize() const
{
    return mAverageSorobanStateSize;
}

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

uint32_t
SorobanNetworkConfig::ledgerMaxDependentTxClusters() const
{
    return mLedgerMaxDependentTxClusters;
}

uint32_t
SorobanNetworkConfig::ledgerTargetCloseTimeMilliseconds() const
{
    return mLedgerTargetCloseTimeMilliseconds;
}

uint32_t
SorobanNetworkConfig::nominationTimeoutInitialMilliseconds() const
{
    return mNominationTimeoutInitialMilliseconds;
}

uint32_t
SorobanNetworkConfig::nominationTimeoutIncrementMilliseconds() const
{
    return mNominationTimeoutIncrementMilliseconds;
}

uint32_t
SorobanNetworkConfig::ballotTimeoutInitialMilliseconds() const
{
    return mBallotTimeoutInitialMilliseconds;
}

uint32_t
SorobanNetworkConfig::ballotTimeoutIncrementMilliseconds() const
{
    return mBallotTimeoutIncrementMilliseconds;
}

uint32_t
SorobanNetworkConfig::txMaxFootprintEntries() const
{
    return mTxMaxFootprintEntries;
}

int64_t
SorobanNetworkConfig::feeFlatRateWrite1KB() const
{
    return mFeeFlatRateWrite1KB;
}

Resource
SorobanNetworkConfig::maxLedgerResources() const
{
    std::vector<int64_t> limits = {ledgerMaxTxCount(),
                                   ledgerMaxInstructions(),
                                   ledgerMaxTransactionSizesBytes(),
                                   ledgerMaxDiskReadBytes(),
                                   ledgerMaxWriteBytes(),
                                   ledgerMaxDiskReadEntries(),
                                   ledgerMaxWriteLedgerEntries()};
    return Resource(limits);
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

void
SorobanNetworkConfig::updateRecalibratedCostTypesForV20(
    AbstractLedgerTxn& ltxRoot)
{
    LedgerTxn ltx(ltxRoot);

    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_COST_PARAMS_CPU_INSTRUCTIONS;

    auto& cpuParams = ltx.load(key)
                          .current()
                          .data.configSetting()
                          .contractCostParamsCpuInsns();

    for (size_t val = 0; val < cpuParams.size(); ++val)
    {
        switch (val)
        {
        case DispatchHostFunction:
            cpuParams[val] = ContractCostParamEntry{ExtensionPoint{0}, 295, 0};
            break;
        case VisitObject:
            cpuParams[val] = ContractCostParamEntry{ExtensionPoint{0}, 60, 0};
            break;
        case ValSer:
            cpuParams[val] = ContractCostParamEntry{ExtensionPoint{0}, 221, 26};
            break;
        case ValDeser:
            cpuParams[val] =
                ContractCostParamEntry{ExtensionPoint{0}, 331, 4369};
            break;
        case ComputeSha256Hash:
            cpuParams[val] =
                ContractCostParamEntry{ExtensionPoint{0}, 3636, 7013};
            break;
        case ComputeEd25519PubKey:
            cpuParams[val] =
                ContractCostParamEntry{ExtensionPoint{0}, 40256, 0};
            break;
        case VerifyEd25519Sig:
            cpuParams[val] =
                ContractCostParamEntry{ExtensionPoint{0}, 377551, 4059};
            break;
        case VmInstantiation:
            cpuParams[val] =
                ContractCostParamEntry{ExtensionPoint{0}, 417482, 45712};
            break;
        case VmCachedInstantiation:
            cpuParams[val] =
                ContractCostParamEntry{ExtensionPoint{0}, 417482, 45712};
            break;
        case InvokeVmFunction:
            cpuParams[val] = ContractCostParamEntry{ExtensionPoint{0}, 1945, 0};
            break;
        case ComputeKeccak256Hash:
            cpuParams[val] =
                ContractCostParamEntry{ExtensionPoint{0}, 6481, 5943};
            break;
        case DecodeEcdsaCurve256Sig:
            cpuParams[val] = ContractCostParamEntry{ExtensionPoint{0}, 711, 0};
            break;
        case RecoverEcdsaSecp256k1Key:
            cpuParams[val] =
                ContractCostParamEntry{ExtensionPoint{0}, 2314804, 0};
            break;
        case Int256AddSub:
            cpuParams[val] = ContractCostParamEntry{ExtensionPoint{0}, 4176, 0};
            break;
        case Int256Mul:
            cpuParams[val] = ContractCostParamEntry{ExtensionPoint{0}, 4716, 0};
            break;
        case Int256Div:
            cpuParams[val] = ContractCostParamEntry{ExtensionPoint{0}, 4680, 0};
            break;
        case Int256Pow:
            cpuParams[val] = ContractCostParamEntry{ExtensionPoint{0}, 4256, 0};
            break;
        case Int256Shift:
            cpuParams[val] = ContractCostParamEntry{ExtensionPoint{0}, 884, 0};
            break;
        case ChaCha20DrawBytes:
            cpuParams[val] =
                ContractCostParamEntry{ExtensionPoint{0}, 1059, 502};
            break;
        default:
            break;
        }
    }

    LedgerKey memKey(CONFIG_SETTING);
    memKey.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_COST_PARAMS_MEMORY_BYTES;

    auto& memParams = ltx.load(memKey)
                          .current()
                          .data.configSetting()
                          .contractCostParamsMemBytes();
    for (size_t val = 0; val < memParams.size(); ++val)
    {
        switch (val)
        {
        case VmInstantiation:
            memParams[val] =
                ContractCostParamEntry{ExtensionPoint{0}, 132773, 4903};
            break;
        case VmCachedInstantiation:
            memParams[val] =
                ContractCostParamEntry{ExtensionPoint{0}, 132773, 4903};
            break;
        default:
            break;
        }
    }

    ltx.commit();
}
#endif

bool
SorobanNetworkConfig::isValidCostParams(ContractCostParams const& params,
                                        uint32_t ledgerVersion)
{
    auto getNumCostTypes = [](uint32_t ledgerVersion) -> uint32_t {
        if (protocolVersionIsBefore(ledgerVersion, ProtocolVersion::V_21))
        {
            return static_cast<uint32_t>(ContractCostType::ChaCha20DrawBytes) +
                   1;
        }
        else if (protocolVersionIsBefore(ledgerVersion, ProtocolVersion::V_22))
        {
            return static_cast<uint32_t>(
                       ContractCostType::VerifyEcdsaSecp256r1Sig) +
                   1;
        }
        else
        {
            return static_cast<uint32_t>(ContractCostType::Bls12381FrInv) + 1;
        }
    };

    if (params.size() != getNumCostTypes(ledgerVersion))
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
SorobanNetworkConfig::rustBridgeFeeConfiguration(uint32_t ledgerVersion) const
{
    CxxFeeConfiguration res{};
    res.fee_per_instruction_increment = feeRatePerInstructionsIncrement();

    res.fee_per_disk_read_entry = feeDiskReadLedgerEntry();
    res.fee_per_write_entry = feeWriteLedgerEntry();
    res.fee_per_disk_read_1kb = feeDiskRead1KB();
    res.fee_per_write_1kb =
        protocolVersionStartsFrom(ledgerVersion, ProtocolVersion::V_23)
            ? feeFlatRateWrite1KB()
            : feeRent1KB();

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
    res.fee_per_rent_1kb = feeRent1KB();
    // Note, while this is only introduced in protocol 23, it's safe to just
    // pass 0 prior to protocol 23 through to bridge (it will be ignored).
    res.fee_per_write_1kb = feeFlatRateWrite1KB();
    res.fee_per_write_entry = feeWriteLedgerEntry();
    res.persistent_rent_rate_denominator = cfg.persistentRentRateDenominator;
    res.temporary_rent_rate_denominator = cfg.tempRentRateDenominator;
    return res;
}

void
SorobanNetworkConfig::computeRentWriteFee(uint32_t protocolVersion)
{
    ZoneScoped;

    CxxRentWriteFeeConfiguration feeConfig{};
    feeConfig.state_target_size_bytes = mSorobanStateTargetSizeBytes;
    feeConfig.state_size_rent_fee_growth_factor =
        mSorobanStateRentFeeGrowthFactor;
    feeConfig.rent_fee_1kb_state_size_low = mRentFee1KBSorobanStateSizeLow;
    feeConfig.rent_fee_1kb_state_size_high = mRentFee1KBSorobanStateSizeHigh;
    // This may throw, but only if core is mis-configured.
    mFeeRent1KB = rust_bridge::compute_rent_write_fee_per_1kb(
        Config::CURRENT_LEDGER_PROTOCOL_VERSION, protocolVersion,
        mAverageSorobanStateSize, feeConfig);
}

bool
SorobanNetworkConfig::operator==(SorobanNetworkConfig const& other) const
{
    return mMaxContractSizeBytes == other.maxContractSizeBytes() &&
           mMaxContractDataKeySizeBytes ==
               other.maxContractDataKeySizeBytes() &&
           mMaxContractDataEntrySizeBytes ==
               other.maxContractDataEntrySizeBytes() &&

           mLedgerMaxInstructions == other.ledgerMaxInstructions() &&
           mTxMaxInstructions == other.txMaxInstructions() &&
           mFeeRatePerInstructionsIncrement ==
               other.feeRatePerInstructionsIncrement() &&
           mTxMemoryLimit == other.txMemoryLimit() &&

           mLedgerMaxDiskReadEntries == other.ledgerMaxDiskReadEntries() &&
           mLedgerMaxDiskReadBytes == other.ledgerMaxDiskReadBytes() &&
           mLedgerMaxWriteLedgerEntries ==
               other.ledgerMaxWriteLedgerEntries() &&
           mLedgerMaxWriteBytes == other.ledgerMaxWriteBytes() &&
           mLedgerMaxTxCount == other.ledgerMaxTxCount() &&

           mTxMaxDiskReadEntries == other.txMaxDiskReadEntries() &&
           mTxMaxFootprintEntries == other.txMaxFootprintEntries() &&
           mTxMaxDiskReadBytes == other.txMaxDiskReadBytes() &&
           mTxMaxWriteLedgerEntries == other.txMaxWriteLedgerEntries() &&
           mTxMaxWriteBytes == other.txMaxWriteBytes() &&
           mFeeDiskReadLedgerEntry == other.feeDiskReadLedgerEntry() &&
           mFeeWriteLedgerEntry == other.feeWriteLedgerEntry() &&
           mFeeDiskRead1KB == other.feeDiskRead1KB() &&
           mFeeFlatRateWrite1KB == other.feeFlatRateWrite1KB() &&
           mSorobanStateTargetSizeBytes ==
               other.sorobanStateTargetSizeBytes() &&

           mRentFee1KBSorobanStateSizeLow ==
               other.rentFee1KBSorobanStateSizeLow() &&
           mRentFee1KBSorobanStateSizeHigh ==
               other.rentFee1KBSorobanStateSizeHigh() &&
           mSorobanStateRentFeeGrowthFactor ==
               other.sorobanStateRentFeeGrowthFactor() &&

           mFeeHistorical1KB == other.feeHistorical1KB() &&

           mTxMaxContractEventsSizeBytes ==
               other.txMaxContractEventsSizeBytes() &&
           mFeeContractEvents1KB == other.feeContractEventsSize1KB() &&

           mLedgerMaxTransactionsSizeBytes ==
               other.ledgerMaxTransactionSizesBytes() &&
           mTxMaxSizeBytes == other.txMaxSizeBytes() &&
           mFeeTransactionSize1KB == other.feeTransactionSize1KB() &&

           mCpuCostParams == other.cpuCostParams() &&
           mMemCostParams == other.memCostParams() &&

           mLedgerMaxDependentTxClusters ==
               other.ledgerMaxDependentTxClusters() &&

           mStateArchivalSettings == other.stateArchivalSettings() &&
           mEvictionIterator == other.evictionIterator() &&

           mLedgerTargetCloseTimeMilliseconds ==
               other.ledgerTargetCloseTimeMilliseconds() &&
           mNominationTimeoutInitialMilliseconds ==
               other.nominationTimeoutInitialMilliseconds() &&
           mNominationTimeoutIncrementMilliseconds ==
               other.nominationTimeoutIncrementMilliseconds() &&
           mBallotTimeoutInitialMilliseconds ==
               other.ballotTimeoutInitialMilliseconds() &&
           mBallotTimeoutIncrementMilliseconds ==
               other.ballotTimeoutIncrementMilliseconds();
}

} // namespace stellar
