// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "SorobanTxTestUtils.h"
#include "ledger/LedgerTypeUtils.h"
#include "rust/RustBridge.h"
#include "test/Catch2.h"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "transactions/InvokeHostFunctionOpFrame.h"
#include "transactions/TransactionUtils.h"
#include "util/XDRCereal.h"
#include "xdrpp/printer.h"

namespace stellar
{

namespace txtest
{
namespace
{
SCVal
signPayloadForClassicAccount(std::vector<TestAccount*> const& signers,
                             uint256 payload)
{
    SCVal signatureStruct(SCV_VEC);
    auto& signatures = signatureStruct.vec().activate();
    auto sortedSigners = signers;
    std::sort(sortedSigners.begin(), sortedSigners.end(),
              [](TestAccount* a, TestAccount* b) {
                  return a->getPublicKey() < b->getPublicKey();
              });
    for (auto& account : sortedSigners)
    {
        SCVal signatureVal(SCV_MAP);
        signatureVal.map().activate().emplace_back(
            makeSymbolSCVal("public_key"),
            makeBytesSCVal(account->getPublicKey().ed25519()));
        auto signature = account->getSecretKey().sign(payload);
        signatureVal.map().activate().emplace_back(makeSymbolSCVal("signature"),
                                                   makeBytesSCVal(signature));
        signatures.push_back(signatureVal);
    }
    return signatureStruct;
}
} // namespace

SCAddress
makeContractAddress(Hash const& hash)
{
    SCAddress addr(SC_ADDRESS_TYPE_CONTRACT);
    addr.contractId() = hash;
    return addr;
}

SCAddress
makeMuxedAccountAddress(AccountID const& accountID, uint64_t id)
{
    SCAddress addr(SC_ADDRESS_TYPE_MUXED_ACCOUNT);
    addr.muxedAccount().ed25519 = accountID.ed25519();
    addr.muxedAccount().id = id;
    return addr;
}

SCVal
makeI32(int32_t i32)
{
    SCVal val(SCV_I32);
    val.i32() = i32;
    return val;
}

SCVal
makeI128(uint64_t u64)
{
    Int128Parts p;
    p.hi = 0;
    p.lo = u64;

    SCVal val(SCV_I128);
    val.i128() = p;
    return val;
}

SCSymbol
makeSymbol(std::string const& str)
{
    SCSymbol val;
    val.assign(str.begin(), str.end());
    return val;
}

SCVal
makeU64(uint64_t u64)
{
    SCVal val(SCV_U64);
    val.u64() = u64;
    return val;
}

SCVal
makeU32(uint32_t u32)
{
    SCVal val(SCV_U32);
    val.u32() = u32;
    return val;
}

SCVal
makeVecSCVal(std::vector<SCVal> elems)
{
    SCVal val(SCV_VEC);
    val.vec().activate().assign(elems.begin(), elems.end());
    return val;
}

SCVal
makeBool(bool b)
{
    SCVal val(SCV_BOOL);
    val.b() = b;
    return val;
}

int64_t
getContractBalance(Application& app, SCAddress const& contractID,
                   SCVal const& accountVal)
{
    LedgerKey balanceKey(CONTRACT_DATA);
    balanceKey.contractData().contract = contractID;

    balanceKey.contractData().key =
        makeVecSCVal({makeSymbolSCVal("Balance"), accountVal});
    balanceKey.contractData().durability = ContractDataDurability::PERSISTENT;

    LedgerTxn ltx(app.getLedgerTxnRoot());
    auto ltxe = ltx.load(balanceKey);
    if (!ltxe)
    {
        return 0;
    }
    auto const& balance =
        ltxe.current().data.contractData().val.map()->at(0).val.i128();

    // Contract balances can be greater than INT64_MAX, but for these tests
    // we'll assume balances < INT64_MAX.
    REQUIRE(balance.hi == 0);
    return balance.lo;
}

ContractIDPreimage
makeContractIDPreimage(TestAccount& source, uint256 salt)
{
    ContractIDPreimage idPreimage(CONTRACT_ID_PREIMAGE_FROM_ADDRESS);
    idPreimage.fromAddress().address.type(SC_ADDRESS_TYPE_ACCOUNT);
    idPreimage.fromAddress().address.accountId().ed25519() =
        source.getPublicKey().ed25519();
    idPreimage.fromAddress().salt = salt;
    return idPreimage;
}

ContractIDPreimage
makeContractIDPreimage(Asset const& asset)
{
    ContractIDPreimage idPreimage(CONTRACT_ID_PREIMAGE_FROM_ASSET);
    idPreimage.fromAsset() = asset;
    return idPreimage;
}

HashIDPreimage
makeFullContractIdPreimage(Hash const& networkID,
                           ContractIDPreimage const& contractIDPreimage)
{
    HashIDPreimage fullPreImage;
    fullPreImage.type(ENVELOPE_TYPE_CONTRACT_ID);
    fullPreImage.contractID().contractIDPreimage = contractIDPreimage;
    fullPreImage.contractID().networkID = networkID;
    return fullPreImage;
}

ContractExecutable
makeWasmExecutable(Hash const& wasmHash)
{
    ContractExecutable executable(CONTRACT_EXECUTABLE_WASM);
    executable.wasm_hash() = wasmHash;
    return executable;
}

ContractExecutable
makeAssetExecutable(Asset const& asset)
{
    ContractExecutable executable(CONTRACT_EXECUTABLE_STELLAR_ASSET);
    return executable;
}

LedgerKey
makeContractInstanceKey(SCAddress const& contractAddress)
{
    SCVal instanceContractDataKey(SCValType::SCV_LEDGER_KEY_CONTRACT_INSTANCE);
    return contractDataKey(contractAddress, instanceContractDataKey,
                           CONTRACT_INSTANCE_ENTRY_DURABILITY);
}

TransactionFrameBaseConstPtr
makeSorobanWasmUploadTx(Application& app, TestAccount& source,
                        RustBuf const& wasm, SorobanResources& uploadResources,
                        uint32_t inclusionFee, int64_t additionalRefundableFee)
{
    Operation uploadOp;
    uploadOp.body.type(INVOKE_HOST_FUNCTION);
    auto& uploadHF = uploadOp.body.invokeHostFunctionOp().hostFunction;
    uploadHF.type(HOST_FUNCTION_TYPE_UPLOAD_CONTRACT_WASM);
    uploadHF.wasm().assign(wasm.data.begin(), wasm.data.end());
    if (uploadResources.footprint.readWrite.empty())
    {
        REQUIRE(uploadResources.footprint.readOnly.empty());
        uploadResources.footprint.readWrite = {
            contractCodeKey(sha256(uploadHF.wasm()))};
    }
    auto uploadResourceFee =
        sorobanResourceFee(app, uploadResources, 1000 + wasm.data.size(), 40) +
        DEFAULT_TEST_RESOURCE_FEE + additionalRefundableFee;
    return sorobanTransactionFrameFromOps(app.getNetworkID(), source,
                                          {uploadOp}, {}, uploadResources,
                                          inclusionFee, uploadResourceFee);
}

bool
isExpiredStatus(ExpirationStatus status)
{
    return status == ExpirationStatus::EXPIRED_IN_LIVE_STATE ||
           status == ExpirationStatus::HOT_ARCHIVE;
}

SorobanResources
defaultUploadWasmResourcesWithoutFootprint(RustBuf const& wasm,
                                           uint32_t ledgerVersion)
{
    SorobanResources resources;

    // Use a different default starting from v21 for the vm module cache
    // changes. Keep the v20 resources the same so we can validate that we're
    // not changing fees for the existing protocol.
    if (protocolVersionStartsFrom(ledgerVersion, ProtocolVersion::V_21))
    {
        resources.instructions =
            static_cast<uint32_t>(500'000 + (wasm.data.size() * 5000));
        resources.diskReadBytes = 1000;
        resources.writeBytes = static_cast<uint32_t>(wasm.data.size() + 150);
    }
    else
    {
        resources.instructions =
            static_cast<uint32_t>(500'000 + (wasm.data.size() * 1000));
        resources.diskReadBytes = 1000;
        resources.writeBytes = static_cast<uint32_t>(wasm.data.size() + 100);
    }
    return resources;
}

ContractEvent
makeContractEvent(Hash const& contractId, std::vector<SCVal> const& topics,
                  SCVal const& data)
{
    ContractEvent event;
    event.type = ContractEventType::CONTRACT;
    event.contractID.activate() = contractId;
    event.body.v0().topics.assign(topics.begin(), topics.end());
    event.body.v0().data = data;
    return event;
}

ContractEvent
makeTransferEvent(SCAddress const& from, SCAddress const& to, int64_t amount,
                  std::optional<int64_t> toMuxId)
{
    return ContractEvent();
}

ContractEvent
makeTransferEvent(const stellar::Hash& contractId, Asset const& asset,
                  SCAddress const& from, SCAddress const& to, int64_t amount,
                  std::optional<SCMapEntry> toMemoEntry)
{
    std::string name;
    switch (asset.type())
    {
    case AssetType::ASSET_TYPE_NATIVE:
        name = "native";
        break;
    case AssetType::ASSET_TYPE_CREDIT_ALPHANUM4:
        name = std::string(asset.alphaNum4().assetCode.begin(),
                           asset.alphaNum4().assetCode.end()) +
               ":" + KeyUtils::toStrKey(asset.alphaNum4().issuer);
        break;
    case AssetType::ASSET_TYPE_CREDIT_ALPHANUM12:
        name = std::string(asset.alphaNum12().assetCode.begin(),
                           asset.alphaNum12().assetCode.end()) +
               ":" + KeyUtils::toStrKey(asset.alphaNum12().issuer);
        break;
    default:
        releaseAssertOrThrow(false);
        break;
    }

    std::vector<SCVal> topics = {makeSymbolSCVal("transfer"),
                                 makeAddressSCVal(from), makeAddressSCVal(to),
                                 makeStringSCVal(std::move(name))};
    SCVal data;
    if (toMemoEntry)
    {
        data.type(SCValType::SCV_MAP);
        data.map().activate().push_back(
            SCMapEntry(makeSymbolSCVal("amount"), makeI128(amount)));
        data.map().activate().push_back(*toMemoEntry);
    }
    else
    {
        data = makeI128(amount);
    }
    return makeContractEvent(contractId, topics, data);
}

ContractEvent
makeMintOrBurnEvent(bool isMint, const stellar::Hash& contractId,
                    Asset const& asset, SCAddress const& addr, int64 amount,
                    std::optional<SCMapEntry> memoEntry)
{
    ContractEvent ev;
    ev.type = ContractEventType::CONTRACT;
    ev.contractID.activate() = contractId;

    SCVec topics = {isMint ? makeSymbolSCVal("mint") : makeSymbolSCVal("burn"),
                    makeAddressSCVal(getAddressWithDroppedMuxedInfo(addr)),
                    makeSep0011AssetStringSCVal(asset)};
    ev.body.v0().topics = topics;

    if (isMint && memoEntry)
    {
        SCVal data;
        data.type(SCValType::SCV_MAP);
        data.map().activate().push_back(
            SCMapEntry(makeSymbolSCVal("amount"), makeI128(amount)));
        data.map().activate().push_back(*memoEntry);
        ev.body.v0().data = data;
    }
    else
    {
        // a memo entry should not be passed in for the burn event
        releaseAssert(!memoEntry);
        ev.body.v0().data = makeI128SCVal(amount);
    }

    return ev;
}

void
validateFeeEvent(TransactionEvent const& feeEvent, PublicKey const& feeSource,
                 int64_t feeCharged, uint32_t protocolVersion, bool isRefund)
{
    auto const& feeEventTopics = feeEvent.event.body.v0().topics;
    REQUIRE(feeEventTopics.size() == 2);

    auto const& firstTopic = feeEventTopics.at(0);
    REQUIRE((firstTopic.type() == SCV_SYMBOL && firstTopic.sym() == "fee"));

    auto const& secondTopic = feeEventTopics.at(1);
    REQUIRE((secondTopic.type() == SCV_ADDRESS &&
             secondTopic.address().accountId() == feeSource));

    auto const& feeEventData = feeEvent.event.body.v0().data;

    auto feei128 = rust_bridge::i128_from_i64(feeCharged);
    REQUIRE(feeEventData.i128().hi == feei128.hi);
    REQUIRE(feeEventData.i128().lo == feei128.lo);

    if (!isRefund)
    {
        REQUIRE(feeEvent.stage ==
                TransactionEventStage::TRANSACTION_EVENT_STAGE_BEFORE_ALL_TXS);
    }
    else
    {
        if (protocolVersionIsBefore(protocolVersion, ProtocolVersion::V_23))
        {
            REQUIRE(feeEvent.stage ==
                    TransactionEventStage::TRANSACTION_EVENT_STAGE_AFTER_TX);
        }
        else
        {
            REQUIRE(
                feeEvent.stage ==
                TransactionEventStage::TRANSACTION_EVENT_STAGE_AFTER_ALL_TXS);
        }
    }
}

SorobanResources
defaultCreateWasmContractResources(RustBuf const& wasm)
{
    return SorobanResources();
}

TransactionFrameBaseConstPtr
makeSorobanCreateContractTx(Application& app, TestAccount& source,
                            ContractIDPreimage const& idPreimage,
                            ContractExecutable const& executable,
                            SorobanResources& createResources,
                            uint32_t inclusionFee,
                            ConstructorParams const& constructorParams)
{
    // Default to V1 function when constructor has no arguments. This is just
    // to have some coverage after the protocol bump - constructor tests should
    // force the version explicitly.
    auto hostFnVersion = constructorParams.constructorArgs.empty()
                             ? ConstructorParams::HostFnVersion::V1
                             : ConstructorParams::HostFnVersion::V2;
    if (constructorParams.forceHostFnVersion)
    {
        hostFnVersion = *constructorParams.forceHostFnVersion;
    }
    releaseAssert(constructorParams.constructorArgs.empty() ||
                  hostFnVersion == ConstructorParams::HostFnVersion::V2);
    if (createResources.footprint.readWrite.empty())
    {
        releaseAssert(createResources.footprint.readOnly.empty());
        auto contractID = xdrSha256(
            makeFullContractIdPreimage(app.getNetworkID(), idPreimage));
        if (executable.type() ==
            ContractExecutableType::CONTRACT_EXECUTABLE_WASM)
        {
            createResources.footprint.readOnly = {
                contractCodeKey(executable.wasm_hash())};
        }
        createResources.footprint.readWrite = {
            makeContractInstanceKey(makeContractAddress(contractID))};
    }
    if (constructorParams.additionalResources)
    {
        auto const& additionalResources =
            *constructorParams.additionalResources;
        createResources.footprint.readOnly.insert(
            createResources.footprint.readOnly.end(),
            additionalResources.footprint.readOnly.begin(),
            additionalResources.footprint.readOnly.end());
        createResources.footprint.readWrite.insert(
            createResources.footprint.readWrite.end(),
            additionalResources.footprint.readWrite.begin(),
            additionalResources.footprint.readWrite.end());
        createResources.instructions += additionalResources.instructions;
        createResources.diskReadBytes += additionalResources.diskReadBytes;
        createResources.writeBytes += additionalResources.writeBytes;
    }

    Operation createOp;
    createOp.body.type(INVOKE_HOST_FUNCTION);
    auto& createHF = createOp.body.invokeHostFunctionOp().hostFunction;

    if (hostFnVersion == ConstructorParams::HostFnVersion::V2)
    {
        createHF.type(HOST_FUNCTION_TYPE_CREATE_CONTRACT_V2);
        auto& createContractArgs = createHF.createContractV2();
        createContractArgs.contractIDPreimage = idPreimage;
        createContractArgs.executable = executable;
        createContractArgs.constructorArgs.assign(
            constructorParams.constructorArgs.begin(),
            constructorParams.constructorArgs.end());
    }
    else
    {
        createHF.type(HOST_FUNCTION_TYPE_CREATE_CONTRACT);
        auto& createContractArgs = createHF.createContract();
        createContractArgs.contractIDPreimage = idPreimage;
        createContractArgs.executable = executable;
    }

    if (executable.type() !=
        ContractExecutableType::CONTRACT_EXECUTABLE_STELLAR_ASSET)
    {
        SorobanAuthorizationEntry auth;
        auth.credentials.type(SOROBAN_CREDENTIALS_SOURCE_ACCOUNT);
        auto authHostFnVersion =
            protocolVersionStartsFrom(app.getLedgerManager()
                                          .getLastClosedLedgerHeader()
                                          .header.ledgerVersion,
                                      ProtocolVersion::V_22)
                ? ConstructorParams::HostFnVersion::V2
                : ConstructorParams::HostFnVersion::V1;
        if (constructorParams.forceAuthHostFnVersion)
        {
            authHostFnVersion = *constructorParams.forceAuthHostFnVersion;
        }
        if (authHostFnVersion == ConstructorParams::HostFnVersion::V2)
        {
            auth.rootInvocation.function.type(
                SOROBAN_AUTHORIZED_FUNCTION_TYPE_CREATE_CONTRACT_V2_HOST_FN);
            auto& invocationFn =
                auth.rootInvocation.function.createContractV2HostFn();
            invocationFn.contractIDPreimage = idPreimage;
            invocationFn.executable = executable;
            invocationFn.constructorArgs.assign(
                constructorParams.constructorArgs.begin(),
                constructorParams.constructorArgs.end());
            auth.rootInvocation.subInvocations =
                constructorParams.additionalAuthInvocations;
        }
        else
        {
            auth.rootInvocation.function.type(
                SOROBAN_AUTHORIZED_FUNCTION_TYPE_CREATE_CONTRACT_HOST_FN);
            auth.rootInvocation.function.createContractHostFn()
                .contractIDPreimage = idPreimage;
            auth.rootInvocation.function.createContractHostFn().executable =
                executable;
        }

        createOp.body.invokeHostFunctionOp().auth = {auth};
    }

    auto createResourceFee =
        sorobanResourceFee(app, createResources, 1000, 40) +
        DEFAULT_TEST_RESOURCE_FEE;
    return sorobanTransactionFrameFromOps(app.getNetworkID(), source,
                                          {createOp}, {}, createResources,
                                          inclusionFee, createResourceFee);
}

TransactionFrameBaseConstPtr
makeSorobanCreateContractTx(Application& app, TestAccount& source,
                            ContractIDPreimage const& idPreimage,
                            ContractExecutable const& executable,
                            SorobanResources& createResources,
                            uint32_t inclusionFee)
{
    return makeSorobanCreateContractTx(app, source, idPreimage, executable,
                                       createResources, inclusionFee, {});
}

TransactionTestFramePtr
sorobanTransactionFrameFromOps(Hash const& networkID, TestAccount& source,
                               std::vector<Operation> const& ops,
                               std::vector<SecretKey> const& opKeys,
                               SorobanInvocationSpec const& spec,
                               std::optional<std::string> memo,
                               std::optional<SequenceNumber> seq)
{
    return sorobanTransactionFrameFromOps(
        networkID, source, ops, opKeys, spec.getResources(),
        spec.getInclusionFee(), spec.getResourceFee(), memo, seq,
        spec.getArchivedIndexes());
}

SorobanInvocationSpec::SorobanInvocationSpec(SorobanResources const& resources,
                                             uint32_t nonRefundableResourceFee,
                                             uint32_t refundableResourceFee,
                                             uint32_t inclusionFee)
    : mResources(resources)
    , mNonRefundableResourceFee(nonRefundableResourceFee)
    , mRefundableResourceFee(refundableResourceFee)
    , mInclusionFee(inclusionFee)
{
}

SorobanResources const&
SorobanInvocationSpec::getResources() const
{
    return mResources;
}

uint32_t
SorobanInvocationSpec::getFee() const
{
    return mInclusionFee + getResourceFee();
}

uint32_t
SorobanInvocationSpec::getResourceFee() const
{
    return mNonRefundableResourceFee + mRefundableResourceFee;
}

uint32_t
SorobanInvocationSpec::getInclusionFee() const
{
    return mInclusionFee;
}

std::optional<std::vector<uint32_t>>
SorobanInvocationSpec::getArchivedIndexes() const
{
    return mArchivedIndexes;
}

SorobanInvocationSpec
SorobanInvocationSpec::setInstructions(int64_t instructions) const
{
    auto newSpec = *this;
    newSpec.mResources.instructions = static_cast<uint32_t>(instructions);
    return newSpec;
}

SorobanInvocationSpec
SorobanInvocationSpec::setReadOnlyFootprint(
    xdr::xvector<LedgerKey> const& keys) const
{
    auto newSpec = *this;
    newSpec.mResources.footprint.readOnly = keys;
    return newSpec;
}

SorobanInvocationSpec
SorobanInvocationSpec::setReadWriteFootprint(
    xdr::xvector<LedgerKey> const& keys) const
{
    auto newSpec = *this;
    newSpec.mResources.footprint.readWrite = keys;
    return newSpec;
}

SorobanInvocationSpec
SorobanInvocationSpec::extendReadOnlyFootprint(
    xdr::xvector<LedgerKey> const& keys) const
{
    auto newSpec = *this;
    newSpec.mResources.footprint.readOnly.insert(
        newSpec.mResources.footprint.readOnly.end(), keys.begin(), keys.end());
    return newSpec;
}

SorobanInvocationSpec
SorobanInvocationSpec::extendReadWriteFootprint(
    xdr::xvector<LedgerKey> const& keys) const
{
    auto newSpec = *this;
    newSpec.mResources.footprint.readWrite.insert(
        newSpec.mResources.footprint.readWrite.end(), keys.begin(), keys.end());
    return newSpec;
}

SorobanInvocationSpec
SorobanInvocationSpec::setReadBytes(uint32_t readBytes) const
{
    auto newSpec = *this;
    newSpec.mResources.diskReadBytes = readBytes;
    return newSpec;
}

SorobanInvocationSpec
SorobanInvocationSpec::setWriteBytes(uint32_t writeBytes) const
{
    auto newSpec = *this;
    newSpec.mResources.writeBytes = writeBytes;
    return newSpec;
}

SorobanInvocationSpec
SorobanInvocationSpec::setRefundableResourceFee(uint32_t fee) const
{
    auto newSpec = *this;
    newSpec.mRefundableResourceFee = fee;
    return newSpec;
}

SorobanInvocationSpec
SorobanInvocationSpec::setNonRefundableResourceFee(uint32_t fee) const
{
    auto newSpec = *this;
    newSpec.mNonRefundableResourceFee = fee;
    return newSpec;
}

SorobanInvocationSpec
SorobanInvocationSpec::setInclusionFee(uint32_t fee) const
{
    auto newSpec = *this;
    newSpec.mInclusionFee = fee;
    return newSpec;
}

SorobanInvocationSpec
SorobanInvocationSpec::setArchivedIndexes(
    std::vector<uint32_t> const& indexes) const
{
    auto newSpec = *this;
    newSpec.mArchivedIndexes = indexes;
    return newSpec;
}

TestContract::TestContract(SorobanTest& test, SCAddress const& address,
                           xdr::xvector<LedgerKey> const& contractKeys)
    : mTest(test), mContractKeys(contractKeys), mAddress(address)
{
}

xdr::xvector<LedgerKey> const&
TestContract::getKeys() const
{
    return mContractKeys;
}

SCAddress const&
TestContract::getAddress() const
{
    return mAddress;
}

SorobanTest&
TestContract::getTest() const
{
    return mTest;
}

LedgerKey
TestContract::getDataKey(SCVal const& key, ContractDataDurability durability)
{
    return stellar::contractDataKey(mAddress, key, durability);
}

TestContract::Invocation
TestContract::prepareInvocation(std::string const& functionName,
                                std::vector<SCVal> const& args,
                                SorobanInvocationSpec const& spec,
                                bool addContractKeys) const
{
    return Invocation(*this, functionName, args, spec, addContractKeys);
}

void
TestContract::Invocation::deduplicateFootprint()
{
    xdr::xvector<LedgerKey> readOnly;
    xdr::xvector<LedgerKey> readWrite;
    UnorderedSet<LedgerKey> keys;

    auto deduplicate = [&](auto const& fp) {
        for (const auto& key : fp)
        {
            if (keys.insert(key).second)
            {
                readWrite.push_back(key);
            }
        }
    };
    deduplicate(mSpec.getResources().footprint.readWrite);
    deduplicate(mSpec.getResources().footprint.readOnly);
    mSpec =
        mSpec.setReadOnlyFootprint(readOnly).setReadWriteFootprint(readWrite);
}

TestContract::Invocation::Invocation(TestContract const& contract,
                                     std::string const& functionName,
                                     std::vector<SCVal> const& args,
                                     SorobanInvocationSpec const& spec,
                                     bool addContractKeys)
    : mTest(contract.getTest()), mSpec(spec)
{
    mOp.body.type(INVOKE_HOST_FUNCTION);
    auto& ihf = mOp.body.invokeHostFunctionOp().hostFunction;
    ihf.type(HOST_FUNCTION_TYPE_INVOKE_CONTRACT);
    ihf.invokeContract().contractAddress = contract.getAddress();
    ihf.invokeContract().functionName = makeSymbol(functionName);
    ihf.invokeContract().args.assign(args.begin(), args.end());

    if (addContractKeys)
    {
        mSpec = mSpec.extendReadOnlyFootprint(contract.getKeys());
    }
}

TestContract::Invocation&
TestContract::Invocation::withAuthorizedTopCall()
{
    SorobanAuthorizedInvocation ai;
    ai.function.type(SOROBAN_AUTHORIZED_FUNCTION_TYPE_CONTRACT_FN);
    ai.function.contractFn() =
        mOp.body.invokeHostFunctionOp().hostFunction.invokeContract();
    return withSourceAccountAuthorization(ai);
}

TestContract::Invocation&
TestContract::Invocation::withAuthorizedTopCall(SorobanSigner const& signer)
{
    SorobanAuthorizedInvocation ai;
    ai.function.type(SOROBAN_AUTHORIZED_FUNCTION_TYPE_CONTRACT_FN);
    ai.function.contractFn() =
        mOp.body.invokeHostFunctionOp().hostFunction.invokeContract();
    return withAuthorization(ai, signer);
}

TestContract::Invocation&
TestContract::Invocation::withAuthorization(
    SorobanAuthorizedInvocation const& invocation,
    SorobanCredentials credentials)
{
    mOp.body.invokeHostFunctionOp().auth.emplace_back(credentials, invocation);
    if (credentials.type() ==
        SorobanCredentialsType::SOROBAN_CREDENTIALS_ADDRESS)
    {
        SCVal nonceKey(SCValType::SCV_LEDGER_KEY_NONCE);
        nonceKey.nonce_key().nonce = credentials.address().nonce;
        mSpec = mSpec.extendReadWriteFootprint(
            {contractDataKey(credentials.address().address, nonceKey,
                             ContractDataDurability::TEMPORARY)});
    }
    return *this;
}

TestContract::Invocation&
TestContract::Invocation::withAuthorization(
    SorobanAuthorizedInvocation const& invocation, SorobanSigner const& signer)
{
    mSpec = mSpec.extendReadOnlyFootprint(signer.getLedgerKeys());
    return withAuthorization(invocation, signer.sign(invocation));
}

TestContract::Invocation&
TestContract::Invocation::withSourceAccountAuthorization(
    SorobanAuthorizedInvocation const& invocation)
{
    SorobanCredentials credentials(SOROBAN_CREDENTIALS_SOURCE_ACCOUNT);
    return withAuthorization(invocation, credentials);
}

TestContract::Invocation&
TestContract::Invocation::withDeduplicatedFootprint()
{
    mDeduplicateFootprint = true;
    return *this;
}

TestContract::Invocation&
TestContract::Invocation::withSpec(SorobanInvocationSpec const& spec)
{
    mSpec = spec;
    return *this;
}

SorobanInvocationSpec
TestContract::Invocation::getSpec()
{
    return mSpec;
}

TestContract::Invocation&
TestContract::Invocation::withOpSourceAccount(AccountID const& source)
{
    mOp.sourceAccount.activate() = toMuxedAccount(source);
    return *this;
}

TransactionTestFramePtr
TestContract::Invocation::createTx(TestAccount* source,
                                   std::optional<std::string> memo)
{
    if (mDeduplicateFootprint)
    {
        deduplicateFootprint();
    }
    auto& acc = source ? *source : mTest.getRoot();

    return sorobanTransactionFrameFromOps(mTest.getApp().getNetworkID(), acc,
                                          {mOp}, {}, mSpec, memo);
}

TestContract::Invocation&
TestContract::Invocation::withExactNonRefundableResourceFee()
{
    // Compute tx frame size before finalizing the resource fee via a dummy TX.
    // This is a bit hacky, but we need the exact tx size in order to
    // enable tests that rely on the exact refundable fee value.
    // Note, that we don't use the root account here in order to not mess up
    // the sequence numbers.
    auto dummyTx = sorobanTransactionFrameFromOps(mTest.getApp().getNetworkID(),
                                                  mTest.getDummyAccount(),
                                                  {mOp}, {}, mSpec);
    auto txSize = xdr::xdr_size(dummyTx->getEnvelope());
    auto fee = sorobanResourceFee(mTest.getApp(), mSpec.getResources(), txSize,
                                  0, mSpec.getArchivedIndexes());
    releaseAssert(fee <= UINT32_MAX);
    mSpec = mSpec.setNonRefundableResourceFee(static_cast<uint32_t>(fee));
    return *this;
}

bool
TestContract::Invocation::invoke(TestAccount* source)
{
    auto tx = createTx(source);
    CLOG_INFO(Tx, "invoke tx {}", xdr::xdr_to_string(tx->getEnvelope()));
    auto result = mTest.invokeTx(tx);
    mTxMeta = mTest.getLastTxMeta();

    mFeeCharged = result.feeCharged;
    if (result.result.code() == txFAILED || result.result.code() == txSUCCESS)
    {
        mResultCode =
            result.result.results()[0].tr().invokeHostFunctionResult().code();
    }
    else
    {
        mResultCode = std::nullopt;
    }
    return isSuccessResult(result);
}

SCVal
TestContract::Invocation::getReturnValue() const
{
    REQUIRE(mTxMeta);
    return mTxMeta->getReturnValue();
}

TransactionMetaFrame const&
TestContract::Invocation::getTxMeta() const
{
    REQUIRE(mTxMeta);
    return *mTxMeta;
}

std::optional<InvokeHostFunctionResultCode>
TestContract::Invocation::getResultCode() const
{
    return mResultCode;
}

int64_t
TestContract::Invocation::getFeeCharged() const
{
    return mFeeCharged;
}

void
SorobanTest::initialize(bool useTestLimits,
                        std::function<void(SorobanNetworkConfig&)> cfgModifyFn)
{
    if (useTestLimits)
    {
        overrideSorobanNetworkConfigForTest(getApp());
    }

    modifySorobanNetworkConfig(getApp(), cfgModifyFn);
}

// This constructor duplication exists because mClock cannot be accessed in a
// delegate constructor, which keeps us from calling createTestApplication
SorobanTest::SorobanTest(Config cfg, bool useTestLimits,
                         std::function<void(SorobanNetworkConfig&)> cfgModifyFn)
    : mApp(createTestApplication(mClock, cfg))
    , mRoot(getApp().getRoot())
    , mDummyAccount(mRoot->create(
          "dummyAcc", getApp().getLedgerManager().getLastMinBalance(1)))
{
    initialize(useTestLimits, cfgModifyFn);
}

SorobanTest::SorobanTest(Application::pointer app, Config cfg,
                         bool useTestLimits,
                         std::function<void(SorobanNetworkConfig&)> cfgModifyFn)
    : mApp(app)
    , mRoot(getApp().getRoot())
    , mDummyAccount(mRoot->create(
          "dummyAcc", getApp().getLedgerManager().getLastMinBalance(1)))
{
    initialize(useTestLimits, cfgModifyFn);
}

int64_t
SorobanTest::computeFeePerIncrement(int64_t resourceVal, int64_t feeRate,
                                    int64_t increment)
{
    // ceiling division for (resourceVal * feeRate) / increment
    int64_t num = (resourceVal * feeRate);
    return (num + increment - 1) / increment;
};

int64_t
SorobanTest::getAccountBalance(TestAccount* source)
{
    auto& account = source ? *source : getRoot();
    return account.getBalance();
}

void
SorobanTest::checkRefundableFee(int64_t initialBalance,
                                TransactionFrameBaseConstPtr tx,
                                int64_t expectedRentFeeCharged,
                                size_t eventsSize, bool success)
{
    if (!success)
    {
        REQUIRE(expectedRentFeeCharged == 0);
    }
    auto lcm = getLastLcm();
    auto txm = getLastTxMeta();
    auto feeSource = tx->getFeeSourceID();
    // Get the account balance after transaction execution
    int64_t balanceAfterFeeCharged;
    if (feeSource == getRoot().getPublicKey())
    {
        balanceAfterFeeCharged = getRoot().getBalance();
    }
    else
    {
        LedgerTxn ltx(mApp->getLedgerTxnRoot());
        auto account = ltx.load(accountKey(feeSource));
        REQUIRE(account);
        balanceAfterFeeCharged = account.current().data.account().balance;
    }

    int64_t chargedBaseFee = 100 * tx->getNumOperations();

    std::optional<std::vector<uint32_t>> archivedIndexes;
    auto ext = tx->getResourcesExt();
    if (ext.v() == 1)
    {
        archivedIndexes = ext.resourceExt().archivedSorobanEntries;
    }
    auto txSize = tx->getResources(false, getLedgerVersion())
                      .getVal(Resource::Type::TX_BYTE_SIZE);
    int64_t nonRefundableResourceFee =
        sorobanResourceFee(getApp(), tx->sorobanResources(), txSize, 0,
                           archivedIndexes, tx->isRestoreFootprintTx());
    int64_t eventsFee =
        sorobanResourceFee(getApp(), tx->sorobanResources(), txSize, eventsSize,
                           archivedIndexes, tx->isRestoreFootprintTx()) -
        nonRefundableResourceFee;
    if (!success)
    {
        eventsFee = 0;
    }
    int64_t expectedFeeCharged = nonRefundableResourceFee + chargedBaseFee +
                                 eventsFee + expectedRentFeeCharged;

    int64_t actualFeeCharged = initialBalance - balanceAfterFeeCharged;
    int64_t actualRentFeeCharged = actualFeeCharged - nonRefundableResourceFee -
                                   eventsFee - chargedBaseFee;
    REQUIRE(actualRentFeeCharged == expectedRentFeeCharged);

    // Meta should contain the refund in `changesAfter`.
    int64_t chargedBeforeRefund =
        (tx->getFullFee() - tx->getInclusionFee()) + chargedBaseFee;
    int64_t expectedRefund = chargedBeforeRefund - expectedFeeCharged;

    LedgerEntryChanges refundChanges;
    if (protocolVersionStartsFrom(getLedgerVersion(),
                                  PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION))
    {
        refundChanges = lcm.getPostTxApplyFeeProcessing(0);
    }
    else
    {
        refundChanges = txm.getChangesAfter();
    }
    if (expectedRefund != 0)
    {
        REQUIRE(refundChanges.size() == 2);
        REQUIRE(refundChanges[1].updated().data.account().balance -
                    refundChanges[0].state().data.account().balance ==
                expectedRefund);
    }
    else
    {
        REQUIRE(refundChanges.size() == 0);
    }

    if (txm.eventsAreSupported())
    {
        auto const& txEvents = txm.getTxEvents();

        if (!txEvents.empty())
        {
            validateFeeEvent(txEvents[0], feeSource, chargedBeforeRefund,
                             getLedgerVersion(), false);
            if (expectedRefund == 0)
            {
                REQUIRE(txEvents.size() == 1);
            }
            else
            {
                REQUIRE(txEvents.size() == 2);
                validateFeeEvent(txEvents[1], feeSource, -expectedRefund,
                                 getLedgerVersion(), true);
            }
        }
    }
}

void
SorobanTest::invokeArchivalOp(TransactionFrameBaseConstPtr tx,
                              int64_t expectedRefundableFeeCharged)
{
    MutableTxResultPtr result;
    {
        auto diagnostics = DiagnosticEventManager::createDisabled();
        LedgerTxn ltx(getApp().getLedgerTxnRoot());
        result = tx->checkValid(getApp().getAppConnector(), ltx, 0, 0, 0,
                                diagnostics);
    }
    REQUIRE(result->isSuccess());

    int64_t initBalance = getAccountBalance();
    int64_t baseFee = 100;
    int64_t chargedBeforeRefund =
        (tx->getFullFee() - tx->getInclusionFee()) + baseFee;
    REQUIRE(result->getFeeCharged() == chargedBeforeRefund);

    auto invokeResult = invokeTx(tx);
    REQUIRE(isSuccessResult(invokeResult));

    checkRefundableFee(initBalance, tx, expectedRefundableFeeCharged);
}

Hash
SorobanTest::uploadWasm(RustBuf const& wasm, SorobanResources& uploadResources,
                        int64_t additionalRefundableFee)
{
    SCBytes expectedWasm(wasm.data.begin(), wasm.data.end());
    Hash expectedWasmHash = sha256(expectedWasm);
    auto contractCodeLedgerKey = contractCodeKey(expectedWasmHash);

    bool wasUploaded = false;
    {
        LedgerTxn ltx(getApp().getLedgerTxnRoot());
        auto ltxe = ltx.loadWithoutRecord(contractCodeLedgerKey);
        if (ltxe)
        {
            wasUploaded = true;
        }
    }
    std::optional<uint32_t> previousLiveUntilLedger;
    if (wasUploaded)
    {
        // If Wasm has already been uploaded, we don't expect the TTL to
        // change, but otherwise run the transaction just as a smoke test.
        previousLiveUntilLedger = getTTL(contractCodeLedgerKey);
    }

    auto tx = makeSorobanWasmUploadTx(getApp(), *mRoot, wasm, uploadResources,
                                      1000, additionalRefundableFee);
    REQUIRE(isSuccessResult(invokeTx(tx)));

    // Verify the uploaded contract code is correct.
    {
        LedgerTxn ltx(getApp().getLedgerTxnRoot());
        auto ltxe = ltx.loadWithoutRecord(contractCodeLedgerKey);
        REQUIRE(ltxe);
        REQUIRE(ltxe.current().data.contractCode().code == expectedWasm);
    }
    if (!previousLiveUntilLedger)
    {
        // Check TTLs for contract code
        auto expectedLiveUntilLedger =
            getNetworkCfg().stateArchivalSettings().minPersistentTTL +
            getLCLSeq() - 1;
        REQUIRE(getTTL(contractCodeLedgerKey) == expectedLiveUntilLedger);
    }
    else
    {
        REQUIRE(getTTL(contractCodeLedgerKey) == *previousLiveUntilLedger);
    }
    return expectedWasmHash;
}

SCAddress
SorobanTest::createContract(ContractIDPreimage const& idPreimage,
                            ContractExecutable const& executable,
                            SorobanResources& createResources,
                            ConstructorParams const& constructorParams)
{
    auto tx =
        makeSorobanCreateContractTx(getApp(), *mRoot, idPreimage, executable,
                                    createResources, 1000, constructorParams);
    REQUIRE(isSuccessResult(invokeTx(tx)));
    Hash contractID = xdrSha256(
        makeFullContractIdPreimage(getApp().getNetworkID(), idPreimage));
    SCAddress contractAddress = makeContractAddress(contractID);
    auto contractInstanceKey = makeContractInstanceKey(contractAddress);
    {
        // Verify the created instance.
        LedgerTxn ltx(getApp().getLedgerTxnRoot());
        auto ltxe = ltx.load(contractInstanceKey);
        REQUIRE(ltxe);

        auto const& cd = ltxe.current().data.contractData();
        REQUIRE(cd.durability == CONTRACT_INSTANCE_ENTRY_DURABILITY);
        REQUIRE(cd.val.instance().executable == executable);
    }

    // Check TTLs for the contract instance.
    auto const& ses = getNetworkCfg().stateArchivalSettings();
    auto expectedLiveUntilLedger = ses.minPersistentTTL + getLCLSeq() - 1;
    REQUIRE(getTTL(contractInstanceKey) == expectedLiveUntilLedger);
    return contractAddress;
}

int64_t
SorobanTest::getRentFeeForExtension(xdr::xvector<LedgerKey> const& keys,
                                    uint32_t newLifetime)

{
    rust::Vec<CxxLedgerEntryRentChange> rustEntryRentChanges;
    LedgerTxn ltx(getApp().getLedgerTxnRoot());
    for (auto const& key : keys)
    {
        auto ltxe = ltx.loadWithoutRecord(key);
        REQUIRE(ltxe);
        auto entry = ltxe.current();
        uint32_t entrySize = static_cast<uint32_t>(xdr::xdr_size(entry));
        auto ttlLtxe = ltx.loadWithoutRecord(getTTLKey(key));
        REQUIRE(ttlLtxe);
        rustEntryRentChanges.emplace_back(
            createEntryRentChangeWithoutModification(
                entry, entrySize,
                /*entryLiveUntilLedger=*/
                ttlLtxe.current().data.ttl().liveUntilLedgerSeq,
                /*newLiveUntilLedger=*/getLCLSeq() + 1 + newLifetime,
                getLedgerVersion(), getNetworkCfg()));
    }
    return rust_bridge::compute_rent_fee(
        Config::CURRENT_LEDGER_PROTOCOL_VERSION, getLedgerVersion(),
        rustEntryRentChanges, getNetworkCfg().rustBridgeRentFeeConfiguration(),
        getLCLSeq());
}

Application&
SorobanTest::getApp() const
{
    return *mApp;
}

TestContract&
SorobanTest::deployWasmContract(RustBuf const& wasm,
                                std::optional<SorobanResources> uploadResources,
                                std::optional<SorobanResources> createResources,
                                int64_t additionalRefundableFee)
{
    return deployWasmContract(wasm, {}, uploadResources, createResources,
                              additionalRefundableFee);
}

TestContract&
SorobanTest::deployWasmContract(RustBuf const& wasm,
                                ConstructorParams const& constructorParams,
                                std::optional<SorobanResources> uploadResources,
                                std::optional<SorobanResources> createResources,
                                int64_t additionalRefundableFee)
{
    if (!uploadResources)
    {
        uploadResources = defaultUploadWasmResourcesWithoutFootprint(
            wasm, getLclProtocolVersion(getApp()));
    }

    if (!createResources)
    {
        createResources.emplace();
        createResources->instructions = 5'000'000;
        createResources->diskReadBytes =
            static_cast<uint32_t>(wasm.data.size() + 1000);
        createResources->writeBytes = 1000;
    }
    Hash wasmHash = uploadWasm(wasm, *uploadResources, additionalRefundableFee);
    SCAddress contractAddress = createContract(
        makeContractIDPreimage(*mRoot,
                               sha256(std::to_string(mContracts.size()))),
        makeWasmExecutable(wasmHash), *createResources, constructorParams);
    xdr::xvector<LedgerKey> contractKeys = {
        contractCodeKey(wasmHash), makeContractInstanceKey(contractAddress)};
    mContracts.emplace_back(
        std::make_unique<TestContract>(*this, contractAddress, contractKeys));
    return *mContracts.back();
}

SCAddress
SorobanTest::nextContractID()
{
    auto idPreimage = makeContractIDPreimage(
        *mRoot, sha256(std::to_string(mContracts.size())));
    auto contractID = xdrSha256(
        makeFullContractIdPreimage(getApp().getNetworkID(), idPreimage));
    return makeContractAddress(contractID);
}

TestContract&
SorobanTest::deployAssetContract(Asset const& asset)
{
    SorobanResources createResources;
    createResources.instructions = 400'000;
    createResources.diskReadBytes = 1000;
    createResources.writeBytes = 1000;

    SCAddress contractAddress =
        createContract(makeContractIDPreimage(asset),
                       makeAssetExecutable(asset), createResources, {});
    xdr::xvector<LedgerKey> contractKeys = {
        makeContractInstanceKey(contractAddress)};
    mContracts.emplace_back(
        std::make_unique<TestContract>(*this, contractAddress, contractKeys));
    return *mContracts.back();
}

TestAccount&
SorobanTest::getRoot()
{
    // TestAccount caches the next seqno in-memory, assuming all invoked TXs
    // succeed. This is not true for these tests, so we load the seqno from
    // disk to circumvent the cache.
    mRoot->loadSequenceNumber();
    return *mRoot;
}

TestAccount&
SorobanTest::getDummyAccount()
{
    return mDummyAccount;
}

SorobanNetworkConfig const&
SorobanTest::getNetworkCfg()
{
    return getApp().getLedgerManager().getMutableSorobanNetworkConfigForApply();
}

uint32_t
SorobanTest::getLedgerVersion() const
{
    return getApp()
        .getLedgerManager()
        .getLastClosedLedgerHeader()
        .header.ledgerVersion;
}

uint32_t
SorobanTest::getLCLSeq() const
{
    return getApp().getLedgerManager().getLastClosedLedgerNum();
}

TransactionFrameBaseConstPtr
SorobanTest::createExtendOpTx(SorobanResources const& resources,
                              uint32_t extendTo, uint32_t inclusionFee,
                              int64_t resourceFee, TestAccount* source)
{
    Operation op;
    op.body.type(EXTEND_FOOTPRINT_TTL);
    op.body.extendFootprintTTLOp().extendTo = extendTo;
    auto& acc = source ? *source : getRoot();
    return sorobanTransactionFrameFromOps(getApp().getNetworkID(), acc, {op},
                                          {}, resources, inclusionFee,
                                          resourceFee);
}

TransactionFrameBaseConstPtr
SorobanTest::createRestoreTx(SorobanResources const& resources,
                             uint32_t inclusionFee, int64_t resourceFee,
                             TestAccount* source)
{
    Operation op;
    op.body.type(RESTORE_FOOTPRINT);
    auto& acc = source ? *source : getRoot();
    return sorobanTransactionFrameFromOps(getApp().getNetworkID(), acc, {op},
                                          {}, resources, inclusionFee,
                                          resourceFee);
}

bool
SorobanTest::isTxValid(TransactionFrameBaseConstPtr tx)
{
    LedgerSnapshot ls(getApp());
    auto diagnostics = DiagnosticEventManager::createDisabled();
    auto ret =
        tx->checkValid(getApp().getAppConnector(), ls, 0, 0, 0, diagnostics);
    return ret->isSuccess();
}

TransactionResult
SorobanTest::invokeTx(TransactionFrameBaseConstPtr tx)
{
    {
        auto diagnostics = DiagnosticEventManager::createDisabled();
        LedgerSnapshot ls(getApp());
        REQUIRE(
            tx->checkValid(getApp().getAppConnector(), ls, 0, 0, 0, diagnostics)
                ->isSuccess());
    }

    auto resultSet = closeLedger(*mApp, {tx});
    REQUIRE(resultSet.results.size() == 1);

    return resultSet.results[0].result;
}

TransactionMetaFrame const&
SorobanTest::getLastTxMeta(size_t index) const
{
    return getApp().getLedgerManager().getLastClosedLedgerTxMeta().at(index);
}

LedgerCloseMetaFrame
SorobanTest::getLastLcm() const
{
    auto const& lcm =
        getApp().getLedgerManager().getLastClosedLedgerCloseMeta();
    REQUIRE(lcm.has_value());
    return *lcm;
}

uint32_t
SorobanTest::getTTL(LedgerKey const& k)
{
    LedgerTxn ltx(getApp().getLedgerTxnRoot());
    auto ltxe = ltx.loadWithoutRecord(k);
    REQUIRE(ltxe);

    auto ttlKey = getTTLKey(k);
    auto ttlLtxe = ltx.loadWithoutRecord(ttlKey);
    REQUIRE(ttlLtxe);
    return ttlLtxe.current().data.ttl().liveUntilLedgerSeq;
}

bool
SorobanTest::isEntryLive(LedgerKey const& k, uint32_t ledgerSeq)
{
    auto ttlKey = getTTLKey(k);
    LedgerTxn ltx(getApp().getLedgerTxnRoot());
    auto ttlLtxe = ltx.loadWithoutRecord(ttlKey);
    REQUIRE(ttlLtxe);
    return isLive(ttlLtxe.current(), ledgerSeq);
}

ExpirationStatus
SorobanTest::getEntryExpirationStatus(LedgerKey const& key)
{
    auto ttlKey = getTTLKey(key);
    LedgerSnapshot ls(getApp());
    if (auto lse = ls.load(ttlKey))
    {
        if (lse.current().data.ttl().liveUntilLedgerSeq <= getLCLSeq())
        {
            return ExpirationStatus::EXPIRED_IN_LIVE_STATE;
        }
        return ExpirationStatus::LIVE;
    }
    auto hotArchive = getApp()
                          .getBucketManager()
                          .getBucketSnapshotManager()
                          .copySearchableHotArchiveBucketListSnapshot();
    releaseAssert(hotArchive);
    if (hotArchive->load(key) != nullptr)
    {
        return ExpirationStatus::HOT_ARCHIVE;
    }
    return ExpirationStatus::NOT_FOUND;
}

void
SorobanTest::invokeRestoreOp(xdr::xvector<LedgerKey> const& readWrite,
                             int64_t expectedRefundableFeeCharged)
{
    SorobanResources resources;
    resources.footprint.readWrite = readWrite;
    resources.instructions = 0;
    resources.diskReadBytes = 10'000;
    resources.writeBytes = 10'000;

    auto resourceFee = 300'000 + 40'000 * readWrite.size();
    auto tx = createRestoreTx(resources, 1'000, resourceFee);
    invokeArchivalOp(tx, expectedRefundableFeeCharged);
}

void
SorobanTest::invokeExtendOp(xdr::xvector<LedgerKey> const& readOnly,
                            uint32_t extendTo,
                            std::optional<int64_t> expectedRefundableFeeCharged)
{
    if (!expectedRefundableFeeCharged)
    {
        expectedRefundableFeeCharged =
            getRentFeeForExtension(readOnly, extendTo);
    }

    SorobanResources extendResources;
    extendResources.footprint.readOnly = readOnly;
    extendResources.diskReadBytes = 10'000;

    auto resourceFee = DEFAULT_TEST_RESOURCE_FEE * readOnly.size() +
                       *expectedRefundableFeeCharged;
    auto tx = createExtendOpTx(extendResources, extendTo, 1'000, resourceFee);
    invokeArchivalOp(tx, *expectedRefundableFeeCharged);
}

SorobanSigner
SorobanTest::createContractSigner(TestContract const& contract,
                                  std::function<SCVal(uint256)> signFn)
{
    return SorobanSigner(*this, contract.getAddress(), contract.getKeys(),
                         signFn);
}

SorobanSigner
SorobanTest::createClassicAccountSigner(TestAccount const& account,
                                        std::vector<TestAccount*> signers)
{
    xdr::xvector<LedgerKey> accountKeys;
    // Account key has to be already present in the footprint, but the account
    // owner doesn't have to be the signer, so add the account key to the
    // footprint unconditionally.
    LedgerKey accountKey(LedgerEntryType::ACCOUNT);
    accountKey.account().accountID = account.getPublicKey();
    accountKeys.push_back(accountKey);
    for (const auto& signer : signers)
    {
        if (signer->getPublicKey().ed25519() !=
            account.getPublicKey().ed25519())
        {
            LedgerKey signerKey(LedgerEntryType::ACCOUNT);
            signerKey.account().accountID = signer->getPublicKey();
            accountKeys.push_back(signerKey);
        }
    }
    return SorobanSigner(*this, makeAccountAddress(account.getPublicKey()),
                         {accountKey}, [signers](uint256 payload) {
                             return signPayloadForClassicAccount(signers,
                                                                 payload);
                         });
}

AssetContractTestClient::AssetContractTestClient(SorobanTest& test,
                                                 Asset const& asset)
    : mContract(test.deployAssetContract(asset))
    , mAsset(asset)
    , mApp(mContract.getTest().getApp())
{
}

LedgerKey
AssetContractTestClient::makeBalanceKey(AccountID const& acc)
{
    LedgerKey balanceKey;
    if (mAsset.type() == ASSET_TYPE_NATIVE)
    {
        balanceKey.type(ACCOUNT);
        balanceKey.account().accountID = acc;
    }
    else
    {
        balanceKey.type(TRUSTLINE);
        balanceKey.trustLine().accountID = acc;
        balanceKey.trustLine().asset = assetToTrustLineAsset(mAsset);
    }

    return balanceKey;
}

LedgerKey
AssetContractTestClient::makeContractDataBalanceKey(SCAddress const& addr)
{
    SCVal val(SCV_ADDRESS);
    val.address() = addr;

    LedgerKey balanceKey(CONTRACT_DATA);
    balanceKey.contractData().contract = mContract.getAddress();

    balanceKey.contractData().key =
        makeVecSCVal({makeSymbolSCVal("Balance"), val});
    balanceKey.contractData().durability = ContractDataDurability::PERSISTENT;

    return balanceKey;
}

void
AssetContractTestClient::setLastEvent(
    TestContract::Invocation const& invocation, bool success)
{
    if (!success)
    {
        mLastEvent = std::nullopt;
        return;
    }
    auto const& sorobanEvents =
        invocation.getTxMeta().getSorobanContractEvents();
    REQUIRE(sorobanEvents.size() == 1);
    mLastEvent = sorobanEvents[0];
}

LedgerKey
AssetContractTestClient::makeBalanceKey(SCAddress const& addr)
{
    if (addr.type() == SC_ADDRESS_TYPE_ACCOUNT)
    {
        return makeBalanceKey(addr.accountId());
    }
    else
    {
        return makeContractDataBalanceKey(addr);
    }
}

LedgerKey
AssetContractTestClient::makeIssuerKey(Asset const& asset)
{
    LedgerKey issuerLedgerKey(ACCOUNT);
    issuerLedgerKey.account().accountID = getIssuer(asset);
    return issuerLedgerKey;
}

int64_t
AssetContractTestClient::getBalance(SCAddress const& addr)
{
    SCVal val(SCV_ADDRESS);
    val.address() = addr;

    return addr.type() == SC_ADDRESS_TYPE_ACCOUNT
               ? txtest::getBalance(mApp, addr.accountId(), mAsset)
               : getContractBalance(mApp, mContract.getAddress(), val);
}

SorobanInvocationSpec
AssetContractTestClient::defaultSpec() const
{
    return SorobanInvocationSpec()
        .setInstructions(2'000'000)
        .setReadBytes(2000)
        .setWriteBytes(2000);
}

TestContract const&
AssetContractTestClient::getContract() const
{
    return mContract;
}

std::optional<ContractEvent>
AssetContractTestClient::lastEvent() const
{
    return mLastEvent;
}

ContractEvent
AssetContractTestClient::makeTransferEvent(SCAddress const& from,
                                           SCAddress const& to, int64_t amount,
                                           std::optional<uint64_t> toMuxId)
{
    return txtest::makeTransferEvent(
        mContract.getAddress().contractId(), mAsset, from, to, amount,
        toMuxId ? std::optional<SCMapEntry>(SCMapEntry(
                      makeSymbolSCVal("to_muxed_id"), makeU64(*toMuxId)))
                : std::nullopt);
}

TransactionTestFramePtr
AssetContractTestClient::getTransferTx(TestAccount& fromAcc,
                                       SCAddress const& toAddr, int64_t amount,
                                       bool sourceIsRoot)
{
    SCVal toVal(SCV_ADDRESS);
    toVal.address() = toAddr;

    SCVal fromVal(SCV_ADDRESS);
    fromVal.address() = makeAccountAddress(fromAcc.getPublicKey());

    LedgerKey fromBalanceKey = makeBalanceKey(fromAcc.getPublicKey());
    LedgerKey toBalanceKey = makeBalanceKey(toAddr);

    auto spec = defaultSpec();

    if (mAsset.type() != ASSET_TYPE_NATIVE)
    {
        spec = spec.extendReadOnlyFootprint({makeIssuerKey(mAsset)});

        if (!(getIssuer(mAsset) == fromAcc.getPublicKey()))
        {
            spec = spec.extendReadWriteFootprint({fromBalanceKey});
        }

        if (toAddr.type() != SC_ADDRESS_TYPE_ACCOUNT ||
            !(getIssuer(mAsset) == toAddr.accountId()))
        {
            spec = spec.extendReadWriteFootprint({toBalanceKey});
        }
    }
    else
    {
        spec = spec.setReadWriteFootprint({fromBalanceKey, toBalanceKey});
    }

    auto invocation =
        mContract
            .prepareInvocation("transfer", {fromVal, toVal, makeI128(amount)},
                               spec)
            .withAuthorizedTopCall();
    if (!sourceIsRoot)
    {
        return invocation.createTx(&fromAcc);
    }
    auto tx = invocation.withOpSourceAccount(fromAcc.getPublicKey()).createTx();
    tx->addSignature(fromAcc.getSecretKey());
    return tx;
}

TestContract::Invocation
AssetContractTestClient::transferInvocation(TestAccount& fromAcc,
                                            SCAddress const& maybeMuxedToAddr,
                                            int64_t amount, bool& fromIsIssuer,
                                            bool& toIsIssuer, SCAddress& toAddr)
{
    SCVal fromVal(SCV_ADDRESS);
    fromVal.address() = makeAccountAddress(fromAcc.getPublicKey());

    SCVal toVal(SCV_ADDRESS);
    toAddr = maybeMuxedToAddr;
    if (maybeMuxedToAddr.type() != SCAddressType::SC_ADDRESS_TYPE_MUXED_ACCOUNT)
    {
        toVal.address() = maybeMuxedToAddr;
    }
    else
    {
        PublicKey pk;
        pk.ed25519() = maybeMuxedToAddr.muxedAccount().ed25519;
        toVal.address() =
            makeMuxedAccountAddress(pk, maybeMuxedToAddr.muxedAccount().id);
        toAddr = makeAccountAddress(pk);
    }

    LedgerKey fromBalanceKey = makeBalanceKey(fromAcc.getPublicKey());
    LedgerKey toBalanceKey = makeBalanceKey(toAddr);

    auto spec = defaultSpec();

    fromIsIssuer = false;
    toIsIssuer = false;
    if (mAsset.type() != ASSET_TYPE_NATIVE)
    {
        spec = spec.extendReadOnlyFootprint({makeIssuerKey(mAsset)});

        if (getIssuer(mAsset) == fromAcc.getPublicKey())
        {
            fromIsIssuer = true;
        }
        else
        {
            spec = spec.extendReadWriteFootprint({fromBalanceKey});
        }

        if (toAddr.type() == SC_ADDRESS_TYPE_ACCOUNT &&
            getIssuer(mAsset) == toAddr.accountId())
        {
            toIsIssuer = true;
        }
        else
        {
            if (toBalanceKey != fromBalanceKey)
            {
                spec = spec.extendReadWriteFootprint({toBalanceKey});
            }
        }
    }
    else
    {
        spec = spec.setReadWriteFootprint({fromBalanceKey, toBalanceKey});
    }

    auto invocation =
        mContract
            .prepareInvocation("transfer", {fromVal, toVal, makeI128(amount)},
                               spec)
            .withAuthorizedTopCall();
    return invocation;
}

bool
AssetContractTestClient::transfer(TestAccount& fromAcc,
                                  SCAddress const& maybeMuxedToAddr,
                                  int64_t amount)
{
    bool fromIsIssuer;
    bool toIsIssuer;
    SCAddress toAddr;
    auto invocation = transferInvocation(fromAcc, maybeMuxedToAddr, amount,
                                         fromIsIssuer, toIsIssuer, toAddr);

    auto preTransferFromBalance = mAsset.type() == ASSET_TYPE_NATIVE
                                      ? fromAcc.getBalance()
                                      : fromAcc.getTrustlineBalance(mAsset);
    auto preTransferToBalance = getBalance(toAddr);
    bool success = invocation.invoke(&fromAcc);
    setLastEvent(invocation, success);

    auto postTransferFromBalance = mAsset.type() == ASSET_TYPE_NATIVE
                                       ? fromAcc.getBalance()
                                       : fromAcc.getTrustlineBalance(mAsset);

    auto postTransferToBalance = getBalance(toAddr);
    if (success)
    {

        if (!fromIsIssuer)
        {
            int64_t expectedBalance = preTransferFromBalance - amount;
            if (mAsset.type() == ASSET_TYPE_NATIVE)
            {
                expectedBalance -= invocation.getFeeCharged();
            }
            REQUIRE(postTransferFromBalance == expectedBalance);
        }
        if (!toIsIssuer)
        {
            REQUIRE(postTransferToBalance - amount == preTransferToBalance);
        }

        {
            // From is an account so it should never have a contract data
            // balance
            LedgerTxn ltx(mApp.getLedgerTxnRoot());
            auto fromAddr = makeAccountAddress(fromAcc.getPublicKey());
            REQUIRE(!ltx.load(makeContractDataBalanceKey(fromAddr)));
        }

        if (toIsIssuer)
        {
            // make sure we didn't create an entry for the issuer
            LedgerTxn ltx(mApp.getLedgerTxnRoot());
            REQUIRE(!ltx.load(makeContractDataBalanceKey(toAddr)));
        }
    }
    else
    {
        auto const& contractEvents =
            invocation.getTxMeta().getSorobanContractEvents();
        REQUIRE(contractEvents.size() == 0);
        if (mApp.getConfig().ENABLE_SOROBAN_DIAGNOSTIC_EVENTS)
        {
            // Check for a contract error in the second event (the first should
            // be an `fn_call` event)

            auto const& diagnosticEvents =
                invocation.getTxMeta().getDiagnosticEvents();
            REQUIRE(diagnosticEvents.size() > 1);

            auto const& contract_ev = diagnosticEvents.at(1);
            REQUIRE(!contract_ev.inSuccessfulContractCall);
            REQUIRE(contract_ev.event.type == ContractEventType::DIAGNOSTIC);
            auto const& topics = contract_ev.event.body.v0().topics.at(1);
            REQUIRE(topics.type() == SCV_ERROR);
            REQUIRE(topics.error().type() == SCE_CONTRACT);
        }
        int64_t expectedFromBalance = preTransferFromBalance;
        if (mAsset.type() == ASSET_TYPE_NATIVE)
        {
            expectedFromBalance -= invocation.getFeeCharged();
        }

        REQUIRE(postTransferFromBalance == expectedFromBalance);
        REQUIRE(postTransferToBalance == preTransferToBalance);
    }
    return success;
}

bool
AssetContractTestClient::mint(TestAccount& admin, SCAddress const& toAddr,
                              int64_t amount)
{
    SCVal toVal(SCV_ADDRESS);
    toVal.address() = toAddr;

    LedgerKey toBalanceKey = makeBalanceKey(toAddr);

    auto spec = defaultSpec().setReadWriteFootprint({toBalanceKey});

    if (mAsset.type() != ASSET_TYPE_NATIVE)
    {
        LedgerKey issuerLedgerKey(ACCOUNT);
        issuerLedgerKey.account().accountID = getIssuer(mAsset);
        spec = spec.extendReadOnlyFootprint({issuerLedgerKey});
    }

    auto preMintBalance = getBalance(toAddr);
    auto invocation =
        mContract.prepareInvocation("mint", {toVal, makeI128(amount)}, spec)
            .withAuthorizedTopCall();
    bool success = invocation.invoke(&admin);
    setLastEvent(invocation, success);
    auto postMintBalance = getBalance(toAddr);
    if (success)
    {
        REQUIRE(postMintBalance - amount == preMintBalance);
    }
    else
    {
        REQUIRE(postMintBalance == preMintBalance);
    }
    return success;
}

bool
AssetContractTestClient::burn(TestAccount& from, int64_t amount)
{
    auto fromAddr = makeAccountAddress(from.getPublicKey());

    SCVal fromVal(SCV_ADDRESS);
    fromVal.address() = fromAddr;

    LedgerKey fromBalanceKey;
    if (mAsset.type() == ASSET_TYPE_NATIVE)
    {
        fromBalanceKey.type(ACCOUNT);
        fromBalanceKey.account().accountID = fromAddr.accountId();
    }
    else
    {
        fromBalanceKey.type(TRUSTLINE);
        fromBalanceKey.trustLine().accountID = fromAddr.accountId();
        fromBalanceKey.trustLine().asset = assetToTrustLineAsset(mAsset);
    }

    auto spec = defaultSpec();

    bool isIssuer = getIssuer(mAsset) == from.getPublicKey();
    if (!isIssuer)
    {
        spec = spec.extendReadWriteFootprint({fromBalanceKey});
    }

    auto preBurnBalance = getBalance(fromAddr);
    auto invocation =
        mContract.prepareInvocation("burn", {fromVal, makeI128(amount)}, spec)
            .withAuthorizedTopCall();
    bool success = invocation.invoke(&from);
    setLastEvent(invocation, success);
    auto postBurnBalance = getBalance(fromAddr);
    if (success)
    {
        REQUIRE(preBurnBalance - amount == postBurnBalance);
    }
    else
    {
        REQUIRE(preBurnBalance == postBurnBalance);
    }
    return success;
}

bool
AssetContractTestClient::clawback(TestAccount& admin, SCAddress const& fromAddr,
                                  int64_t amount)
{
    SCVal fromVal(SCV_ADDRESS);
    fromVal.address() = fromAddr;

    LedgerKey fromBalanceKey = makeBalanceKey(fromAddr);
    auto spec = defaultSpec().setReadWriteFootprint({fromBalanceKey});

    if (mAsset.type() != ASSET_TYPE_NATIVE)
    {
        spec = spec.extendReadOnlyFootprint({makeIssuerKey(mAsset)});
    }

    auto preClawbackBalance = getBalance(fromAddr);
    auto invocation =
        mContract
            .prepareInvocation("clawback", {fromVal, makeI128(amount)}, spec)
            .withAuthorizedTopCall();
    bool success = invocation.invoke(&admin);
    setLastEvent(invocation, success);
    auto postClawbackBalance = getBalance(fromAddr);
    if (success)
    {
        REQUIRE(preClawbackBalance - amount == postClawbackBalance);
    }
    else
    {
        REQUIRE(preClawbackBalance == postClawbackBalance);
    }
    return success;
}

ContractStorageTestClient::ContractStorageTestClient(
    SorobanTest& test, int64_t additionalRefundableFee)
    : mContract(test.deployWasmContract(
          rust_bridge::get_test_wasm_contract_data(), std::nullopt,
          std::nullopt, additionalRefundableFee))
{
}

TestContract&
ContractStorageTestClient::getContract() const
{
    return mContract;
}

SorobanInvocationSpec
ContractStorageTestClient::defaultSpecWithoutFootprint()
{
    return SorobanInvocationSpec()
        .setInstructions(4'000'000)
        .setReadBytes(10'000)
        .setNonRefundableResourceFee(0)
        .setRefundableResourceFee(1'000'000);
}

SorobanInvocationSpec
ContractStorageTestClient::readKeySpec(std::string const& key,
                                       ContractDataDurability durability) const
{
    return defaultSpecWithoutFootprint().setReadOnlyFootprint(
        {mContract.getDataKey(makeSymbolSCVal(key), durability)});
}

SorobanInvocationSpec
ContractStorageTestClient::writeKeySpec(std::string const& key,
                                        ContractDataDurability durability) const
{
    return defaultSpecWithoutFootprint()
        .setReadWriteFootprint(
            {mContract.getDataKey(makeSymbolSCVal(key), durability)})
        .setWriteBytes(1000);
}

uint32_t
ContractStorageTestClient::getTTL(std::string const& key,
                                  ContractDataDurability durability)
{
    return mContract.getTest().getTTL(
        mContract.getDataKey(makeSymbolSCVal(key), durability));
}

bool
ContractStorageTestClient::isEntryLive(std::string const& key,
                                       ContractDataDurability durability,
                                       uint32_t ledgerSeq)
{
    return mContract.getTest().isEntryLive(
        mContract.getDataKey(makeSymbolSCVal(key), durability), ledgerSeq);
}

ExpirationStatus
ContractStorageTestClient::getEntryExpirationStatus(
    std::string const& key, ContractDataDurability durability)
{
    return mContract.getTest().getEntryExpirationStatus(
        mContract.getDataKey(makeSymbolSCVal(key), durability));
}

TestContract::Invocation
ContractStorageTestClient::putInvocation(
    std::string const& key, ContractDataDurability durability, uint64_t val,
    std::optional<SorobanInvocationSpec> spec)
{
    if (!spec)
    {
        spec = writeKeySpec(key, durability);
    }

    std::string funcStr = durability == ContractDataDurability::TEMPORARY
                              ? "put_temporary"
                              : "put_persistent";
    return mContract.prepareInvocation(
        funcStr, {makeSymbolSCVal(key), makeU64SCVal(val)}, *spec);
}

InvokeHostFunctionResultCode
ContractStorageTestClient::put(std::string const& key,
                               ContractDataDurability durability, uint64_t val,
                               std::optional<SorobanInvocationSpec> spec)
{
    auto invocation = putInvocation(key, durability, val, spec);
    invocation.withExactNonRefundableResourceFee().invoke();
    return *invocation.getResultCode();
}

InvokeHostFunctionResultCode
ContractStorageTestClient::get(std::string const& key,
                               ContractDataDurability durability,
                               std::optional<uint64_t> expectValue,
                               std::optional<SorobanInvocationSpec> spec)
{
    if (!spec)
    {
        spec = readKeySpec(key, durability);
    }

    std::string funcStr = durability == ContractDataDurability::TEMPORARY
                              ? "get_temporary"
                              : "get_persistent";
    auto invocation =
        mContract.prepareInvocation(funcStr, {makeSymbolSCVal(key)}, *spec);
    bool isSuccess = invocation.withExactNonRefundableResourceFee().invoke();
    if (isSuccess && expectValue)
    {
        REQUIRE(*expectValue == invocation.getReturnValue().u64());
    }
    return *invocation.getResultCode();
}

InvokeHostFunctionResultCode
ContractStorageTestClient::has(std::string const& key,
                               ContractDataDurability durability,
                               std::optional<bool> expectHas,
                               std::optional<SorobanInvocationSpec> spec)
{
    if (!spec)
    {
        spec = readKeySpec(key, durability);
    }

    std::string funcStr = durability == ContractDataDurability::TEMPORARY
                              ? "has_temporary"
                              : "has_persistent";

    auto invocation =
        mContract.prepareInvocation(funcStr, {makeSymbolSCVal(key)}, *spec);
    bool isSuccess = invocation.withExactNonRefundableResourceFee().invoke();
    if (isSuccess && expectHas)
    {
        REQUIRE(invocation.getReturnValue().b() == expectHas);
    }
    return *invocation.getResultCode();
}

TestContract::Invocation
ContractStorageTestClient::delInvocation(
    std::string const& key, ContractDataDurability durability,
    std::optional<SorobanInvocationSpec> spec)
{
    if (!spec)
    {
        // Even though deletion is technically a write, we don't charge any
        // bytes for it and thus don't need to add write bytes to resources.
        spec = writeKeySpec(key, durability).setWriteBytes(0);
    }

    std::string funcStr = durability == ContractDataDurability::TEMPORARY
                              ? "del_temporary"
                              : "del_persistent";

    return mContract.prepareInvocation(funcStr, {makeSymbolSCVal(key)}, *spec);
}

InvokeHostFunctionResultCode
ContractStorageTestClient::del(std::string const& key,
                               ContractDataDurability durability,
                               std::optional<SorobanInvocationSpec> spec)
{

    auto invocation = delInvocation(key, durability, spec);
    invocation.withExactNonRefundableResourceFee().invoke();
    return *invocation.getResultCode();
}

InvokeHostFunctionResultCode
ContractStorageTestClient::extend(std::string const& key,
                                  ContractDataDurability durability,
                                  uint32_t threshold, uint32_t extendTo,
                                  std::optional<SorobanInvocationSpec> spec)
{
    if (!spec)
    {
        spec = readKeySpec(key, durability);
    }

    std::string funcStr = durability == ContractDataDurability::TEMPORARY
                              ? "extend_temporary"
                              : "extend_persistent";
    auto invocation = mContract.prepareInvocation(
        funcStr, {makeSymbolSCVal(key), makeU32(threshold), makeU32(extendTo)},
        *spec);
    invocation.withExactNonRefundableResourceFee().invoke();
    return *invocation.getResultCode();
}

TestContract::Invocation
ContractStorageTestClient::resizeStorageAndExtendInvocation(
    std::string const& key, uint32_t numKiloBytes, uint32_t thresh,
    uint32_t extendTo, std::optional<SorobanInvocationSpec> spec)
{
    if (!spec)
    {
        spec = writeKeySpec(key, ContractDataDurability::PERSISTENT)
                   .setWriteBytes(numKiloBytes * 1024 + 200);
    }

    std::string funcStr = "replace_with_bytes_and_extend";
    return mContract.prepareInvocation(funcStr,
                                       {makeSymbolSCVal(key),
                                        makeU32(numKiloBytes), makeU32(thresh),
                                        makeU32(extendTo)},
                                       *spec);
}

InvokeHostFunctionResultCode
ContractStorageTestClient::resizeStorageAndExtend(
    std::string const& key, uint32_t numKiloBytes, uint32_t thresh,
    uint32_t extendTo, std::optional<SorobanInvocationSpec> spec)
{
    auto invocation = resizeStorageAndExtendInvocation(key, numKiloBytes,
                                                       thresh, extendTo, spec);
    invocation.withExactNonRefundableResourceFee().invoke();
    return *invocation.getResultCode();
}

SorobanSigner::SorobanSigner(SorobanTest& test, SCAddress const& address,
                             xdr::xvector<LedgerKey> const& keys,
                             std::function<SCVal(uint256)> signFn)
    : mTest(test), mSignFn(signFn), mAddress(address), mKeys(keys)
{
}

SorobanCredentials
SorobanSigner::sign(SorobanAuthorizedInvocation const& invocation) const
{
    SorobanCredentials fullCredentials(
        SorobanCredentialsType::SOROBAN_CREDENTIALS_ADDRESS);
    auto& credentials = fullCredentials.address();
    credentials.nonce = uniform_int_distribution<int64_t>()(Catch::rng());
    credentials.signatureExpirationLedger = mTest.getLCLSeq() + 10'000;
    credentials.address = mAddress;

    HashIDPreimage signaturePreimage(
        EnvelopeType::ENVELOPE_TYPE_SOROBAN_AUTHORIZATION);
    auto& preimage = signaturePreimage.sorobanAuthorization();
    preimage.invocation = invocation;
    preimage.networkID = mTest.getApp().getNetworkID();
    preimage.nonce = credentials.nonce;
    preimage.signatureExpirationLedger = credentials.signatureExpirationLedger;

    credentials.signature = mSignFn(xdrSha256(signaturePreimage));
    return fullCredentials;
}

SCVal
SorobanSigner::getAddressVal() const
{
    return makeAddressSCVal(mAddress);
}

xdr::xvector<LedgerKey> const&
SorobanSigner::getLedgerKeys() const
{
    return mKeys;
}

AuthTestTreeNode::AuthTestTreeNode(SCAddress const& contract)
    : mContractAddress(contract)
{
}

AuthTestTreeNode&
AuthTestTreeNode::add(std::vector<AuthTestTreeNode> children)
{
    mChildren.insert(mChildren.end(), children.begin(), children.end());
    return *this;
}

void
AuthTestTreeNode::setAddress(SCAddress const& address)
{
    mContractAddress = address;
}

SCVal
AuthTestTreeNode::toSCVal(int addressCount) const
{
    std::vector<SCVal> children;
    for (auto const& child : mChildren)
    {
        children.emplace_back(child.toSCVal(addressCount));
    }

    SCVal node(SCV_MAP);
    auto& fields = node.map().activate();

    fields.emplace_back(makeSymbolSCVal("children"), makeVecSCVal(children));
    fields.emplace_back(makeSymbolSCVal("contract"),
                        makeAddressSCVal(mContractAddress));
    fields.emplace_back(
        makeSymbolSCVal("need_auth"),
        makeVecSCVal(std::vector<SCVal>(addressCount, makeBool(true))));
    fields.emplace_back(makeSymbolSCVal("try_call"), makeBool(false));
    return node;
}

SorobanAuthorizedInvocation
AuthTestTreeNode::toAuthorizedInvocation() const
{
    SorobanAuthorizedInvocation invocation;
    auto& function = invocation.function.contractFn();
    function.contractAddress = mContractAddress;
    function.functionName = makeSymbol("tree_fn");

    for (auto const& child : mChildren)
    {
        invocation.subInvocations.emplace_back(child.toAuthorizedInvocation());
    }
    return invocation;
}

} // namespace txtext
} // namespace stellar
