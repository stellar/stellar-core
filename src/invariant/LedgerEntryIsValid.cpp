// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/LedgerEntryIsValid.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerTxn.h"
#include "main/Application.h"
#include "util/GlobalChecks.h"
#include "util/ProtocolVersion.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdrpp/printer.h"
#include <crypto/SHA.h>
#include <fmt/format.h>

namespace stellar
{

static bool
signerCompare(Signer const& s1, Signer const& s2)
{
    return s1.key < s2.key;
}

#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
LedgerEntryIsValid::LedgerEntryIsValid(
    LumenContractInfo const& lumenContractInfo)
    : Invariant(false), mLumenContractInfo(lumenContractInfo)
{
}
#else
LedgerEntryIsValid::LedgerEntryIsValid() : Invariant(false)
{
}
#endif

std::shared_ptr<Invariant>
LedgerEntryIsValid::registerInvariant(Application& app)
{
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    auto lumenInfo = getLumenContractInfo(app.getConfig().NETWORK_PASSPHRASE);
    return app.getInvariantManager().registerInvariant<LedgerEntryIsValid>(
        lumenInfo);
#else
    return app.getInvariantManager().registerInvariant<LedgerEntryIsValid>();
#endif
}

std::string
LedgerEntryIsValid::getName() const
{
    return "LedgerEntryIsValid";
}

std::string
LedgerEntryIsValid::checkOnOperationApply(Operation const& operation,
                                          OperationResult const& result,
                                          LedgerTxnDelta const& ltxDelta)
{
    uint32_t currLedgerSeq = ltxDelta.header.current.ledgerSeq;
    if (currLedgerSeq > INT32_MAX)
    {
        return fmt::format(
            FMT_STRING("LedgerHeader ledgerSeq ({:d}) exceeds limits ({:d})"),
            currLedgerSeq, INT32_MAX);
    }

    auto ver = ltxDelta.header.current.ledgerVersion;
    for (auto const& entryDelta : ltxDelta.entry)
    {
        if (!entryDelta.second.current)
            continue;

        auto s = checkIsValid(*entryDelta.second.current,
                              entryDelta.second.previous, currLedgerSeq, ver);
        if (!s.empty())
        {
            s += ": " + entryDelta.second.current->toString();
            return s;
        }
    }
    return {};
}

std::string
LedgerEntryIsValid::checkIsValid(
    InternalLedgerEntry const& le,
    std::shared_ptr<InternalLedgerEntry const> const& genPrevious,
    uint32_t ledgerSeq, uint32 version) const
{
    if (le.type() == InternalLedgerEntryType::LEDGER_ENTRY)
    {
        auto const* previous =
            genPrevious ? &genPrevious->ledgerEntry() : nullptr;
        return checkIsValid(le.ledgerEntry(), previous, ledgerSeq, version);
    }
    return "";
}

std::string
LedgerEntryIsValid::checkIsValid(LedgerEntry const& le,
                                 LedgerEntry const* previous,
                                 uint32_t ledgerSeq, uint32 version) const
{
    if (le.lastModifiedLedgerSeq != ledgerSeq)
    {
        return fmt::format(
            FMT_STRING("LedgerEntry lastModifiedLedgerSeq ({:d}) does not"
                       " equal LedgerHeader ledgerSeq ({:d})"),
            le.lastModifiedLedgerSeq, ledgerSeq);
    }

    if (protocolVersionIsBefore(version, ProtocolVersion::V_14) &&
        le.ext.v() == 1)
    {
        return "LedgerEntry has v1 extension before protocol version 14";
    }

    switch (le.data.type())
    {
    case ACCOUNT:
        return checkIsValid(le.data.account(), version);
    case TRUSTLINE:
        return checkIsValid(le.data.trustLine(), previous, version);
    case OFFER:
        return checkIsValid(le.data.offer(), version);
    case DATA:
        return checkIsValid(le.data.data(), version);
    case CLAIMABLE_BALANCE:
        if (le.ext.v() != 1 || !le.ext.v1().sponsoringID)
        {
            return "ClaimableBalance is not sponsored";
        }
        return checkIsValid(le.data.claimableBalance(), previous, version);
    case LIQUIDITY_POOL:
        if (le.ext.v() != 0)
        {
            return "LiquidityPool is sponsored";
        }
        return checkIsValid(le.data.liquidityPool(), previous, version);
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    case CONTRACT_DATA:
        return checkIsValid(le.data.contractData(), previous, version);
    case CONTRACT_CODE:
        return checkIsValid(le.data.contractCode(), previous, version);
    case CONFIG_SETTING:
        return checkIsValid(le.data.configSetting(), previous, version);
#endif
    default:
        return "LedgerEntry has invalid type";
    }
}

std::string
LedgerEntryIsValid::checkIsValid(AccountEntry const& ae, uint32 version) const
{
    if (ae.balance < 0)
    {
        return fmt::format(FMT_STRING("Account balance ({:d}) is negative"),
                           ae.balance);
    }
    if (ae.seqNum < 0)
    {
        return fmt::format(FMT_STRING("Account seqNum ({:d}) is negative"),
                           ae.seqNum);
    }
    if (ae.numSubEntries > INT32_MAX)
    {
        return fmt::format(
            FMT_STRING("Account numSubEntries ({:d}) exceeds limit ({:d})"),
            ae.numSubEntries, INT32_MAX);
    }

    if (!accountFlagIsValid(ae.flags, version))
    {
        return "Account flags are invalid";
    }

    if (!isStringValid(ae.homeDomain))
    {
        return "Account homeDomain is invalid";
    }
    if (std::adjacent_find(ae.signers.begin(), ae.signers.end(),
                           [](Signer const& s1, Signer const& s2) {
                               return !signerCompare(s1, s2);
                           }) != ae.signers.end())
    {
        return "Account signers are not strictly increasing";
    }
    if (protocolVersionStartsFrom(version, ProtocolVersion::V_10))
    {
        if (!std::all_of(ae.signers.begin(), ae.signers.end(),
                         [](Signer const& s) {
                             return (s.weight <= UINT8_MAX) && (s.weight != 0);
                         }))
        {
            return "Account signers have invalid weights";
        }
    }

    if (hasAccountEntryExtV2(ae))
    {
        if (protocolVersionIsBefore(version, ProtocolVersion::V_14))
        {
            return "Account has v2 extension before protocol version 14";
        }
        auto const& extV2 = ae.ext.v1().ext.v2();
        if (ae.signers.size() != extV2.signerSponsoringIDs.size())
        {
            return "Account signers not paired with signerSponsoringIDs";
        }

        if (protocolVersionStartsFrom(version, ProtocolVersion::V_18) &&
            ae.numSubEntries > UINT32_MAX - extV2.numSponsoring)
        {
            return "Account numSubEntries + numSponsoring is > UINT32_MAX";
        }
    }
    return {};
}

std::string
LedgerEntryIsValid::checkIsValid(TrustLineEntry const& tl,
                                 LedgerEntry const* previous,
                                 uint32 version) const
{
    if (tl.asset.type() == ASSET_TYPE_NATIVE)
    {
        return "TrustLine asset is native";
    }
    if (!isAssetValid(tl.asset, version))
    {
        return "TrustLine asset is invalid";
    }
    if (tl.asset.type() == ASSET_TYPE_POOL_SHARE && tl.ext.v() == 1 &&
        (tl.ext.v1().liabilities.buying != 0 ||
         tl.ext.v1().liabilities.selling != 0))
    {
        return "Pool share TrustLine has liabilities";
    }
    if (hasTrustLineEntryExtV2(tl))
    {
        if (protocolVersionIsBefore(version, ProtocolVersion::V_18))
        {
            return "TrustLine has v2 extension before protocol version 18";
        }
        if (tl.ext.v1().ext.v2().liquidityPoolUseCount < 0)
        {
            return "TrustLine liquidityPoolUseCount is negative";
        }
    }
    if (tl.balance < 0)
    {
        return fmt::format(FMT_STRING("TrustLine balance ({:d}) is negative"),
                           tl.balance);
    }
    if (tl.limit <= 0)
    {
        return fmt::format(FMT_STRING("TrustLine limit ({:d}) is not positive"),
                           tl.limit);
    }
    if (tl.balance > tl.limit)
    {
        return fmt::format(
            FMT_STRING("TrustLine balance ({:d}) exceeds limit ({:d})"),
            tl.balance, tl.limit);
    }
    if (!trustLineFlagIsValid(tl.flags, version))
    {
        return "TrustLine flags are invalid";
    }
    if (previous && !isClawbackEnabledOnTrustline(previous->data.trustLine()) &&
        isClawbackEnabledOnTrustline(tl))
    {
        return "TrustLine clawback flag was enabled";
    }
    return {};
}

std::string
LedgerEntryIsValid::checkIsValid(OfferEntry const& oe, uint32 version) const
{
    if (oe.offerID <= 0)
    {
        return fmt::format(FMT_STRING("Offer offerID ({:d}) must be positive"),
                           oe.offerID);
    }
    if (!isAssetValid(oe.selling, version))
    {
        return "Offer selling asset is invalid";
    }
    if (!isAssetValid(oe.buying, version))
    {
        return "Offer buying asset is invalid";
    }
    if (oe.amount <= 0)
    {
        return "Offer amount is not positive";
    }
    if (oe.price.n <= 0 || oe.price.d < 1)
    {
        return fmt::format(FMT_STRING("Offer price ({:d} / {:d}) is invalid"),
                           oe.price.n, oe.price.d);
    }
    if ((oe.flags & ~MASK_OFFERENTRY_FLAGS) != 0)
    {
        return "Offer flags are invalid";
    }
    return {};
}

std::string
LedgerEntryIsValid::checkIsValid(DataEntry const& de, uint32 version) const
{
    if (de.dataName.size() == 0)
    {
        return "Data dataName is empty";
    }
    if (!isStringValid(de.dataName))
    {
        return "Data dataName is invalid";
    }
    return {};
}

bool
LedgerEntryIsValid::validatePredicate(ClaimPredicate const& pred,
                                      uint32_t depth) const
{
    if (depth > 4)
    {
        return false;
    }

    switch (pred.type())
    {
    case CLAIM_PREDICATE_UNCONDITIONAL:
        break;
    case CLAIM_PREDICATE_AND:
    {
        auto const& andPredicates = pred.andPredicates();
        if (andPredicates.size() != 2)
        {
            return false;
        }
        return validatePredicate(andPredicates[0], depth + 1) &&
               validatePredicate(andPredicates[1], depth + 1);
    }

    case CLAIM_PREDICATE_OR:
    {
        auto const& orPredicates = pred.orPredicates();
        if (orPredicates.size() != 2)
        {
            return false;
        }
        return validatePredicate(orPredicates[0], depth + 1) &&
               validatePredicate(orPredicates[1], depth + 1);
    }
    case CLAIM_PREDICATE_NOT:
    {
        if (!pred.notPredicate())
        {
            return false;
        }
        return validatePredicate(*pred.notPredicate(), depth + 1);
    }

    case CLAIM_PREDICATE_BEFORE_ABSOLUTE_TIME:
        return pred.absBefore() >= 0;
    default:
        return false;
    }

    return true;
}

std::string
LedgerEntryIsValid::checkIsValid(ClaimableBalanceEntry const& cbe,
                                 LedgerEntry const* previous,
                                 uint32 version) const
{
    if (protocolVersionIsBefore(version, ProtocolVersion::V_17) &&
        cbe.ext.v() == 1)
    {
        return "ClaimableBalance has v1 extension before protocol version 17";
    }

    if (isClawbackEnabledOnClaimableBalance(cbe) &&
        cbe.asset.type() == ASSET_TYPE_NATIVE)
    {
        return "ClaimableBalance clawback set on native balance";
    }

    if (!claimableBalanceFlagIsValid(cbe))
    {
        return "ClaimableBalance flags are invalid";
    }

    if (previous)
    {
        releaseAssert(previous->data.type() == CLAIMABLE_BALANCE);
        auto const& previousCbe = previous->data.claimableBalance();

        if (!(cbe == previousCbe))
        {
            return "ClaimableBalance cannot be modified";
        }
    }

    if (cbe.claimants.empty())
    {
        return "ClaimableBalance claimants is empty";
    }
    if (!isAssetValid(cbe.asset, version))
    {
        return "ClaimableBalance asset is invalid";
    }
    if (cbe.amount <= 0)
    {
        return "ClaimableBalance amount is not positive";
    }

    for (auto const& claimant : cbe.claimants)
    {
        if (!validatePredicate(claimant.v0().predicate, 1))
        {
            return "ClaimableBalance claimant is invalid";
        }
    }

    return {};
}

std::string
LedgerEntryIsValid::checkIsValid(LiquidityPoolEntry const& lp,
                                 LedgerEntry const* previous,
                                 uint32 version) const
{
    if (protocolVersionIsBefore(version, ProtocolVersion::V_18))
    {
        return "LiquidityPools are only valid from V18";
    }

    if (lp.body.type() != LIQUIDITY_POOL_CONSTANT_PRODUCT)
    {
        return "LiquidityPool type must be constant product";
    }

    auto const& cp = lp.body.constantProduct();
    if (!isAssetValid(cp.params.assetA, version))
    {
        return "LiquidityPool assetA is invalid";
    }
    if (!isAssetValid(cp.params.assetB, version))
    {
        return "LiquidityPool assetB is invalid";
    }
    if (!(cp.params.assetA < cp.params.assetB))
    {
        return "LiquidityPool assets are in an invalid order";
    }
    if (cp.params.fee != 30)
    {
        return "LiquidityPool fee is not 30 basis points";
    }

    if (cp.reserveA < 0)
    {
        return "LiquidityPool reserveA is negative";
    }
    if (cp.reserveB < 0)
    {
        return "LiquidityPool reserveB is negative";
    }
    if (cp.totalPoolShares < 0)
    {
        return "LiquidityPool totalPoolShares is negative";
    }
    if (cp.poolSharesTrustLineCount < 0)
    {
        return "LiquidityPool poolSharesTrustLineCount is negative";
    }

    if (previous)
    {
        if (previous->data.type() != LIQUIDITY_POOL)
        {
            return "LiquidityPool used to be of different type";
        }

        auto const& lpPrev = previous->data.liquidityPool();
        if (lpPrev.body.type() != lp.body.type())
        {
            return "LiquidityPool body changed type";
        }

        if (!(lpPrev.body.constantProduct().params ==
              lp.body.constantProduct().params))
        {
            return "LiquidityPool parameters changed";
        }
    }

    return {};
}

#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
std::string
LedgerEntryIsValid::checkIsValid(ContractDataEntry const& cde,
                                 LedgerEntry const* previous,
                                 uint32 version) const
{
    // Lumen contract validation
    if (cde.contract.type() == SC_ADDRESS_TYPE_CONTRACT &&
        cde.contract.contractId() == mLumenContractInfo.mLumenContractID)
    {
        // Identify balance entries
        if (cde.key.type() == SCV_VEC && cde.key.vec() &&
            !cde.key.vec()->empty() &&
            cde.key.vec()->at(0) == mLumenContractInfo.mBalanceSymbol)
        {
            if (cde.durability != ContractDataDurability::PERSISTENT)
            {
                return "Balance entry must be persistent";
            }
            if (cde.body.bodyType() != DATA_ENTRY)
            {
                return "Expected data entry for balance";
            }

            auto const& val = cde.body.data().val;
            if (val.type() != SCV_MAP || val.map()->size() == 0)
            {
                return "Balance entry val must be a populated Map";
            }

            auto const& amountEntry = val.map()->at(0);
            if (!(amountEntry.key == mLumenContractInfo.mAmountSymbol))
            {
                return "Balance amount symbol is incorrect";
            }
            if (amountEntry.val.type() != SCV_I128)
            {
                return "Balance amount must be a I128";
            }
            auto lo = amountEntry.val.i128().lo;
            auto hi = amountEntry.val.i128().hi;
            if (lo > INT64_MAX || hi > 0)
            {
                return "Balance amount is invalid";
            }
        }
    }

    if (cde.body.bodyType() == DATA_ENTRY &&
        (cde.body.data().flags & ~MASK_CONTRACT_DATA_FLAGS_V20) != 0)
    {
        return "Invalid contract data flags";
    }
    return {};
}

std::string
LedgerEntryIsValid::checkIsValid(ContractCodeEntry const& cce,
                                 LedgerEntry const* previous,
                                 uint32 version) const
{
    if (cce.body.bodyType() == DATA_ENTRY &&
        sha256(cce.body.code()) != cce.hash)
    {
        return "Contract code doesn't match hash";
    }

    if (!previous)
    {
        return {};
    }
    else if (previous->data.type() != CONTRACT_CODE)
    {
        return "ContractCode used to be of different type";
    }

    auto const& prevCode = previous->data.contractCode();
    if (cce.hash != prevCode.hash)
    {
        return "ContractCode hash modified";
    }

    if (cce.body.bodyType() != prevCode.body.bodyType())
    {
        return "Mismatch on bodytype";
    }

    if (cce.body.bodyType() == DATA_ENTRY &&
        cce.body.code() != prevCode.body.code())
    {
        return "ContractCode code modified";
    }
    return {};
}

std::string
LedgerEntryIsValid::checkIsValid(ConfigSettingEntry const& cfg,
                                 LedgerEntry const* previous,
                                 uint32 version) const
{
    switch (cfg.configSettingID())
    {
    case ConfigSettingID::CONFIG_SETTING_CONTRACT_MAX_SIZE_BYTES:
        if (cfg.contractMaxSizeBytes() <= 0)
        {
            return "Invalid contractMaxSizeBytes";
        }
        break;
    case ConfigSettingID::CONFIG_SETTING_CONTRACT_COST_PARAMS_CPU_INSTRUCTIONS:
        if (!SorobanNetworkConfig::isValidCostParams(
                cfg.contractCostParamsCpuInsns()))
        {
            return "Invalid contractCostParamsCpuInsns";
        }
        break;
    case ConfigSettingID::CONFIG_SETTING_CONTRACT_COST_PARAMS_MEMORY_BYTES:
        if (!SorobanNetworkConfig::isValidCostParams(
                cfg.contractCostParamsMemBytes()))
        {
            return "Invalid contractCostParamsMemBytes";
        }
        break;
    case ConfigSettingID::CONFIG_SETTING_CONTRACT_DATA_KEY_SIZE_BYTES:
        if (cfg.contractDataKeySizeBytes() <= 0)
        {
            return "Invalid contractDataKeySizeBytes";
        }
        break;
    case ConfigSettingID::CONFIG_SETTING_CONTRACT_DATA_ENTRY_SIZE_BYTES:
        if (cfg.contractDataEntrySizeBytes() <= 0)
        {
            return "Invalid contractDataEntrySizeBytes";
        }
        break;
    case ConfigSettingID::CONFIG_SETTING_CONTRACT_EXECUTION_LANES:
        break;
    case ConfigSettingID::CONFIG_SETTING_CONTRACT_BANDWIDTH_V0:
        if (cfg.contractBandwidth().feePropagateData1KB < 0 ||
            cfg.contractBandwidth().ledgerMaxPropagateSizeBytes <
                MinimumSorobanNetworkConfig::LEDGER_MAX_PROPAGATE_SIZE_BYTES ||
            cfg.contractBandwidth().txMaxSizeBytes <
                MinimumSorobanNetworkConfig::TX_MAX_SIZE_BYTES)
        {
            return "Invalid contractBandwidth";
        }
        break;
    case ConfigSettingID::CONFIG_SETTING_CONTRACT_COMPUTE_V0:
        if (cfg.contractCompute().feeRatePerInstructionsIncrement < 0 ||
            cfg.contractCompute().ledgerMaxInstructions <
                MinimumSorobanNetworkConfig::LEDGER_MAX_INSTRUCTIONS ||
            cfg.contractCompute().txMaxInstructions <
                MinimumSorobanNetworkConfig::TX_MAX_INSTRUCTIONS ||
            cfg.contractCompute().txMemoryLimit <
                MinimumSorobanNetworkConfig::MEMORY_LIMIT)
        {
            return "Invalid contractCompute";
        }
        break;
    case ConfigSettingID::CONFIG_SETTING_CONTRACT_HISTORICAL_DATA_V0:
        if (cfg.contractHistoricalData().feeHistorical1KB < 0)
        {
            return "Invalid feeHistorical1KB";
        }
        break;
    case ConfigSettingID::CONFIG_SETTING_CONTRACT_LEDGER_COST_V0:
        if (cfg.contractLedgerCost().ledgerMaxReadLedgerEntries <
                MinimumSorobanNetworkConfig::LEDGER_MAX_READ_LEDGER_ENTRIES ||
            cfg.contractLedgerCost().ledgerMaxReadBytes <
                MinimumSorobanNetworkConfig::LEDGER_MAX_READ_BYTES ||
            cfg.contractLedgerCost().ledgerMaxWriteLedgerEntries <
                MinimumSorobanNetworkConfig::LEDGER_MAX_WRITE_LEDGER_ENTRIES ||
            cfg.contractLedgerCost().ledgerMaxWriteBytes <
                MinimumSorobanNetworkConfig::LEDGER_MAX_WRITE_BYTES ||
            cfg.contractLedgerCost().txMaxReadLedgerEntries <
                MinimumSorobanNetworkConfig::LEDGER_MAX_READ_LEDGER_ENTRIES ||
            cfg.contractLedgerCost().txMaxReadBytes <
                MinimumSorobanNetworkConfig::TX_MAX_READ_BYTES ||
            cfg.contractLedgerCost().txMaxWriteLedgerEntries <
                MinimumSorobanNetworkConfig::TX_MAX_WRITE_LEDGER_ENTRIES ||
            cfg.contractLedgerCost().txMaxWriteBytes <
                MinimumSorobanNetworkConfig::TX_MAX_WRITE_BYTES ||
            cfg.contractLedgerCost().feeReadLedgerEntry < 0 ||
            cfg.contractLedgerCost().feeWriteLedgerEntry < 0 ||
            cfg.contractLedgerCost().feeRead1KB < 0 ||
            cfg.contractLedgerCost().bucketListTargetSizeBytes <= 0 ||
            cfg.contractLedgerCost().writeFee1KBBucketListLow < 0 ||
            cfg.contractLedgerCost().writeFee1KBBucketListHigh < 0 ||
            cfg.contractLedgerCost().bucketListWriteFeeGrowthFactor < 0)
        {
            return "Invalid contractLedgerCost";
        }
        if (cfg.contractLedgerCost().ledgerMaxReadLedgerEntries <
            cfg.contractLedgerCost().txMaxReadLedgerEntries)
        {
            return "ledgerMaxReadLedgerEntries < txMaxReadLedgerEntries";
        }
        if (cfg.contractLedgerCost().ledgerMaxReadBytes <
            cfg.contractLedgerCost().txMaxReadBytes)
        {
            return "ledgerMaxReadBytes < txMaxReadBytes";
        }
        if (cfg.contractLedgerCost().ledgerMaxWriteLedgerEntries <
            cfg.contractLedgerCost().txMaxWriteLedgerEntries)
        {
            return "ledgerMaxWriteLedgerEntries < txMaxWriteLedgerEntries";
        }
        if (cfg.contractLedgerCost().ledgerMaxWriteBytes <
            cfg.contractLedgerCost().txMaxWriteBytes)
        {
            return "ledgerMaxWriteBytes < txMaxWriteBytes";
        }
        break;
    case ConfigSettingID::CONFIG_SETTING_CONTRACT_META_DATA_V0:
        if (cfg.contractMetaData().txMaxExtendedMetaDataSizeBytes <
                MinimumSorobanNetworkConfig::
                    TX_MAX_EXTENDED_META_DATA_SIZE_BYTES ||
            cfg.contractMetaData().feeExtendedMetaData1KB < 0)
        {
            return "Invalid contractMetaData";
        }
    case ConfigSettingID::CONFIG_SETTING_STATE_EXPIRATION:
        if (cfg.stateExpirationSettings().maxEntryExpiration <
                MinimumSorobanNetworkConfig::MAXIMUM_ENTRY_LIFETIME ||
            cfg.stateExpirationSettings().minTempEntryExpiration < 1 ||
            cfg.stateExpirationSettings().minPersistentEntryExpiration <
                MinimumSorobanNetworkConfig::
                    MINIMUM_PERSISTENT_ENTRY_LIFETIME ||
            cfg.stateExpirationSettings().autoBumpLedgers < 0 ||
            cfg.stateExpirationSettings().persistentRentRateDenominator < 1 ||
            cfg.stateExpirationSettings().tempRentRateDenominator < 1 ||
            cfg.stateExpirationSettings().maxEntriesToExpire < 1 ||
            cfg.stateExpirationSettings().bucketListSizeWindowSampleSize < 1 ||
            cfg.stateExpirationSettings().evictionScanSize < 1)
        {
            return "Invalid stateExpirationSettings";
        }

        if (cfg.stateExpirationSettings().maxEntryExpiration <=
            cfg.stateExpirationSettings().minPersistentEntryExpiration)
        {
            return "maxEntryExpiration <= minPersistentEntryExpiration";
        }

        if (cfg.stateExpirationSettings().maxEntryExpiration <=
            cfg.stateExpirationSettings().minTempEntryExpiration)
        {
            return "maxEntryExpiration <= minTempEntryExpiration";
        }
    case ConfigSettingID::CONFIG_SETTING_BUCKETLIST_SIZE_WINDOW:
        break;
    }

    return {};
}
#endif
}
