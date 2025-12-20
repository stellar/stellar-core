// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "util/Decoder.h"

#include "xdr/Stellar-SCP.h"
#include "xdr/Stellar-contract-config-setting.h"
#include "xdr/Stellar-contract-env-meta.h"
#include "xdr/Stellar-contract.h"
#include "xdr/Stellar-exporter.h"
#include "xdr/Stellar-internal.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-ledger.h"
#include "xdr/Stellar-overlay.h"
#include "xdr/Stellar-transaction.h"
#include "xdr/Stellar-types.h"

#ifdef _XDRPP_CEREAL_H_HEADER_INCLUDED_
#error This header includes xdrpp/cereal.h after necessary definitions \
       so include this file instead of directly including xdrpp/cereal.h.
#endif

#include "crypto/Hex.h"
#include "crypto/SecretKey.h"
#include "transactions/TransactionUtils.h"
#include "util/types.h"
#include <cereal/archives/json.hpp>

using namespace std::placeholders;

template <uint32_t N>
void
cereal_override(cereal::JSONOutputArchive& ar, xdr::xstring<N> const& s,
                char const* field)
{
    xdr::archive(ar, static_cast<std::string const&>(s), field);
}

template <uint32_t N>
void
cereal_override(cereal::JSONOutputArchive& ar, xdr::opaque_array<N> const& s,
                char const* field)
{
    xdr::archive(ar, stellar::binToHex(stellar::ByteSlice(s.data(), s.size())),
                 field);
}

template <typename T>
std::enable_if_t<xdr::xdr_traits<T>::is_container>
cereal_override(cereal::JSONOutputArchive& ar, T const& t, char const* field)
{
    // CEREAL_SAVE_FUNCTION_NAME in include/cereal/archives/json.hpp runs
    // ar.setNextName() and ar(). ar() in turns calls process() in
    // include/cereal/cereal.hpp which calls prologue(), processImpl(),
    // epilogue(). We are imitating this behavior here by creating a sub-object
    // using prologue(), printing the content with xdr::archive, and finally
    // calling epilogue(). We must use xdr::archive instead of ar() because we
    // need to access the nested cereal_overrides.
    //
    // tl;dr This does what ar(cereal::make_nvp(...)) does while using nested
    // cereal_overrides.
    ar.setNextName(field);
    cereal::prologue(ar, t);

    // It does not matter what value we pass here to cereal::make_size_tag
    // since it will be ignored. See the comment
    //
    // > SizeTags are strictly ignored for JSON, they just indicate
    // > that the current node should be made into an array
    //
    // in include/cereal/archives/json.hpp
    ar(cereal::make_size_tag(0));
    for (auto const& element : t)
    {
        xdr::archive(ar, element);
    }
    cereal::epilogue(ar, t);
}

template <uint32_t N>
void
cereal_override(cereal::JSONOutputArchive& ar, xdr::opaque_vec<N> const& s,
                char const* field)
{
    xdr::archive(ar, stellar::binToHex(stellar::ByteSlice(s.data(), s.size())),
                 field);
}

void cereal_override(cereal::JSONOutputArchive& ar, stellar::PublicKey const& s,
                     char const* field);

void cereal_override(cereal::JSONOutputArchive& ar,
                     stellar::SCAddress const& addr, char const* field);

void cereal_override(cereal::JSONOutputArchive& ar,
                     stellar::ConfigUpgradeSetKey const& key,
                     char const* field);

void cereal_override(cereal::JSONOutputArchive& ar,
                     stellar::MuxedAccount const& muxedAccount,
                     char const* field);

void cerealPoolAsset(cereal::JSONOutputArchive& ar, stellar::Asset const& asset,
                     char const* field);

void cerealPoolAsset(cereal::JSONOutputArchive& ar,
                     stellar::TrustLineAsset const& asset, char const* field);

void cerealPoolAsset(cereal::JSONOutputArchive& ar,
                     stellar::ChangeTrustAsset const& asset, char const* field);

template <typename T>
typename std::enable_if<std::is_same<stellar::Asset, T>::value ||
                        std::is_same<stellar::TrustLineAsset, T>::value ||
                        std::is_same<stellar::ChangeTrustAsset, T>::value>::type
cereal_override(cereal::JSONOutputArchive& ar, T const& asset,
                char const* field)
{
    switch (asset.type())
    {
    case stellar::ASSET_TYPE_NATIVE:
        xdr::archive(ar, std::string("NATIVE"), field);
        break;
    case stellar::ASSET_TYPE_POOL_SHARE:
        cerealPoolAsset(ar, asset, field);
        break;
    case stellar::ASSET_TYPE_CREDIT_ALPHANUM4:
    case stellar::ASSET_TYPE_CREDIT_ALPHANUM12:
    {
        ar.setNextName(field);
        ar.startNode();

        // asset is templated, so we just pull the assetCode string directly so
        // we don't have to templatize assetToString
        std::string code;
        if (asset.type() == stellar::ASSET_TYPE_CREDIT_ALPHANUM4)
        {
            stellar::assetCodeToStr(asset.alphaNum4().assetCode, code);
        }
        else
        {
            stellar::assetCodeToStr(asset.alphaNum12().assetCode, code);
        }

        xdr::archive(ar, code, "assetCode");
        xdr::archive(ar, stellar::getIssuer(asset), "issuer");
        ar.finishNode();
        break;
    }
    default:
        xdr::archive(ar, std::string("UNKNOWN"), field);
    }
}

template <typename T>
typename std::enable_if<xdr::xdr_traits<T>::is_enum>::type
cereal_override(cereal::JSONOutputArchive& ar, T const& t, char const* field)
{
    auto const np = xdr::xdr_traits<T>::enum_name(t);
    std::string name;
    if (np != nullptr)
    {
        name = np;
    }
    else
    {
        name = std::to_string(t);
    }
    xdr::archive(ar, name, field);
}

