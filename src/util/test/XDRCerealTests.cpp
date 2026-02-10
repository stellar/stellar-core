#include "crypto/StrKey.h"
#include "test/Catch2.h"
#include "util/Decoder.h"
#include <fmt/format.h>
#include <xdrpp/autocheck.h>

namespace
{

template <typename T>
bool
roundtrip(const T& a)
{
    std::stringstream ss;
    T b;
    {
        cereal::JSONOutputArchive ar{ss};
        xdr::archive(ar, a);
    }
    cereal::JSONInputArchive ar{ss};
    xdr::archive(ar, b);
    return a == b;
}

// We make our own generator for XDR types instead of using the overload for
// autocheck::generator defined in <xdrpp/autocheck.h> so that we can set the
// `levels` value to 1.
template <typename T> struct XDRGenerator
{
    using result_type = T;

    result_type
    operator()(size_t size) const
    {
        xdr::generator_t g(size, 1);
        T t;
        xdr::archive(g, t);
        return t;
    }
};

template <typename T, typename = void> struct Generator
{
    using type = autocheck::generator<T>;
};

template <typename T>
struct Generator<T, std::enable_if_t<xdr::xdr_traits<T>::valid &&
                                     !xdr::xdr_traits<T>::is_numeric>>
{
    using type = XDRGenerator<T>;
};

template <typename T>
bool
roundtripRandoms()
{
    autocheck::arbitrary<typename Generator<T>::type> gen;
    autocheck::value<std::tuple<T>> arg;
    gen.resize([](size_t s) { return std::min(s, static_cast<size_t>(10)); });

    for (int i = 0; i < 1000; i++)
    {
        gen(arg);
        auto& actual = std::get<0>(arg.ref());
        if (!roundtrip(actual))
        {
            std::cerr << "Failed for XDR #" << i << ": "
                      << stellar::decoder::encode_b64(
                             xdr::xdr_to_opaque(actual))
                      << "\n";
            return false;
        }
    }
    return true;
}

#define ROUNDTRIP(name) \
    TEST_CASE("XDRCereal overrides can roundtrip " #name, \
              "[xdr cereal roundtrip][acceptance]") \
    { \
        using namespace stellar; \
        REQUIRE(roundtripRandoms<name>()); \
    }

ROUNDTRIP(bool)
ROUNDTRIP(xdr::pointer<xdr::pointer<int>>)

#ifdef TEST_XDR_ROUNDTRIPS
ROUNDTRIP(AccountEntry)
ROUNDTRIP(AccountEntryExtensionV1)
ROUNDTRIP(AccountEntryExtensionV2)
ROUNDTRIP(AccountEntryExtensionV3)
ROUNDTRIP(AccountFlags)
ROUNDTRIP(AccountID)
ROUNDTRIP(AccountMergeResult)
ROUNDTRIP(AccountMergeResultCode)
ROUNDTRIP(AllowTrustOp)
ROUNDTRIP(AllowTrustResult)
ROUNDTRIP(AllowTrustResultCode)
ROUNDTRIP(AlphaNum12)
ROUNDTRIP(AlphaNum4)
ROUNDTRIP(Asset)
ROUNDTRIP(AssetCode)
ROUNDTRIP(AssetCode12)
ROUNDTRIP(AssetCode4)
ROUNDTRIP(AssetType)
ROUNDTRIP(Auth)
ROUNDTRIP(AuthCert)
ROUNDTRIP(AuthenticatedMessage)
ROUNDTRIP(BeginSponsoringFutureReservesOp)
ROUNDTRIP(BeginSponsoringFutureReservesResult)
ROUNDTRIP(BeginSponsoringFutureReservesResultCode)
ROUNDTRIP(BinaryFuseFilterType)
ROUNDTRIP(BucketEntry)
ROUNDTRIP(BucketEntryType)
ROUNDTRIP(BucketListType)
ROUNDTRIP(BucketMetadata)
ROUNDTRIP(BumpSequenceOp)
ROUNDTRIP(BumpSequenceResult)
ROUNDTRIP(BumpSequenceResultCode)
ROUNDTRIP(ChangeTrustAsset)
ROUNDTRIP(ChangeTrustOp)
ROUNDTRIP(ChangeTrustResult)
ROUNDTRIP(ChangeTrustResultCode)
ROUNDTRIP(ClaimAtom)
ROUNDTRIP(ClaimAtomType)
ROUNDTRIP(ClaimClaimableBalanceOp)
ROUNDTRIP(ClaimClaimableBalanceResult)
ROUNDTRIP(ClaimClaimableBalanceResultCode)
ROUNDTRIP(ClaimLiquidityAtom)
ROUNDTRIP(ClaimOfferAtom)
ROUNDTRIP(ClaimOfferAtomV0)
ROUNDTRIP(ClaimPredicate)
ROUNDTRIP(ClaimPredicateType)
ROUNDTRIP(ClaimableBalanceEntry)
ROUNDTRIP(ClaimableBalanceEntryExtensionV1)
ROUNDTRIP(ClaimableBalanceFlags)
ROUNDTRIP(ClaimableBalanceID)
ROUNDTRIP(ClaimableBalanceIDType)
ROUNDTRIP(Claimant)
ROUNDTRIP(ClaimantType)
ROUNDTRIP(ClawbackClaimableBalanceOp)
ROUNDTRIP(ClawbackClaimableBalanceResult)
ROUNDTRIP(ClawbackClaimableBalanceResultCode)
ROUNDTRIP(ClawbackOp)
ROUNDTRIP(ClawbackResult)
ROUNDTRIP(ClawbackResultCode)
ROUNDTRIP(ConfigSettingContractBandwidthV0)
ROUNDTRIP(ConfigSettingContractComputeV0)
ROUNDTRIP(ConfigSettingContractEventsV0)
ROUNDTRIP(ConfigSettingContractExecutionLanesV0)
ROUNDTRIP(ConfigSettingContractHistoricalDataV0)
ROUNDTRIP(ConfigSettingContractLedgerCostExtV0)
ROUNDTRIP(ConfigSettingContractLedgerCostV0)
ROUNDTRIP(ConfigSettingContractParallelComputeV0)
ROUNDTRIP(ConfigSettingEntry)
ROUNDTRIP(ConfigSettingID)
ROUNDTRIP(ConfigSettingSCPTiming)
ROUNDTRIP(ConfigUpgradeSet)
ROUNDTRIP(ConfigUpgradeSetKey)
ROUNDTRIP(ContractCodeCostInputs)
ROUNDTRIP(ContractCodeEntry)
ROUNDTRIP(ContractCostParamEntry)
ROUNDTRIP(ContractCostParams)
ROUNDTRIP(ContractCostType)
ROUNDTRIP(ContractDataDurability)
ROUNDTRIP(ContractDataEntry)
ROUNDTRIP(ContractEvent)
ROUNDTRIP(ContractEventType)
ROUNDTRIP(ContractExecutable)
ROUNDTRIP(ContractExecutableType)
ROUNDTRIP(ContractIDPreimage)
ROUNDTRIP(ContractIDPreimageType)
ROUNDTRIP(CreateAccountOp)
ROUNDTRIP(CreateAccountResult)
ROUNDTRIP(CreateAccountResultCode)
ROUNDTRIP(CreateClaimableBalanceOp)
ROUNDTRIP(CreateClaimableBalanceResult)
ROUNDTRIP(CreateClaimableBalanceResultCode)
ROUNDTRIP(CreateContractArgs)
ROUNDTRIP(CreateContractArgsV2)
ROUNDTRIP(CreatePassiveSellOfferOp)
ROUNDTRIP(CryptoKeyType)
ROUNDTRIP(Curve25519Public)
ROUNDTRIP(Curve25519Secret)
ROUNDTRIP(DataEntry)
ROUNDTRIP(DataValue)
ROUNDTRIP(DecoratedSignature)
ROUNDTRIP(DependentTxCluster)
ROUNDTRIP(DiagnosticEvent)
ROUNDTRIP(DontHave)
ROUNDTRIP(Duration)
ROUNDTRIP(EncryptedBody)
ROUNDTRIP(EndSponsoringFutureReservesResult)
ROUNDTRIP(EndSponsoringFutureReservesResultCode)
ROUNDTRIP(EnvelopeType)
ROUNDTRIP(Error)
ROUNDTRIP(ErrorCode)
ROUNDTRIP(EvictionIterator)
ROUNDTRIP(ExtendFootprintTTLOp)
ROUNDTRIP(ExtendFootprintTTLResult)
ROUNDTRIP(ExtendFootprintTTLResultCode)
ROUNDTRIP(ExtensionPoint)
ROUNDTRIP(FeeBumpTransaction)
ROUNDTRIP(FeeBumpTransactionEnvelope)
ROUNDTRIP(FloodAdvert)
ROUNDTRIP(FloodDemand)
ROUNDTRIP(GeneralizedTransactionSet)
ROUNDTRIP(Hash)
ROUNDTRIP(HashIDPreimage)
ROUNDTRIP(Hello)
ROUNDTRIP(HmacSha256Key)
ROUNDTRIP(HmacSha256Mac)
ROUNDTRIP(HostFunction)
ROUNDTRIP(HostFunctionType)
ROUNDTRIP(HotArchiveBucketEntry)
ROUNDTRIP(HotArchiveBucketEntryType)
ROUNDTRIP(IPAddrType)
ROUNDTRIP(InflationPayout)
ROUNDTRIP(InflationResult)
ROUNDTRIP(InflationResultCode)
ROUNDTRIP(InnerTransactionResult)
ROUNDTRIP(InnerTransactionResultPair)
ROUNDTRIP(Int128Parts)
ROUNDTRIP(Int256Parts)
ROUNDTRIP(InvokeContractArgs)
ROUNDTRIP(InvokeHostFunctionOp)
ROUNDTRIP(InvokeHostFunctionResult)
ROUNDTRIP(InvokeHostFunctionResultCode)
ROUNDTRIP(InvokeHostFunctionSuccessPreImage)
ROUNDTRIP(LedgerBounds)
ROUNDTRIP(LedgerCloseMeta)
ROUNDTRIP(LedgerCloseMetaExt)
ROUNDTRIP(LedgerCloseMetaExtV1)
ROUNDTRIP(LedgerCloseMetaV0)
ROUNDTRIP(LedgerCloseMetaV1)
ROUNDTRIP(LedgerCloseMetaV2)
ROUNDTRIP(LedgerCloseValueSignature)
ROUNDTRIP(LedgerEntry)
ROUNDTRIP(LedgerEntryChange)
ROUNDTRIP(LedgerEntryChangeType)
ROUNDTRIP(LedgerEntryChanges)
ROUNDTRIP(LedgerEntryExtensionV1)
ROUNDTRIP(LedgerEntryType)
ROUNDTRIP(LedgerFootprint)
ROUNDTRIP(LedgerHeader)
ROUNDTRIP(LedgerHeaderExtensionV1)
ROUNDTRIP(LedgerHeaderFlags)
ROUNDTRIP(LedgerHeaderHistoryEntry)
ROUNDTRIP(LedgerKey)
ROUNDTRIP(LedgerSCPMessages)
ROUNDTRIP(LedgerUpgrade)
ROUNDTRIP(LedgerUpgradeType)
ROUNDTRIP(Liabilities)
ROUNDTRIP(LiquidityPoolConstantProductParameters)
ROUNDTRIP(LiquidityPoolDepositOp)
ROUNDTRIP(LiquidityPoolDepositResult)
ROUNDTRIP(LiquidityPoolDepositResultCode)
ROUNDTRIP(LiquidityPoolEntry)
ROUNDTRIP(LiquidityPoolParameters)
ROUNDTRIP(LiquidityPoolType)
ROUNDTRIP(LiquidityPoolWithdrawOp)
ROUNDTRIP(LiquidityPoolWithdrawResult)
ROUNDTRIP(LiquidityPoolWithdrawResultCode)
ROUNDTRIP(ManageBuyOfferOp)
ROUNDTRIP(ManageBuyOfferResult)
ROUNDTRIP(ManageBuyOfferResultCode)
ROUNDTRIP(ManageDataOp)
ROUNDTRIP(ManageDataResult)
ROUNDTRIP(ManageDataResultCode)
ROUNDTRIP(ManageOfferEffect)
ROUNDTRIP(ManageOfferSuccessResult)
ROUNDTRIP(ManageSellOfferOp)
ROUNDTRIP(ManageSellOfferResult)
ROUNDTRIP(ManageSellOfferResultCode)
ROUNDTRIP(Memo)
ROUNDTRIP(MemoType)
ROUNDTRIP(MessageType)
ROUNDTRIP(MuxedAccount)
ROUNDTRIP(MuxedEd25519Account)
ROUNDTRIP(OfferEntry)
ROUNDTRIP(OfferEntryFlags)
ROUNDTRIP(Operation)
ROUNDTRIP(OperationMeta)
ROUNDTRIP(OperationMetaV2)
ROUNDTRIP(OperationResult)
ROUNDTRIP(OperationResultCode)
ROUNDTRIP(OperationType)
ROUNDTRIP(ParallelTxExecutionStage)
ROUNDTRIP(ParallelTxsComponent)
ROUNDTRIP(PathPaymentStrictReceiveOp)
ROUNDTRIP(PathPaymentStrictReceiveResult)
ROUNDTRIP(PathPaymentStrictReceiveResultCode)
ROUNDTRIP(PathPaymentStrictSendOp)
ROUNDTRIP(PathPaymentStrictSendResult)
ROUNDTRIP(PathPaymentStrictSendResultCode)
ROUNDTRIP(PaymentOp)
ROUNDTRIP(PaymentResult)
ROUNDTRIP(PaymentResultCode)
ROUNDTRIP(PeerAddress)
ROUNDTRIP(PeerStats)
ROUNDTRIP(PreconditionType)
ROUNDTRIP(Preconditions)
ROUNDTRIP(PreconditionsV2)
ROUNDTRIP(Price)
ROUNDTRIP(PublicKeyType)
ROUNDTRIP(RestoreFootprintOp)
ROUNDTRIP(RestoreFootprintResult)
ROUNDTRIP(RestoreFootprintResultCode)
ROUNDTRIP(RevokeSponsorshipOp)
ROUNDTRIP(RevokeSponsorshipResult)
ROUNDTRIP(RevokeSponsorshipResultCode)
ROUNDTRIP(RevokeSponsorshipType)
ROUNDTRIP(SCAddress)
ROUNDTRIP(SCAddressType)
ROUNDTRIP(SCBytes)
ROUNDTRIP(SCContractInstance)
ROUNDTRIP(SCError)
ROUNDTRIP(SCErrorCode)
ROUNDTRIP(SCErrorType)
ROUNDTRIP(SCMap)
ROUNDTRIP(SCMapEntry)
ROUNDTRIP(SCNonceKey)
ROUNDTRIP(SCPBallot)
ROUNDTRIP(SCPEnvelope)
ROUNDTRIP(SCPHistoryEntry)
ROUNDTRIP(SCPHistoryEntryV0)
ROUNDTRIP(SCPNomination)
ROUNDTRIP(SCPQuorumSet)
ROUNDTRIP(SCPStatement)
ROUNDTRIP(SCPStatementType)
// Note SCString and SCSymbol aren't listed because their overloads are
// ambiguous
ROUNDTRIP(SCVal)
ROUNDTRIP(SCValType)
ROUNDTRIP(SCVec)
ROUNDTRIP(SendMore)
ROUNDTRIP(SendMoreExtended)
ROUNDTRIP(SequenceNumber)
ROUNDTRIP(SerializedBinaryFuseFilter)
ROUNDTRIP(SetOptionsOp)
ROUNDTRIP(SetOptionsResult)
ROUNDTRIP(SetOptionsResultCode)
ROUNDTRIP(SetTrustLineFlagsOp)
ROUNDTRIP(SetTrustLineFlagsResult)
ROUNDTRIP(SetTrustLineFlagsResultCode)
ROUNDTRIP(ShortHashSeed)
ROUNDTRIP(SignedTimeSlicedSurveyRequestMessage)
ROUNDTRIP(SignedTimeSlicedSurveyResponseMessage)
ROUNDTRIP(SignedTimeSlicedSurveyStartCollectingMessage)
ROUNDTRIP(SignedTimeSlicedSurveyStopCollectingMessage)
ROUNDTRIP(Signer)
ROUNDTRIP(SignerKey)
ROUNDTRIP(SignerKeyType)
ROUNDTRIP(SimplePaymentResult)
ROUNDTRIP(SorobanAddressCredentials)
ROUNDTRIP(SorobanAuthorizationEntries)
ROUNDTRIP(SorobanAuthorizationEntry)
ROUNDTRIP(SorobanAuthorizedFunction)
ROUNDTRIP(SorobanAuthorizedFunctionType)
ROUNDTRIP(SorobanAuthorizedInvocation)
ROUNDTRIP(SorobanCredentials)
ROUNDTRIP(SorobanCredentialsType)
ROUNDTRIP(SorobanResources)
ROUNDTRIP(SorobanResourcesExtV0)
ROUNDTRIP(SorobanTransactionData)
ROUNDTRIP(SorobanTransactionMeta)
ROUNDTRIP(SorobanTransactionMetaExt)
ROUNDTRIP(SorobanTransactionMetaExtV1)
ROUNDTRIP(SorobanTransactionMetaV2)
ROUNDTRIP(SponsorshipDescriptor)
ROUNDTRIP(StateArchivalSettings)
ROUNDTRIP(StellarMessage)
ROUNDTRIP(StellarValue)
ROUNDTRIP(StellarValueType)
ROUNDTRIP(SurveyMessageCommandType)
ROUNDTRIP(SurveyMessageResponseType)
ROUNDTRIP(SurveyRequestMessage)
ROUNDTRIP(SurveyResponseBody)
ROUNDTRIP(SurveyResponseMessage)
ROUNDTRIP(TTLEntry)
ROUNDTRIP(ThresholdIndexes)
ROUNDTRIP(TimeBounds)
ROUNDTRIP(TimeSlicedNodeData)
ROUNDTRIP(TimeSlicedPeerData)
ROUNDTRIP(TimeSlicedPeerDataList)
ROUNDTRIP(TimeSlicedSurveyRequestMessage)
ROUNDTRIP(TimeSlicedSurveyResponseMessage)
ROUNDTRIP(TimeSlicedSurveyStartCollectingMessage)
ROUNDTRIP(TimeSlicedSurveyStopCollectingMessage)
ROUNDTRIP(TopologyResponseBodyV2)
ROUNDTRIP(Transaction)
ROUNDTRIP(TransactionEnvelope)
ROUNDTRIP(TransactionEvent)
ROUNDTRIP(TransactionEventStage)
ROUNDTRIP(TransactionHistoryEntry)
ROUNDTRIP(TransactionHistoryResultEntry)
ROUNDTRIP(TransactionMeta)
ROUNDTRIP(TransactionMetaV1)
ROUNDTRIP(TransactionMetaV2)
ROUNDTRIP(TransactionMetaV3)
ROUNDTRIP(TransactionMetaV4)
ROUNDTRIP(TransactionPhase)
ROUNDTRIP(TransactionResult)
ROUNDTRIP(TransactionResultCode)
ROUNDTRIP(TransactionResultMeta)
ROUNDTRIP(TransactionResultMetaV1)
ROUNDTRIP(TransactionResultPair)
ROUNDTRIP(TransactionResultSet)
ROUNDTRIP(TransactionSet)
ROUNDTRIP(TransactionSetV1)
ROUNDTRIP(TransactionSignaturePayload)
ROUNDTRIP(TransactionV0)
ROUNDTRIP(TransactionV0Envelope)
ROUNDTRIP(TransactionV1Envelope)
ROUNDTRIP(TrustLineAsset)
ROUNDTRIP(TrustLineEntry)
ROUNDTRIP(TrustLineEntryExtensionV2)
ROUNDTRIP(TrustLineFlags)
ROUNDTRIP(TxAdvertVector)
ROUNDTRIP(TxSetComponent)
ROUNDTRIP(TxSetComponentType)
ROUNDTRIP(UInt128Parts)
ROUNDTRIP(UInt256Parts)
ROUNDTRIP(UpgradeEntryMeta)
ROUNDTRIP(UpgradeType)
#endif

template <typename T>
std::string
toCerealCompact(T const& t, std::string const& name)
{
    using stellar::xdrToCerealString;
    using namespace rapidjson;
    std::string result = xdrToCerealString(t, name);
    Document d;
    d.Parse(result.c_str());

    StringBuffer sb;
    Writer<StringBuffer> w(sb);
    d.Accept(w);

    return sb.GetString();
}

template <uint32_t N>
void
copyHexToArray(xdr::opaque_array<N>& arr, char const* hex)
{
    auto bin = stellar::hexToBin(hex);
    releaseAssert(bin.size() <= N);
    std::copy(bin.begin(), bin.end(), arr.begin());
}

template <uint32_t N>
void
copyToArray(xdr::opaque_array<N>& arr, std::string const& s)
{
    releaseAssert(s.size() <= N);
    std::copy(s.begin(), s.end(), arr.begin());
}

TEST_CASE("XDRCereal overrides")
{
    using namespace stellar;
    char const accountIdHex[] =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    static_assert(sizeof(accountIdHex) ==
                  uint256::container_fixed_nelem * 2 + 1);
    static_assert(std::is_same_v<uint256, Hash>);

    // Check xstring
    {
        REQUIRE(toCerealCompact(xdr::xstring<5>{"abc"}, "s") ==
                R"({"s":"abc"})");
        xdr::xstring<5> s{std::string{'a', '\0', 'b', 'c'}};
        REQUIRE(toCerealCompact(s, "s") == R"({"s":{"raw":"61006263"}})");
    }

    // Check opaque array
    {
        xdr::opaque_array<8> o;
        copyHexToArray(o, "0123456789abcdef");
        REQUIRE(toCerealCompact(o, "o") == R"({"o":"0123456789abcdef"})");
    }

    // Check container (xdr::xvector)
    {
        xdr::xvector<uint32_t> v{1, 2, 3};
        CHECK(toCerealCompact(v, "v") == R"({"v":[1,2,3]})");
    }

    // Check container with nested override (xdr::xvector of enums)
    {
        xdr::xvector<Asset> v;
        Asset a;
        a.type(ASSET_TYPE_NATIVE);
        v.push_back(a);
        a.type(ASSET_TYPE_CREDIT_ALPHANUM4);
        copyToArray(a.alphaNum4().assetCode, "USD");
        copyHexToArray(a.alphaNum4().issuer.ed25519(), accountIdHex);
        v.push_back(a);
        CHECK(
            toCerealCompact(v, "v") ==
            R"({"v":["NATIVE",{"assetCode":"USD","issuer":"GAASGRLHRGV433YBENCWPCNLZXXQCI2FM6E2XTPPAERUKZ4JVPG66OUL"}]})");
    }

    // Check opaque_vec
    {
        xdr::opaque_vec<> v{0x01, 0x23, 0x45};
        CHECK(toCerealCompact(v, "v") == R"({"v":"012345"})");
    }

    // Check PublicKey
    {
        PublicKey pk;
        pk.type(PUBLIC_KEY_TYPE_ED25519);
        copyHexToArray(pk.ed25519(), accountIdHex);
        CHECK(
            toCerealCompact(pk, "pk") ==
            R"({"pk":"GAASGRLHRGV433YBENCWPCNLZXXQCI2FM6E2XTPPAERUKZ4JVPG66OUL"})");
    }

    // Check SCAddress (account)
    {
        SCAddress addr;
        addr.type(SC_ADDRESS_TYPE_ACCOUNT);
        addr.accountId().type(PUBLIC_KEY_TYPE_ED25519);
        copyHexToArray(addr.accountId().ed25519(), accountIdHex);
        CHECK(
            toCerealCompact(addr, "a") ==
            R"({"a":"GAASGRLHRGV433YBENCWPCNLZXXQCI2FM6E2XTPPAERUKZ4JVPG66OUL"})");
    }

    // Check SCAddress (contract)
    {
        SCAddress addr;
        addr.type(SC_ADDRESS_TYPE_CONTRACT);
        copyHexToArray(addr.contractId(), accountIdHex);
        CHECK(
            toCerealCompact(addr, "a") ==
            R"({"a":"CAASGRLHRGV433YBENCWPCNLZXXQCI2FM6E2XTPPAERUKZ4JVPG67KRS"})");
    }

    // Check ConfigUpgradeSetKey
    {
        ConfigUpgradeSetKey key;
        copyHexToArray(key.contentHash, accountIdHex);
        copyHexToArray(key.contractID, accountIdHex);
        CHECK(
            toCerealCompact(key, "k") ==
            R"({"k":{"contractID":"CAASGRLHRGV433YBENCWPCNLZXXQCI2FM6E2XTPPAERUKZ4JVPG67KRS","contentHash":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"}})");
    }

    // Check MuxedAccount (ED25519)
    {
        MuxedAccount ma;
        ma.type(KEY_TYPE_ED25519);
        copyHexToArray(ma.ed25519(), accountIdHex);
        CHECK(
            toCerealCompact(ma, "m") ==
            R"({"m":"GAASGRLHRGV433YBENCWPCNLZXXQCI2FM6E2XTPPAERUKZ4JVPG66OUL"})");
    }

    // Check MuxedAccount (MUXED_ED25519)
    {
        MuxedAccount ma;
        ma.type(KEY_TYPE_MUXED_ED25519);
        ma.med25519().id = 12345;
        copyHexToArray(ma.med25519().ed25519, accountIdHex);
        CHECK(
            toCerealCompact(ma, "m") ==
            R"({"m":{"id":12345,"accountID":"GAASGRLHRGV433YBENCWPCNLZXXQCI2FM6E2XTPPAERUKZ4JVPG66OUL"}})");
    }

    // Check Asset (NATIVE)
    {
        Asset a;
        a.type(ASSET_TYPE_NATIVE);
        CHECK(toCerealCompact(a, "a") == R"({"a":"NATIVE"})");
    }

    // Check Asset (CREDIT_ALPHANUM4, valid code)
    {
        Asset a;
        a.type(ASSET_TYPE_CREDIT_ALPHANUM4);
        copyToArray(a.alphaNum4().assetCode, "USD");
        a.alphaNum4().issuer.type(PUBLIC_KEY_TYPE_ED25519);
        copyHexToArray(a.alphaNum4().issuer.ed25519(), accountIdHex);
        CHECK(
            toCerealCompact(a, "a") ==
            R"({"a":{"assetCode":"USD","issuer":"GAASGRLHRGV433YBENCWPCNLZXXQCI2FM6E2XTPPAERUKZ4JVPG66OUL"}})");
        ;
    }

    // Check Asset (CREDIT_ALPHANUM12, valid code)
    {
        Asset a;
        a.type(ASSET_TYPE_CREDIT_ALPHANUM12);
        copyToArray(a.alphaNum12().assetCode, "USDC12");
        copyHexToArray(a.alphaNum12().issuer.ed25519(), accountIdHex);
        CHECK(
            toCerealCompact(a, "a") ==
            R"({"a":{"assetCode":"USDC12","issuer":"GAASGRLHRGV433YBENCWPCNLZXXQCI2FM6E2XTPPAERUKZ4JVPG66OUL"}})");
    }

    // Check Asset (CREDIT_ALPHANUM4, invalid code with embedded NUL)
    {
        Asset a;
        a.type(ASSET_TYPE_CREDIT_ALPHANUM4);
        copyToArray(a.alphaNum4().assetCode, std::string{'U', '\0', 'D', '\0'});
        copyHexToArray(a.alphaNum4().issuer.ed25519(), accountIdHex);
        CHECK(
            toCerealCompact(a, "a") ==
            R"({"a":{"assetCodeRaw":"55004400","issuer":"GAASGRLHRGV433YBENCWPCNLZXXQCI2FM6E2XTPPAERUKZ4JVPG66OUL"}})");
    }

    // Check TrustLineAsset (POOL_SHARE)
    {
        TrustLineAsset tla;
        tla.type(ASSET_TYPE_POOL_SHARE);
        copyHexToArray(tla.liquidityPoolID(), accountIdHex);
        CHECK(
            toCerealCompact(tla, "a") ==
            R"({"a":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"})");
    }

    // Check ChangeTrustAsset (POOL_SHARE)
    {
        ChangeTrustAsset cta;
        cta.type(ASSET_TYPE_POOL_SHARE);
        cta.liquidityPool().type(LIQUIDITY_POOL_CONSTANT_PRODUCT);
        auto& cp = cta.liquidityPool().constantProduct();
        cp.assetA.type(ASSET_TYPE_NATIVE);
        cp.assetB.type(ASSET_TYPE_NATIVE);
        cp.fee = 30;
        CHECK(toCerealCompact(cta, "a") ==
              R"({"a":{"assetA":"NATIVE","assetB":"NATIVE","fee":30}})");
    }

    // Check enum
    {
        AssetType e = ASSET_TYPE_NATIVE;
        CHECK(toCerealCompact(e, "e") == R"({"e":"ASSET_TYPE_NATIVE"})");
    }

    // Check pointer (null)
    {
        xdr::pointer<Asset> p;
        CHECK(toCerealCompact(p, "p") == R"({"p":null})");
    }

    // Check pointer (non-null)
    {
        Asset a;
        a.type(ASSET_TYPE_NATIVE);
        xdr::pointer<Asset> p(new Asset{a});
        CHECK(toCerealCompact(p, "p") == R"({"p":"NATIVE"})");
    }

    // Check nested pointer (null)
    {
        xdr::pointer<xdr::pointer<Asset>> p;
        CHECK(toCerealCompact(p, "p") == R"({"p":[]})");
    }

    // Check nested pointer (outer non-null, inner null)
    {
        xdr::pointer<xdr::pointer<Asset>> p;
        p.reset(new xdr::pointer<Asset>{});
        CHECK(toCerealCompact(p, "p") == R"({"p":[null]})");
    }

    // Check nested pointer (outer non-null, inner non-null)
    {
        Asset a;
        a.type(ASSET_TYPE_NATIVE);
        xdr::pointer<xdr::pointer<Asset>> p;
        p.reset(new xdr::pointer<Asset>{new Asset{a}});
        CHECK(toCerealCompact(p, "p") == R"({"p":["NATIVE"]})");
    }
}
} // namespace