template <typename T>
void
cereal_override(cereal::JSONOutputArchive& ar, xdr::pointer<T> const& t,
                char const* field)
{
    // We tolerate a little information-loss here collapsing *T into T for
    // the non-null case, and use JSON 'null' for the null case. This reads
    // much better than the thing JSONOutputArchive does with PtrWrapper.
    if (t)
    {
        xdr::archive(ar, *t, field);
    }
    else
    {
        ar.setNextName(field);
        ar.writeName();
        ar.saveValue(nullptr);
    }
}

// NOTE: Nothing else should include xdrpp/cereal.h directly.
// cereal_override's have to be defined before xdrpp/cereal.h,
// otherwise some interplay of name lookup and visibility
// during the enable_if call in the cereal adaptor fails to find them.
#include <xdrpp/cereal.h>

namespace stellar
{

template <typename T> bool constexpr xdrJsonName{false};

// Convert t to SEP-0051 compliant JSON, falling back to base64 encoding in case
// of an error
template <typename T>
std::string
xdrToJson(T const& t, bool compact = false)
{
    static_assert(xdrJsonName<T>,
                  "XDR Type missing: add to xdrJsonName in XDRCereal.h");
    auto serialized = xdr::xdr_to_opaque(t);
    auto json = rust_bridge::xdr_to_json(serialized, xdrJsonName<T>, compact);
    if (!json.empty())
    {
        return {json};
    }
    return decoder::encode_b64(serialized);
}

template <typename T>
std::string
xdrToJson(xdr::xvector<T> const& t, bool compact = false)
{
    static_assert(xdrJsonName<T>,
                  "XDR Type missing: add to xdrJsonName in XDRCereal.h");
    std::string result = "[";
    bool isFirst = true;
    for (auto const& entry : t)
    {
        if (!isFirst)
        {
            result += ",";
        }
        auto serialized = xdr::xdr_to_opaque(entry);
        auto json =
            rust_bridge::xdr_to_json(serialized, xdrJsonName<T>, compact);
        if (!json.empty())
        {
            result += std::string{json};
        }
        else
        {
            result += '"';
            result += decoder::encode_b64(serialized);
            result += '"';
        }
    }
    return result;
}

template <>
inline constexpr char const* xdrJsonName<AccountEntry> = "AccountEntry";
template <>
inline constexpr char const* xdrJsonName<AccountEntryExtensionV1> =
    "AccountEntryExtensionV1";
template <>
inline constexpr char const* xdrJsonName<AccountEntryExtensionV2> =
    "AccountEntryExtensionV2";
template <>
inline constexpr char const* xdrJsonName<AccountEntryExtensionV3> =
    "AccountEntryExtensionV3";
template <>
inline constexpr char const* xdrJsonName<AccountFlags> = "AccountFlags";
template <> inline constexpr char const* xdrJsonName<AccountID> = "AccountId";
template <>
inline constexpr char const* xdrJsonName<AccountMergeResult> =
    "AccountMergeResult";
template <>
inline constexpr char const* xdrJsonName<AccountMergeResultCode> =
    "AccountMergeResultCode";
template <>
inline constexpr char const* xdrJsonName<AllowTrustOp> = "AllowTrustOp";
template <>
inline constexpr char const* xdrJsonName<AllowTrustResult> = "AllowTrustResult";
template <>
inline constexpr char const* xdrJsonName<AllowTrustResultCode> =
    "AllowTrustResultCode";
template <> inline constexpr char const* xdrJsonName<AlphaNum12> = "AlphaNum12";
template <> inline constexpr char const* xdrJsonName<AlphaNum4> = "AlphaNum4";
template <> inline constexpr char const* xdrJsonName<Asset> = "Asset";
template <>
inline constexpr char const* xdrJsonName<AssetCode12> = "AssetCode12";
template <> inline constexpr char const* xdrJsonName<AssetCode4> = "AssetCode4";
template <> inline constexpr char const* xdrJsonName<AssetCode> = "AssetCode";
template <> inline constexpr char const* xdrJsonName<AssetType> = "AssetType";
template <> inline constexpr char const* xdrJsonName<Auth> = "Auth";
template <> inline constexpr char const* xdrJsonName<AuthCert> = "AuthCert";
template <>
inline constexpr char const* xdrJsonName<AuthenticatedMessage> =
    "AuthenticatedMessage";
template <>
inline constexpr char const* xdrJsonName<BeginSponsoringFutureReservesOp> =
    "BeginSponsoringFutureReservesOp";
template <>
inline constexpr char const* xdrJsonName<BeginSponsoringFutureReservesResult> =
    "BeginSponsoringFutureReservesResult";
template <>
inline constexpr char const*
    xdrJsonName<BeginSponsoringFutureReservesResultCode> =
        "BeginSponsoringFutureReservesResultCode";
template <>
inline constexpr char const* xdrJsonName<BinaryFuseFilterType> =
    "BinaryFuseFilterType";
template <>
inline constexpr char const* xdrJsonName<BucketEntry> = "BucketEntry";
template <>
inline constexpr char const* xdrJsonName<BucketEntryType> = "BucketEntryType";
template <>
inline constexpr char const* xdrJsonName<BucketListType> = "BucketListType";
template <>
inline constexpr char const* xdrJsonName<BucketMetadata> = "BucketMetadata";
template <>
inline constexpr char const* xdrJsonName<BumpSequenceOp> = "BumpSequenceOp";
template <>
inline constexpr char const* xdrJsonName<BumpSequenceResult> =
    "BumpSequenceResult";
template <>
inline constexpr char const* xdrJsonName<BumpSequenceResultCode> =
    "BumpSequenceResultCode";
template <>
inline constexpr char const* xdrJsonName<ChangeTrustAsset> = "ChangeTrustAsset";
template <>
inline constexpr char const* xdrJsonName<ChangeTrustOp> = "ChangeTrustOp";
template <>
inline constexpr char const* xdrJsonName<ChangeTrustResult> =
    "ChangeTrustResult";
template <>
inline constexpr char const* xdrJsonName<ChangeTrustResultCode> =
    "ChangeTrustResultCode";
template <> inline constexpr char const* xdrJsonName<ClaimAtom> = "ClaimAtom";
template <>
inline constexpr char const* xdrJsonName<ClaimAtomType> = "ClaimAtomType";
template <>
inline constexpr char const* xdrJsonName<ClaimClaimableBalanceOp> =
    "ClaimClaimableBalanceOp";
template <>
inline constexpr char const* xdrJsonName<ClaimClaimableBalanceResult> =
    "ClaimClaimableBalanceResult";
template <>
inline constexpr char const* xdrJsonName<ClaimClaimableBalanceResultCode> =
    "ClaimClaimableBalanceResultCode";
template <>
inline constexpr char const* xdrJsonName<ClaimLiquidityAtom> =
    "ClaimLiquidityAtom";
template <>
inline constexpr char const* xdrJsonName<ClaimOfferAtom> = "ClaimOfferAtom";
template <>
inline constexpr char const* xdrJsonName<ClaimOfferAtomV0> = "ClaimOfferAtomV0";
template <>
inline constexpr char const* xdrJsonName<ClaimPredicate> = "ClaimPredicate";
template <>
inline constexpr char const* xdrJsonName<ClaimPredicateType> =
    "ClaimPredicateType";
template <>
inline constexpr char const* xdrJsonName<ClaimableBalanceEntry> =
    "ClaimableBalanceEntry";
template <>
inline constexpr char const* xdrJsonName<ClaimableBalanceEntryExtensionV1> =
    "ClaimableBalanceEntryExtensionV1";
template <>
inline constexpr char const* xdrJsonName<ClaimableBalanceFlags> =
    "ClaimableBalanceFlags";
template <>
inline constexpr char const* xdrJsonName<ClaimableBalanceID> =
    "ClaimableBalanceId";
template <>
inline constexpr char const* xdrJsonName<ClaimableBalanceIDType> =
    "ClaimableBalanceIdType";
template <> inline constexpr char const* xdrJsonName<Claimant> = "Claimant";
template <>
inline constexpr char const* xdrJsonName<ClaimantType> = "ClaimantType";
template <>
inline constexpr char const* xdrJsonName<ClawbackClaimableBalanceOp> =
    "ClawbackClaimableBalanceOp";
template <>
inline constexpr char const* xdrJsonName<ClawbackClaimableBalanceResult> =
    "ClawbackClaimableBalanceResult";
template <>
inline constexpr char const* xdrJsonName<ClawbackClaimableBalanceResultCode> =
    "ClawbackClaimableBalanceResultCode";
template <> inline constexpr char const* xdrJsonName<ClawbackOp> = "ClawbackOp";
template <>
inline constexpr char const* xdrJsonName<ClawbackResult> = "ClawbackResult";
template <>
inline constexpr char const* xdrJsonName<ClawbackResultCode> =
    "ClawbackResultCode";
template <>
inline constexpr char const* xdrJsonName<ConfigSettingContractBandwidthV0> =
    "ConfigSettingContractBandwidthV0";
template <>
inline constexpr char const* xdrJsonName<ConfigSettingContractComputeV0> =
    "ConfigSettingContractComputeV0";
template <>
inline constexpr char const* xdrJsonName<ConfigSettingContractEventsV0> =
    "ConfigSettingContractEventsV0";
template <>
inline constexpr char const*
    xdrJsonName<ConfigSettingContractExecutionLanesV0> =
        "ConfigSettingContractExecutionLanesV0";
template <>
inline constexpr char const*
    xdrJsonName<ConfigSettingContractHistoricalDataV0> =
        "ConfigSettingContractHistoricalDataV0";
template <>
inline constexpr char const* xdrJsonName<ConfigSettingContractLedgerCostExtV0> =
    "ConfigSettingContractLedgerCostExtV0";
template <>
inline constexpr char const* xdrJsonName<ConfigSettingContractLedgerCostV0> =
    "ConfigSettingContractLedgerCostV0";
template <>
inline constexpr char const*
    xdrJsonName<ConfigSettingContractParallelComputeV0> =
        "ConfigSettingContractParallelComputeV0";
template <>
inline constexpr char const* xdrJsonName<ConfigSettingEntry> =
    "ConfigSettingEntry";
template <>
inline constexpr char const* xdrJsonName<ConfigSettingID> = "ConfigSettingId";
template <>
inline constexpr char const* xdrJsonName<ConfigSettingSCPTiming> =
    "ConfigSettingScpTiming";
template <>
inline constexpr char const* xdrJsonName<ConfigUpgradeSet> = "ConfigUpgradeSet";
template <>
inline constexpr char const* xdrJsonName<ConfigUpgradeSetKey> =
    "ConfigUpgradeSetKey";
template <>
inline constexpr char const* xdrJsonName<ContractCodeCostInputs> =
    "ContractCodeCostInputs";
template <>
inline constexpr char const* xdrJsonName<ContractCodeEntry> =
    "ContractCodeEntry";
template <>
inline constexpr char const* xdrJsonName<ContractCostParamEntry> =
    "ContractCostParamEntry";
template <>
inline constexpr char const* xdrJsonName<ContractCostParams> =
    "ContractCostParams";
template <>
inline constexpr char const* xdrJsonName<ContractCostType> = "ContractCostType";
template <>
inline constexpr char const* xdrJsonName<ContractDataDurability> =
    "ContractDataDurability";
template <>
inline constexpr char const* xdrJsonName<ContractDataEntry> =
    "ContractDataEntry";
template <>
inline constexpr char const* xdrJsonName<ContractEvent> = "ContractEvent";
template <>
inline constexpr char const* xdrJsonName<ContractEventType> =
    "ContractEventType";
template <>
inline constexpr char const* xdrJsonName<ContractExecutable> =
    "ContractExecutable";
template <>
inline constexpr char const* xdrJsonName<ContractExecutableType> =
    "ContractExecutableType";
template <>
inline constexpr char const* xdrJsonName<ContractIDPreimage> =
    "ContractIdPreimage";
template <>
inline constexpr char const* xdrJsonName<ContractIDPreimageType> =
    "ContractIdPreimageType";
template <>
inline constexpr char const* xdrJsonName<CreateAccountOp> = "CreateAccountOp";
template <>
inline constexpr char const* xdrJsonName<CreateAccountResult> =
    "CreateAccountResult";
template <>
inline constexpr char const* xdrJsonName<CreateAccountResultCode> =
    "CreateAccountResultCode";
template <>
inline constexpr char const* xdrJsonName<CreateClaimableBalanceOp> =
    "CreateClaimableBalanceOp";
template <>
inline constexpr char const* xdrJsonName<CreateClaimableBalanceResult> =
    "CreateClaimableBalanceResult";
template <>
inline constexpr char const* xdrJsonName<CreateClaimableBalanceResultCode> =
    "CreateClaimableBalanceResultCode";
template <>
inline constexpr char const* xdrJsonName<CreateContractArgs> =
    "CreateContractArgs";
template <>
inline constexpr char const* xdrJsonName<CreateContractArgsV2> =
    "CreateContractArgsV2";
template <>
inline constexpr char const* xdrJsonName<CreatePassiveSellOfferOp> =
    "CreatePassiveSellOfferOp";
template <>
inline constexpr char const* xdrJsonName<CryptoKeyType> = "CryptoKeyType";
template <>
inline constexpr char const* xdrJsonName<Curve25519Public> = "Curve25519Public";
template <>
inline constexpr char const* xdrJsonName<Curve25519Secret> = "Curve25519Secret";
template <> inline constexpr char const* xdrJsonName<DataEntry> = "DataEntry";
template <> inline constexpr char const* xdrJsonName<DataValue> = "DataValue";
template <>
inline constexpr char const* xdrJsonName<DecoratedSignature> =
    "DecoratedSignature";
template <>
inline constexpr char const* xdrJsonName<DependentTxCluster> =
    "DependentTxCluster";
template <>
inline constexpr char const* xdrJsonName<DiagnosticEvent> = "DiagnosticEvent";
template <> inline constexpr char const* xdrJsonName<DontHave> = "DontHave";
template <> inline constexpr char const* xdrJsonName<Duration> = "Duration";
template <>
inline constexpr char const* xdrJsonName<EncryptedBody> = "EncryptedBody";
template <>
inline constexpr char const* xdrJsonName<EndSponsoringFutureReservesResult> =
    "EndSponsoringFutureReservesResult";
template <>
inline constexpr char const*
    xdrJsonName<EndSponsoringFutureReservesResultCode> =
        "EndSponsoringFutureReservesResultCode";
template <>
inline constexpr char const* xdrJsonName<EnvelopeType> = "EnvelopeType";
template <> inline constexpr char const* xdrJsonName<Error> = "SError";
template <> inline constexpr char const* xdrJsonName<ErrorCode> = "ErrorCode";
template <>
inline constexpr char const* xdrJsonName<EvictionIterator> = "EvictionIterator";
template <>
inline constexpr char const* xdrJsonName<ExtendFootprintTTLOp> =
    "ExtendFootprintTtlOp";
template <>
inline constexpr char const* xdrJsonName<ExtendFootprintTTLResult> =
    "ExtendFootprintTtlResult";
template <>
inline constexpr char const* xdrJsonName<ExtendFootprintTTLResultCode> =
    "ExtendFootprintTtlResultCode";
template <>
inline constexpr char const* xdrJsonName<ExtensionPoint> = "ExtensionPoint";
template <>
inline constexpr char const* xdrJsonName<FeeBumpTransaction> =
    "FeeBumpTransaction";
template <>
inline constexpr char const* xdrJsonName<FeeBumpTransactionEnvelope> =
    "FeeBumpTransactionEnvelope";
template <>
inline constexpr char const* xdrJsonName<FloodAdvert> = "FloodAdvert";
template <>
inline constexpr char const* xdrJsonName<FloodDemand> = "FloodDemand";
template <>
inline constexpr char const* xdrJsonName<GeneralizedTransactionSet> =
    "GeneralizedTransactionSet";
template <> inline constexpr char const* xdrJsonName<Hash> = "Hash";
template <>
inline constexpr char const* xdrJsonName<HashIDPreimage> = "HashIdPreimage";
template <> inline constexpr char const* xdrJsonName<Hello> = "Hello";
template <>
inline constexpr char const* xdrJsonName<HmacSha256Key> = "HmacSha256Key";
template <>
inline constexpr char const* xdrJsonName<HmacSha256Mac> = "HmacSha256Mac";
template <>
inline constexpr char const* xdrJsonName<HostFunction> = "HostFunction";
template <>
inline constexpr char const* xdrJsonName<HostFunctionType> = "HostFunctionType";
template <>
inline constexpr char const* xdrJsonName<HotArchiveBucketEntry> =
    "HotArchiveBucketEntry";
template <>
inline constexpr char const* xdrJsonName<HotArchiveBucketEntryType> =
    "HotArchiveBucketEntryType";
template <> inline constexpr char const* xdrJsonName<IPAddrType> = "IpAddrType";
template <>
inline constexpr char const* xdrJsonName<InflationPayout> = "InflationPayout";
template <>
inline constexpr char const* xdrJsonName<InflationResult> = "InflationResult";
template <>
inline constexpr char const* xdrJsonName<InflationResultCode> =
    "InflationResultCode";
template <>
inline constexpr char const* xdrJsonName<InnerTransactionResult> =
    "InnerTransactionResult";
template <>
inline constexpr char const* xdrJsonName<InnerTransactionResultPair> =
    "InnerTransactionResultPair";
template <>
inline constexpr char const* xdrJsonName<Int128Parts> = "Int128Parts";
template <>
inline constexpr char const* xdrJsonName<Int256Parts> = "Int256Parts";
template <>
inline constexpr char const* xdrJsonName<InvokeContractArgs> =
    "InvokeContractArgs";
template <>
inline constexpr char const* xdrJsonName<InvokeHostFunctionOp> =
    "InvokeHostFunctionOp";
template <>
inline constexpr char const* xdrJsonName<InvokeHostFunctionResult> =
    "InvokeHostFunctionResult";
template <>
inline constexpr char const* xdrJsonName<InvokeHostFunctionResultCode> =
    "InvokeHostFunctionResultCode";
template <>
inline constexpr char const* xdrJsonName<InvokeHostFunctionSuccessPreImage> =
    "InvokeHostFunctionSuccessPreImage";
template <>
inline constexpr char const* xdrJsonName<LedgerBounds> = "LedgerBounds";
template <>
inline constexpr char const* xdrJsonName<LedgerCloseMeta> = "LedgerCloseMeta";
template <>
inline constexpr char const* xdrJsonName<LedgerCloseMetaBatch> =
    "LedgerCloseMetaBatch";
template <>
inline constexpr char const* xdrJsonName<LedgerCloseMetaExt> =
    "LedgerCloseMetaExt";
template <>
inline constexpr char const* xdrJsonName<LedgerCloseMetaExtV1> =
    "LedgerCloseMetaExtV1";
template <>
inline constexpr char const* xdrJsonName<LedgerCloseMetaV0> =
    "LedgerCloseMetaV0";
template <>
inline constexpr char const* xdrJsonName<LedgerCloseMetaV1> =
    "LedgerCloseMetaV1";
template <>
inline constexpr char const* xdrJsonName<LedgerCloseMetaV2> =
    "LedgerCloseMetaV2";
template <>
inline constexpr char const* xdrJsonName<LedgerCloseValueSignature> =
    "LedgerCloseValueSignature";
template <>
inline constexpr char const* xdrJsonName<LedgerEntry> = "LedgerEntry";
template <>
inline constexpr char const* xdrJsonName<LedgerEntryChange> =
    "LedgerEntryChange";
template <>
inline constexpr char const* xdrJsonName<LedgerEntryChangeType> =
    "LedgerEntryChangeType";
template <>
inline constexpr char const* xdrJsonName<LedgerEntryChanges> =
    "LedgerEntryChanges";
template <>
inline constexpr char const* xdrJsonName<LedgerEntryExtensionV1> =
    "LedgerEntryExtensionV1";
template <>
inline constexpr char const* xdrJsonName<LedgerEntryType> = "LedgerEntryType";
template <>
inline constexpr char const* xdrJsonName<LedgerFootprint> = "LedgerFootprint";
template <>
inline constexpr char const* xdrJsonName<LedgerHeader> = "LedgerHeader";
template <>
inline constexpr char const* xdrJsonName<LedgerHeaderExtensionV1> =
    "LedgerHeaderExtensionV1";
template <>
inline constexpr char const* xdrJsonName<LedgerHeaderFlags> =
    "LedgerHeaderFlags";
template <>
inline constexpr char const* xdrJsonName<LedgerHeaderHistoryEntry> =
    "LedgerHeaderHistoryEntry";
template <> inline constexpr char const* xdrJsonName<LedgerKey> = "LedgerKey";
template <>
inline constexpr char const* xdrJsonName<LedgerSCPMessages> =
    "LedgerScpMessages";
template <>
inline constexpr char const* xdrJsonName<LedgerUpgrade> = "LedgerUpgrade";
template <>
inline constexpr char const* xdrJsonName<LedgerUpgradeType> =
    "LedgerUpgradeType";
template <>
inline constexpr char const* xdrJsonName<Liabilities> = "Liabilities";
template <>
inline constexpr char const*
    xdrJsonName<LiquidityPoolConstantProductParameters> =
        "LiquidityPoolConstantProductParameters";
template <>
inline constexpr char const* xdrJsonName<LiquidityPoolDepositOp> =
    "LiquidityPoolDepositOp";
template <>
inline constexpr char const* xdrJsonName<LiquidityPoolDepositResult> =
    "LiquidityPoolDepositResult";
template <>
inline constexpr char const* xdrJsonName<LiquidityPoolDepositResultCode> =
    "LiquidityPoolDepositResultCode";
template <>
inline constexpr char const* xdrJsonName<LiquidityPoolEntry> =
    "LiquidityPoolEntry";
template <>
inline constexpr char const* xdrJsonName<LiquidityPoolParameters> =
    "LiquidityPoolParameters";
template <>
inline constexpr char const* xdrJsonName<LiquidityPoolType> =
    "LiquidityPoolType";
template <>
inline constexpr char const* xdrJsonName<LiquidityPoolWithdrawOp> =
    "LiquidityPoolWithdrawOp";
template <>
inline constexpr char const* xdrJsonName<LiquidityPoolWithdrawResult> =
    "LiquidityPoolWithdrawResult";
template <>
inline constexpr char const* xdrJsonName<LiquidityPoolWithdrawResultCode> =
    "LiquidityPoolWithdrawResultCode";
template <>
inline constexpr char const* xdrJsonName<ManageBuyOfferOp> = "ManageBuyOfferOp";
template <>
inline constexpr char const* xdrJsonName<ManageBuyOfferResult> =
    "ManageBuyOfferResult";
template <>
inline constexpr char const* xdrJsonName<ManageBuyOfferResultCode> =
    "ManageBuyOfferResultCode";
template <>
inline constexpr char const* xdrJsonName<ManageDataOp> = "ManageDataOp";
template <>
inline constexpr char const* xdrJsonName<ManageDataResult> = "ManageDataResult";
template <>
inline constexpr char const* xdrJsonName<ManageDataResultCode> =
    "ManageDataResultCode";
template <>
inline constexpr char const* xdrJsonName<ManageOfferEffect> =
    "ManageOfferEffect";
template <>
inline constexpr char const* xdrJsonName<ManageOfferSuccessResult> =
    "ManageOfferSuccessResult";
template <>
inline constexpr char const* xdrJsonName<ManageSellOfferOp> =
    "ManageSellOfferOp";
template <>
inline constexpr char const* xdrJsonName<ManageSellOfferResult> =
    "ManageSellOfferResult";
template <>
inline constexpr char const* xdrJsonName<ManageSellOfferResultCode> =
    "ManageSellOfferResultCode";
template <> inline constexpr char const* xdrJsonName<Memo> = "Memo";
template <> inline constexpr char const* xdrJsonName<MemoType> = "MemoType";
template <>
inline constexpr char const* xdrJsonName<MessageType> = "MessageType";
template <>
inline constexpr char const* xdrJsonName<MuxedAccount> = "MuxedAccount";
template <>
inline constexpr char const* xdrJsonName<MuxedEd25519Account> =
    "MuxedEd25519Account";
template <> inline constexpr char const* xdrJsonName<OfferEntry> = "OfferEntry";
template <>
inline constexpr char const* xdrJsonName<OfferEntryFlags> = "OfferEntryFlags";
template <> inline constexpr char const* xdrJsonName<Operation> = "Operation";
template <>
inline constexpr char const* xdrJsonName<OperationMeta> = "OperationMeta";
template <>
inline constexpr char const* xdrJsonName<OperationMetaV2> = "OperationMetaV2";
template <>
inline constexpr char const* xdrJsonName<OperationResult> = "OperationResult";
template <>
inline constexpr char const* xdrJsonName<OperationResultCode> =
    "OperationResultCode";
template <>
inline constexpr char const* xdrJsonName<OperationType> = "OperationType";
template <>
inline constexpr char const* xdrJsonName<ParallelTxExecutionStage> =
    "ParallelTxExecutionStage";
template <>
inline constexpr char const* xdrJsonName<ParallelTxsComponent> =
    "ParallelTxsComponent";
template <>
inline constexpr char const* xdrJsonName<PathPaymentStrictReceiveOp> =
    "PathPaymentStrictReceiveOp";
template <>
inline constexpr char const* xdrJsonName<PathPaymentStrictReceiveResult> =
    "PathPaymentStrictReceiveResult";
template <>
inline constexpr char const* xdrJsonName<PathPaymentStrictReceiveResultCode> =
    "PathPaymentStrictReceiveResultCode";
template <>
inline constexpr char const* xdrJsonName<PathPaymentStrictSendOp> =
    "PathPaymentStrictSendOp";
template <>
inline constexpr char const* xdrJsonName<PathPaymentStrictSendResult> =
    "PathPaymentStrictSendResult";
template <>
inline constexpr char const* xdrJsonName<PathPaymentStrictSendResultCode> =
    "PathPaymentStrictSendResultCode";
template <> inline constexpr char const* xdrJsonName<PaymentOp> = "PaymentOp";
template <>
inline constexpr char const* xdrJsonName<PaymentResult> = "PaymentResult";
template <>
inline constexpr char const* xdrJsonName<PaymentResultCode> =
    "PaymentResultCode";
template <>
inline constexpr char const* xdrJsonName<PeerAddress> = "PeerAddress";
template <> inline constexpr char const* xdrJsonName<PeerStats> = "PeerStats";
template <>
inline constexpr char const* xdrJsonName<PersistedSCPState> =
    "PersistedScpState";
template <>
inline constexpr char const* xdrJsonName<PersistedSCPStateV0> =
    "PersistedScpStateV0";
template <>
inline constexpr char const* xdrJsonName<PersistedSCPStateV1> =
    "PersistedScpStateV1";
template <>
inline constexpr char const* xdrJsonName<PreconditionType> = "PreconditionType";
template <>
inline constexpr char const* xdrJsonName<Preconditions> = "Preconditions";
template <>
inline constexpr char const* xdrJsonName<PreconditionsV2> = "PreconditionsV2";
template <> inline constexpr char const* xdrJsonName<Price> = "Price";
template <>
inline constexpr char const* xdrJsonName<PublicKeyType> = "PublicKeyType";
template <>
inline constexpr char const* xdrJsonName<RestoreFootprintOp> =
    "RestoreFootprintOp";
template <>
inline constexpr char const* xdrJsonName<RestoreFootprintResult> =
    "RestoreFootprintResult";
template <>
inline constexpr char const* xdrJsonName<RestoreFootprintResultCode> =
    "RestoreFootprintResultCode";
template <>
inline constexpr char const* xdrJsonName<RevokeSponsorshipOp> =
    "RevokeSponsorshipOp";
template <>
inline constexpr char const* xdrJsonName<RevokeSponsorshipResult> =
    "RevokeSponsorshipResult";
template <>
inline constexpr char const* xdrJsonName<RevokeSponsorshipResultCode> =
    "RevokeSponsorshipResultCode";
template <>
inline constexpr char const* xdrJsonName<RevokeSponsorshipType> =
    "RevokeSponsorshipType";
template <> inline constexpr char const* xdrJsonName<SCAddress> = "ScAddress";
template <>
inline constexpr char const* xdrJsonName<SCAddressType> = "ScAddressType";
template <> inline constexpr char const* xdrJsonName<SCBytes> = "ScBytes";
template <>
inline constexpr char const* xdrJsonName<SCContractInstance> =
    "ScContractInstance";
template <>
inline constexpr char const* xdrJsonName<SCEnvMetaEntry> = "ScEnvMetaEntry";
template <>
inline constexpr char const* xdrJsonName<SCEnvMetaKind> = "ScEnvMetaKind";
template <> inline constexpr char const* xdrJsonName<SCError> = "ScError";
template <>
inline constexpr char const* xdrJsonName<SCErrorCode> = "ScErrorCode";
template <>
inline constexpr char const* xdrJsonName<SCErrorType> = "ScErrorType";
template <> inline constexpr char const* xdrJsonName<SCMap> = "ScMap";
template <> inline constexpr char const* xdrJsonName<SCMapEntry> = "ScMapEntry";
template <> inline constexpr char const* xdrJsonName<SCNonceKey> = "ScNonceKey";
template <> inline constexpr char const* xdrJsonName<SCPBallot> = "ScpBallot";
template <>
inline constexpr char const* xdrJsonName<SCPEnvelope> = "ScpEnvelope";
template <>
inline constexpr char const* xdrJsonName<SCPHistoryEntry> = "ScpHistoryEntry";
template <>
inline constexpr char const* xdrJsonName<SCPHistoryEntryV0> =
    "ScpHistoryEntryV0";
template <>
inline constexpr char const* xdrJsonName<SCPNomination> = "ScpNomination";
template <>
inline constexpr char const* xdrJsonName<SCPQuorumSet> = "ScpQuorumSet";
template <>
inline constexpr char const* xdrJsonName<SCPStatement> = "ScpStatement";
template <>
inline constexpr char const* xdrJsonName<SCPStatementType> = "ScpStatementType";
template <> inline constexpr char const* xdrJsonName<SCString> = "ScString";
template <> inline constexpr char const* xdrJsonName<SCSymbol> = "ScSymbol";
template <> inline constexpr char const* xdrJsonName<SCVal> = "ScVal";
template <> inline constexpr char const* xdrJsonName<SCValType> = "ScValType";
template <> inline constexpr char const* xdrJsonName<SCVec> = "ScVec";
template <> inline constexpr char const* xdrJsonName<SendMore> = "SendMore";
template <>
inline constexpr char const* xdrJsonName<SendMoreExtended> = "SendMoreExtended";
template <>
inline constexpr char const* xdrJsonName<SequenceNumber> = "SequenceNumber";
template <>
inline constexpr char const* xdrJsonName<SerializedBinaryFuseFilter> =
    "SerializedBinaryFuseFilter";
template <>
inline constexpr char const* xdrJsonName<SetOptionsOp> = "SetOptionsOp";
template <>
inline constexpr char const* xdrJsonName<SetOptionsResult> = "SetOptionsResult";
template <>
inline constexpr char const* xdrJsonName<SetOptionsResultCode> =
    "SetOptionsResultCode";
template <>
inline constexpr char const* xdrJsonName<SetTrustLineFlagsOp> =
    "SetTrustLineFlagsOp";
template <>
inline constexpr char const* xdrJsonName<SetTrustLineFlagsResult> =
    "SetTrustLineFlagsResult";
template <>
inline constexpr char const* xdrJsonName<SetTrustLineFlagsResultCode> =
    "SetTrustLineFlagsResultCode";
template <>
inline constexpr char const* xdrJsonName<ShortHashSeed> = "ShortHashSeed";
template <>
inline constexpr char const* xdrJsonName<SignedTimeSlicedSurveyRequestMessage> =
    "SignedTimeSlicedSurveyRequestMessage";
template <>
inline constexpr char const*
    xdrJsonName<SignedTimeSlicedSurveyResponseMessage> =
        "SignedTimeSlicedSurveyResponseMessage";
template <>
inline constexpr char const*
    xdrJsonName<SignedTimeSlicedSurveyStartCollectingMessage> =
        "SignedTimeSlicedSurveyStartCollectingMessage";
template <>
inline constexpr char const*
    xdrJsonName<SignedTimeSlicedSurveyStopCollectingMessage> =
        "SignedTimeSlicedSurveyStopCollectingMessage";
template <> inline constexpr char const* xdrJsonName<Signer> = "Signer";
template <> inline constexpr char const* xdrJsonName<SignerKey> = "SignerKey";
template <>
inline constexpr char const* xdrJsonName<SignerKeyType> = "SignerKeyType";
template <>
inline constexpr char const* xdrJsonName<SimplePaymentResult> =
    "SimplePaymentResult";
template <>
inline constexpr char const* xdrJsonName<SorobanAddressCredentials> =
    "SorobanAddressCredentials";
template <>
inline constexpr char const* xdrJsonName<SorobanAuthorizationEntries> =
    "SorobanAuthorizationEntries";
template <>
inline constexpr char const* xdrJsonName<SorobanAuthorizationEntry> =
    "SorobanAuthorizationEntry";
template <>
inline constexpr char const* xdrJsonName<SorobanAuthorizedFunction> =
    "SorobanAuthorizedFunction";
template <>
inline constexpr char const* xdrJsonName<SorobanAuthorizedFunctionType> =
    "SorobanAuthorizedFunctionType";
template <>
inline constexpr char const* xdrJsonName<SorobanAuthorizedInvocation> =
    "SorobanAuthorizedInvocation";
template <>
inline constexpr char const* xdrJsonName<SorobanCredentials> =
    "SorobanCredentials";
template <>
inline constexpr char const* xdrJsonName<SorobanCredentialsType> =
    "SorobanCredentialsType";
template <>
inline constexpr char const* xdrJsonName<SorobanResources> = "SorobanResources";
template <>
inline constexpr char const* xdrJsonName<SorobanResourcesExtV0> =
    "SorobanResourcesExtV0";
template <>
inline constexpr char const* xdrJsonName<SorobanTransactionData> =
    "SorobanTransactionData";
template <>
inline constexpr char const* xdrJsonName<SorobanTransactionMeta> =
    "SorobanTransactionMeta";
template <>
inline constexpr char const* xdrJsonName<SorobanTransactionMetaExt> =
    "SorobanTransactionMetaExt";
template <>
inline constexpr char const* xdrJsonName<SorobanTransactionMetaExtV1> =
    "SorobanTransactionMetaExtV1";
template <>
inline constexpr char const* xdrJsonName<SorobanTransactionMetaV2> =
    "SorobanTransactionMetaV2";
template <>
inline constexpr char const* xdrJsonName<SponsorshipDescriptor> =
    "SponsorshipDescriptor";
template <>
inline constexpr char const* xdrJsonName<StateArchivalSettings> =
    "StateArchivalSettings";
template <>
inline constexpr char const* xdrJsonName<StellarMessage> = "StellarMessage";
template <>
inline constexpr char const* xdrJsonName<StellarValue> = "StellarValue";
template <>
inline constexpr char const* xdrJsonName<StellarValueType> = "StellarValueType";
template <>
inline constexpr char const* xdrJsonName<StoredDebugTransactionSet> =
    "StoredDebugTransactionSet";
template <>
inline constexpr char const* xdrJsonName<StoredTransactionSet> =
    "StoredTransactionSet";
template <>
inline constexpr char const* xdrJsonName<SurveyMessageCommandType> =
    "SurveyMessageCommandType";
template <>
inline constexpr char const* xdrJsonName<SurveyMessageResponseType> =
    "SurveyMessageResponseType";
template <>
inline constexpr char const* xdrJsonName<SurveyRequestMessage> =
    "SurveyRequestMessage";
template <>
inline constexpr char const* xdrJsonName<SurveyResponseBody> =
    "SurveyResponseBody";
template <>
inline constexpr char const* xdrJsonName<SurveyResponseMessage> =
    "SurveyResponseMessage";
template <> inline constexpr char const* xdrJsonName<TTLEntry> = "TtlEntry";
template <>
inline constexpr char const* xdrJsonName<ThresholdIndexes> = "ThresholdIndexes";
template <> inline constexpr char const* xdrJsonName<TimeBounds> = "TimeBounds";
template <>
inline constexpr char const* xdrJsonName<TimeSlicedNodeData> =
    "TimeSlicedNodeData";
template <>
inline constexpr char const* xdrJsonName<TimeSlicedPeerData> =
    "TimeSlicedPeerData";
template <>
inline constexpr char const* xdrJsonName<TimeSlicedPeerDataList> =
    "TimeSlicedPeerDataList";
template <>
inline constexpr char const* xdrJsonName<TimeSlicedSurveyRequestMessage> =
    "TimeSlicedSurveyRequestMessage";
template <>
inline constexpr char const* xdrJsonName<TimeSlicedSurveyResponseMessage> =
    "TimeSlicedSurveyResponseMessage";
template <>
inline constexpr char const*
    xdrJsonName<TimeSlicedSurveyStartCollectingMessage> =
        "TimeSlicedSurveyStartCollectingMessage";
template <>
inline constexpr char const*
    xdrJsonName<TimeSlicedSurveyStopCollectingMessage> =
        "TimeSlicedSurveyStopCollectingMessage";
template <>
inline constexpr char const* xdrJsonName<TopologyResponseBodyV2> =
    "TopologyResponseBodyV2";
template <>
inline constexpr char const* xdrJsonName<Transaction> = "Transaction";
template <>
inline constexpr char const* xdrJsonName<TransactionEnvelope> =
    "TransactionEnvelope";
template <>
inline constexpr char const* xdrJsonName<TransactionEvent> = "TransactionEvent";
template <>
inline constexpr char const* xdrJsonName<TransactionEventStage> =
    "TransactionEventStage";
template <>
inline constexpr char const* xdrJsonName<TransactionHistoryEntry> =
    "TransactionHistoryEntry";
template <>
inline constexpr char const* xdrJsonName<TransactionHistoryResultEntry> =
    "TransactionHistoryResultEntry";
template <>
inline constexpr char const* xdrJsonName<TransactionMeta> = "TransactionMeta";
template <>
inline constexpr char const* xdrJsonName<TransactionMetaV1> =
    "TransactionMetaV1";
template <>
inline constexpr char const* xdrJsonName<TransactionMetaV2> =
    "TransactionMetaV2";
template <>
inline constexpr char const* xdrJsonName<TransactionMetaV3> =
    "TransactionMetaV3";
template <>
inline constexpr char const* xdrJsonName<TransactionMetaV4> =
    "TransactionMetaV4";
template <>
inline constexpr char const* xdrJsonName<TransactionPhase> = "TransactionPhase";
template <>
inline constexpr char const* xdrJsonName<TransactionResult> =
    "TransactionResult";
template <>
inline constexpr char const* xdrJsonName<TransactionResultCode> =
    "TransactionResultCode";
template <>
inline constexpr char const* xdrJsonName<TransactionResultMeta> =
    "TransactionResultMeta";
template <>
inline constexpr char const* xdrJsonName<TransactionResultMetaV1> =
    "TransactionResultMetaV1";
template <>
inline constexpr char const* xdrJsonName<TransactionResultPair> =
    "TransactionResultPair";
template <>
inline constexpr char const* xdrJsonName<TransactionResultSet> =
    "TransactionResultSet";
template <>
inline constexpr char const* xdrJsonName<TransactionSet> = "TransactionSet";
template <>
inline constexpr char const* xdrJsonName<TransactionSetV1> = "TransactionSetV1";
template <>
inline constexpr char const* xdrJsonName<TransactionSignaturePayload> =
    "TransactionSignaturePayload";
template <>
inline constexpr char const* xdrJsonName<TransactionV0> = "TransactionV0";
template <>
inline constexpr char const* xdrJsonName<TransactionV0Envelope> =
    "TransactionV0Envelope";
template <>
inline constexpr char const* xdrJsonName<TransactionV1Envelope> =
    "TransactionV1Envelope";
template <>
inline constexpr char const* xdrJsonName<TrustLineAsset> = "TrustLineAsset";
template <>
inline constexpr char const* xdrJsonName<TrustLineEntry> = "TrustLineEntry";
template <>
inline constexpr char const* xdrJsonName<TrustLineEntryExtensionV2> =
    "TrustLineEntryExtensionV2";
template <>
inline constexpr char const* xdrJsonName<TrustLineFlags> = "TrustLineFlags";
template <>
inline constexpr char const* xdrJsonName<TxAdvertVector> = "TxAdvertVector";
template <>
inline constexpr char const* xdrJsonName<TxSetComponent> = "TxSetComponent";
template <>
inline constexpr char const* xdrJsonName<TxSetComponentType> =
    "TxSetComponentType";
template <>
inline constexpr char const* xdrJsonName<UInt128Parts> = "UInt128Parts";
template <>
inline constexpr char const* xdrJsonName<UInt256Parts> = "UInt256Parts";
template <>
inline constexpr char const* xdrJsonName<UpgradeEntryMeta> = "UpgradeEntryMeta";
template <>
inline constexpr char const* xdrJsonName<UpgradeType> = "UpgradeType";

template <>
inline constexpr char const* xdrJsonName<LedgerEntry::_data_t> =
    "LedgerEntryData";
template <>
inline constexpr char const* xdrJsonName<OperationResult::_tr_t> =
    "OperationResultTr";
}
