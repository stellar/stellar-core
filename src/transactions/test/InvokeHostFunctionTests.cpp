// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Logging.h"
#include "xdr/Stellar-transaction.h"
#include <iterator>
#include <stdexcept>
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION

#include "crypto/Random.h"
#include "crypto/SecretKey.h"
#include "herder/Herder.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "ledger/test/LedgerTestUtils.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/CommandHandler.h"
#include "rust/RustBridge.h"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/InvokeHostFunctionOpFrame.h"
#include "transactions/SignatureUtils.h"
#include "transactions/TransactionUtils.h"
#include "util/Decoder.h"
#include "util/TmpDir.h"
#include "util/XDRCereal.h"
#include "xdr/Stellar-contract.h"
#include "xdr/Stellar-ledger-entries.h"
#include <autocheck/autocheck.hpp>
#include <fmt/format.h>
#include <limits>
#include <type_traits>
#include <variant>

using namespace stellar;
using namespace stellar::txtest;

SCVal
makeWasmRefScContractCode(Hash const& hash)
{
    SCVal val(SCValType::SCV_CONTRACT_INSTANCE);
    val.instance().executable.type(
        ContractExecutableType::CONTRACT_EXECUTABLE_WASM);
    val.instance().executable.wasm_hash() = hash;
    return val;
}

SCAddress
makeContractAddress(Hash const& hash)
{
    SCAddress addr(SC_ADDRESS_TYPE_CONTRACT);
    addr.contractId() = hash;
    return addr;
}

SCVal
makeContractAddressSCVal(SCAddress const& address)
{
    SCVal val(SCValType::SCV_ADDRESS);
    val.address() = address;
    return val;
}

static SCVal
makeI32(int32_t i32)
{
    SCVal val(SCV_I32);
    val.i32() = i32;
    return val;
}

static SCVal
makeU64(uint64_t u64)
{
    SCVal val(SCV_U64);
    val.u64() = u64;
    return val;
}

static SCVal
makeI128(uint64_t u64)
{
    Int128Parts p;
    p.hi = 0;
    p.lo = u64;

    SCVal val(SCV_I128);
    val.i128() = p;
    return val;
}

static SCVal
makeSymbolSCVal(std::string const& str)
{
    SCVal val(SCV_SYMBOL);
    val.sym().assign(str.begin(), str.end());
    return val;
}

static SCSymbol
makeSymbol(std::string const& str)
{
    SCSymbol val;
    val.assign(str.begin(), str.end());
    return val;
}

static SCVal
makeU32(uint32_t u32)
{
    SCVal val(SCV_U32);
    val.u32() = u32;
    return val;
}

static SCVal
makeBytes(SCBytes bytes)
{
    SCVal val(SCV_BYTES);
    val.bytes() = bytes;
    return val;
}

static uint32_t
getLedgerSeq(Application& app)
{
    LedgerTxn ltx(app.getLedgerTxnRoot());
    return ltx.loadHeader().current().ledgerSeq;
}

static LedgerEntry
loadStorageEntry(Application& app, SCAddress const& contractID,
                 std::string const& key, ContractDataDurability type)
{
    auto keySymbol = makeSymbolSCVal(key);
    LedgerTxn ltx(app.getLedgerTxnRoot());
    auto ltxe = loadContractData(ltx, contractID, keySymbol, type);
    REQUIRE(ltxe);
    return ltxe.current();
}

static void
submitTxToUploadWasm(Application& app, Operation const& op,
                     SorobanResources const& resources,
                     Hash const& expectedWasmHash,
                     xdr::opaque_vec<> const& expectedWasm, uint32_t fee,
                     uint32_t refundableFee)
{
    // submit operation
    auto root = TestAccount::createRoot(app);
    auto tx = sorobanTransactionFrameFromOps(app.getNetworkID(), root, {op}, {},
                                             resources, fee, refundableFee);
    LedgerTxn ltx(app.getLedgerTxnRoot());
    TransactionMetaFrame txm(ltx.loadHeader().current().ledgerVersion);
    REQUIRE(tx->checkValid(app, ltx, 0, 0, 0));
    REQUIRE(tx->apply(app, ltx, txm));
    ltx.commit();

    // verify contract code is correct
    LedgerTxn ltx2(app.getLedgerTxnRoot());
    auto ltxe = loadContractCode(ltx2, expectedWasmHash);
    REQUIRE(ltxe);

    auto const& body = ltxe.current().data.contractCode().body;
    REQUIRE(body.bodyType() == DATA_ENTRY);
    REQUIRE(body.code() == expectedWasm);
}

static void
submitTxToCreateContract(Application& app, Operation const& op,
                         SorobanResources const& resources,
                         Hash const& contractID, SCVal const& executableKey,
                         Hash const& expectedWasmHash, uint32_t fee,
                         uint32_t refundableFee)
{
    // submit operation
    auto root = TestAccount::createRoot(app);
    auto tx = sorobanTransactionFrameFromOps(app.getNetworkID(), root, {op}, {},
                                             resources, fee, refundableFee);
    LedgerTxn ltx(app.getLedgerTxnRoot());
    TransactionMetaFrame txm(ltx.loadHeader().current().ledgerVersion);
    REQUIRE(tx->checkValid(app, ltx, 0, 0, 0));
    REQUIRE(tx->apply(app, ltx, txm));
    ltx.commit();

    // verify contract code reference is correct
    LedgerTxn ltx2(app.getLedgerTxnRoot());
    SCAddress contract = makeContractAddress(contractID);
    auto ltxe = loadContractData(ltx2, contract, executableKey,
                                 CONTRACT_INSTANCE_CONTRACT_DURABILITY);
    REQUIRE(ltxe);

    auto const& cd = ltxe.current().data.contractData();
    REQUIRE(cd.durability == CONTRACT_INSTANCE_CONTRACT_DURABILITY);
    REQUIRE(cd.body.bodyType() == DATA_ENTRY);
    REQUIRE(cd.body.data().val == makeWasmRefScContractCode(expectedWasmHash));
}

static xdr::xvector<LedgerKey>
deployContractWithSourceAccountWithResources(Application& app,
                                             RustBuf const& contractWasm,
                                             SorobanResources uploadResources,
                                             SorobanResources createResources,
                                             uint256 salt = sha256("salt"))
{
    auto root = TestAccount::createRoot(app);

    // Upload contract code
    Operation uploadOp;
    uploadOp.body.type(INVOKE_HOST_FUNCTION);
    auto& uploadHF = uploadOp.body.invokeHostFunctionOp().hostFunction;
    uploadHF.type(HOST_FUNCTION_TYPE_UPLOAD_CONTRACT_WASM);
    uploadHF.wasm().assign(contractWasm.data.begin(), contractWasm.data.end());

    LedgerKey contractCodeLedgerKey;
    contractCodeLedgerKey.type(CONTRACT_CODE);
    contractCodeLedgerKey.contractCode().hash = sha256(uploadHF.wasm());
    uploadResources.footprint.readWrite = {contractCodeLedgerKey};
    submitTxToUploadWasm(app, uploadOp, uploadResources,
                         contractCodeLedgerKey.contractCode().hash,
                         uploadHF.wasm(), 100'000, 1'200);

    // Check expirations for contract code
    {
        LedgerTxn ltx(app.getLedgerTxnRoot());
        auto networkConfig =
            app.getLedgerManager().getSorobanNetworkConfig(ltx);
        auto ledgerSeq = ltx.loadHeader().current().ledgerSeq;
        auto expectedExpiration = networkConfig.stateExpirationSettings()
                                      .minPersistentEntryExpiration +
                                  ledgerSeq - 1;

        auto codeLtxe = ltx.load(contractCodeLedgerKey);
        REQUIRE(codeLtxe);
        REQUIRE(codeLtxe.current().data.contractCode().expirationLedgerSeq ==
                expectedExpiration);
    }

    // Deploy the contract instance
    ContractIDPreimage idPreimage(CONTRACT_ID_PREIMAGE_FROM_ADDRESS);
    idPreimage.fromAddress().address.type(SC_ADDRESS_TYPE_ACCOUNT);
    idPreimage.fromAddress().address.accountId().ed25519() =
        root.getPublicKey().ed25519();
    idPreimage.fromAddress().salt = salt;
    HashIDPreimage fullPreImage;
    fullPreImage.type(ENVELOPE_TYPE_CONTRACT_ID);
    fullPreImage.contractID().contractIDPreimage = idPreimage;
    fullPreImage.contractID().networkID = app.getNetworkID();
    auto contractID = xdrSha256(fullPreImage);

    Operation createOp;
    createOp.body.type(INVOKE_HOST_FUNCTION);
    auto& createHF = createOp.body.invokeHostFunctionOp().hostFunction;
    createHF.type(HOST_FUNCTION_TYPE_CREATE_CONTRACT);
    auto& createContractArgs = createHF.createContract();
    createContractArgs.contractIDPreimage = idPreimage;
    createContractArgs.executable.type(CONTRACT_EXECUTABLE_WASM);
    createContractArgs.executable.wasm_hash() =
        contractCodeLedgerKey.contractCode().hash;

    SorobanAuthorizationEntry auth;
    auth.credentials.type(SOROBAN_CREDENTIALS_SOURCE_ACCOUNT);
    auth.rootInvocation.function.type(
        SOROBAN_AUTHORIZED_FUNCTION_TYPE_CREATE_CONTRACT_HOST_FN);
    auth.rootInvocation.function.createContractHostFn().contractIDPreimage =
        idPreimage;
    auth.rootInvocation.function.createContractHostFn().executable.type(
        CONTRACT_EXECUTABLE_WASM);
    auth.rootInvocation.function.createContractHostFn().executable.wasm_hash() =
        contractCodeLedgerKey.contractCode().hash;
    createOp.body.invokeHostFunctionOp().auth = {auth};

    SCVal scContractSourceRefKey(SCValType::SCV_LEDGER_KEY_CONTRACT_INSTANCE);

    LedgerKey contractSourceRefLedgerKey;
    contractSourceRefLedgerKey.type(CONTRACT_DATA);
    contractSourceRefLedgerKey.contractData().contract.type(
        SC_ADDRESS_TYPE_CONTRACT);
    contractSourceRefLedgerKey.contractData().contract.contractId() =
        contractID;
    contractSourceRefLedgerKey.contractData().key = scContractSourceRefKey;
    contractSourceRefLedgerKey.contractData().durability =
        CONTRACT_INSTANCE_CONTRACT_DURABILITY;

    createResources.footprint.readOnly = {contractCodeLedgerKey};
    createResources.footprint.readWrite = {contractSourceRefLedgerKey};

    submitTxToCreateContract(
        app, createOp, createResources, contractID, scContractSourceRefKey,
        contractCodeLedgerKey.contractCode().hash, 100'000, 1200);

    // Check expirations for contract instance
    LedgerTxn ltx(app.getLedgerTxnRoot());
    auto networkConfig = app.getLedgerManager().getSorobanNetworkConfig(ltx);
    auto ledgerSeq = ltx.loadHeader().current().ledgerSeq;
    auto expectedExpiration =
        networkConfig.stateExpirationSettings().minPersistentEntryExpiration +
        ledgerSeq - 1;

    auto instanceLtxe = ltx.load(contractSourceRefLedgerKey);
    REQUIRE(instanceLtxe);
    REQUIRE(instanceLtxe.current().data.contractData().expirationLedgerSeq ==
            expectedExpiration);

    return {contractSourceRefLedgerKey, contractCodeLedgerKey};
}

static xdr::xvector<LedgerKey>
deployContractWithSourceAccount(Application& app, RustBuf const& contractWasm,
                                uint256 salt = sha256("salt"))
{
    SorobanResources uploadResources{};
    uploadResources.instructions = 200'000;
    uploadResources.readBytes = 1000;
    uploadResources.writeBytes = 5000;

    SorobanResources createResources{};
    createResources.instructions = 200'000;
    createResources.readBytes = 5000;
    createResources.writeBytes = 5000;
    return deployContractWithSourceAccountWithResources(
        app, contractWasm, uploadResources, createResources, salt);
}

TEST_CASE("basic contract invocation", "[tx][soroban]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.EXPERIMENTAL_BUCKETLIST_DB = false;
    auto app = createTestApplication(clock, cfg);
    overrideSorobanNetworkConfigForTest(*app);

    auto root = TestAccount::createRoot(*app);
    int64_t initBalance = root.getBalance();

    auto const addI32Wasm = rust_bridge::get_test_wasm_add_i32();

    auto contractKeys = deployContractWithSourceAccount(*app, addI32Wasm);
    auto const& contractID = contractKeys[0].contractData().contract;
    auto isValid = [&](SorobanResources const& resources,
                       SCAddress const& address, SCSymbol const& functionName,
                       std::vector<SCVal> const& args) -> bool {
        Operation op;
        op.body.type(INVOKE_HOST_FUNCTION);
        auto& ihf = op.body.invokeHostFunctionOp().hostFunction;
        ihf.type(HOST_FUNCTION_TYPE_INVOKE_CONTRACT);
        ihf.invokeContract().contractAddress = address;
        ihf.invokeContract().functionName = functionName;
        ihf.invokeContract().args.assign(args.begin(), args.end());

        auto tx = sorobanTransactionFrameFromOps(
            app->getNetworkID(), root, {op}, {}, resources, 100'000, 1200);

        LedgerTxn ltx(app->getLedgerTxnRoot());
        return tx->checkValid(*app, ltx, 0, 0, 0);
    };

    auto call = [&](SorobanResources const& resources, uint32_t refundableFee,
                    SCAddress const& address, SCSymbol const& functionName,
                    std::vector<SCVal> const& args, bool success) {
        Operation op;
        op.body.type(INVOKE_HOST_FUNCTION);
        auto& ihf = op.body.invokeHostFunctionOp().hostFunction;
        ihf.type(HOST_FUNCTION_TYPE_INVOKE_CONTRACT);
        ihf.invokeContract().contractAddress = address;
        ihf.invokeContract().functionName = functionName;
        ihf.invokeContract().args.assign(args.begin(), args.end());

        auto tx =
            sorobanTransactionFrameFromOps(app->getNetworkID(), root, {op}, {},
                                           resources, 100'000, refundableFee);
        {
            LedgerTxn ltx(app->getLedgerTxnRoot());
            REQUIRE(tx->checkValid(*app, ltx, 0, 0, 0));
            ltx.commit();
        }
        TransactionMetaFrame txm(app->getLedgerManager()
                                     .getLastClosedLedgerHeader()
                                     .header.ledgerVersion);
        if (success)
        {
            REQUIRE(tx->getFullFee() == 100'000);
            REQUIRE(tx->getInclusionFee() == 66'102);
            // Initially we store in result the charge for resources plus
            // minimum inclusion  fee bid (currently equivalent to the network
            // `baseFee` of 100).
            int64_t baseCharged =
                (tx->getFullFee() - tx->getInclusionFee()) + 100;
            REQUIRE(tx->getResult().feeCharged == baseCharged);
            {
                LedgerTxn ltx(app->getLedgerTxnRoot());
                // Imitate surge pricing by charging at a higher rate than base
                // fee.
                tx->processFeeSeqNum(ltx, 300);
                ltx.commit();
            }
            // The resource and the base fee are charged, with additional
            // surge pricing fee.
            int64_t balanceAfterFeeCharged = root.getBalance();
            REQUIRE(initBalance - balanceAfterFeeCharged ==
                    baseCharged + /* surge pricing additional fee */ 200);

            {
                LedgerTxn ltx(app->getLedgerTxnRoot());
                REQUIRE(tx->apply(*app, ltx, txm));
                tx->processPostApply(*app, ltx, txm);
                ltx.commit();
                auto changesAfter = txm.getChangesAfter();
                REQUIRE(changesAfter.size() == 2);
                REQUIRE(changesAfter[1].updated().data.account().balance -
                            changesAfter[0].state().data.account().balance ==
                        1180);
            }
            // The account should receive a refund for unspent refundable fee.
            REQUIRE(root.getBalance() - balanceAfterFeeCharged == 1180);
        }
        else
        {
            {
                LedgerTxn ltx(app->getLedgerTxnRoot());
                // Imitate surge pricing by charging at a higher rate than base
                // fee.
                tx->processFeeSeqNum(ltx, 300);
                ltx.commit();
            }
            int64_t balanceAfterFeeCharged = root.getBalance();

            {
                LedgerTxn ltx(app->getLedgerTxnRoot());
                // Unsuccessfully apply the tx and process refunds.
                REQUIRE(!tx->apply(*app, ltx, txm));
                tx->processPostApply(*app, ltx, txm);
                ltx.commit();
                if (refundableFee > 0)
                {
                    auto changesAfter = txm.getChangesAfter();
                    REQUIRE(changesAfter.size() == 2);
                    REQUIRE(
                        changesAfter[1].updated().data.account().balance -
                            changesAfter[0].state().data.account().balance ==
                        refundableFee);
                }
                // The account should receive a full refund for metadata
                // in case of tx failure.
                REQUIRE(root.getBalance() - balanceAfterFeeCharged ==
                        refundableFee);
            }
        }
        SCVal resultVal;
        if (tx->getResult().result.code() == txSUCCESS &&
            !tx->getResult().result.results().empty())
        {
            auto const& ores = tx->getResult().result.results().at(0);
            if (ores.tr().type() == INVOKE_HOST_FUNCTION &&
                ores.tr().invokeHostFunctionResult().code() ==
                    INVOKE_HOST_FUNCTION_SUCCESS)
            {
                resultVal = txm.getXDR().v3().sorobanMeta->returnValue;

                InvokeHostFunctionSuccessPreImage success2;
                success2.returnValue = resultVal;
                success2.events = txm.getXDR().v3().sorobanMeta->events;

                REQUIRE(ores.tr().invokeHostFunctionResult().success() ==
                        xdrSha256(success2));
            }
        }
        return resultVal;
    };

    auto scFunc = makeSymbol("add");
    auto sc7 = makeI32(7);
    auto sc16 = makeI32(16);
    SorobanResources resources;
    resources.footprint.readOnly = contractKeys;
    resources.instructions = 2'000'000;
    resources.readBytes = 2000;
    resources.writeBytes = 0;

    SECTION("correct invocation")
    {
        call(resources, 1200, contractID, scFunc, {sc7, sc16}, true);
        REQUIRE(app->getMetrics()
                    .NewTimer({"soroban", "host-fn-op", "exec"})
                    .count() != 0);
        REQUIRE(app->getMetrics()
                    .NewMeter({"soroban", "host-fn-op", "success"}, "call")
                    .count() != 0);
    }

    SECTION("incorrect invocation parameters")
    {
        SECTION("non-existent contract id")
        {
            SCAddress address(SC_ADDRESS_TYPE_CONTRACT);
            address.contractId()[0] = 1;
            call(resources, 1200, address, scFunc, {sc7, sc16}, false);
        }
        SECTION("account address")
        {
            SCAddress address(SC_ADDRESS_TYPE_ACCOUNT);
            address.accountId() = root.getPublicKey();
            call(resources, 1200, address, scFunc, {sc7, sc16}, false);
        }
        SECTION("too few parameters")
        {
            call(resources, 1200, contractID, scFunc, {sc7}, false);
        }
        SECTION("too many parameters")
        {
            // Too many parameters
            call(resources, 1200, contractID, scFunc, {sc7, sc16, makeI32(0)},
                 false);
        }
    }

    SECTION("insufficient instructions")
    {
        resources.instructions = 10000;
        call(resources, 1200, contractID, scFunc, {sc7, sc16}, false);
    }
    SECTION("insufficient read bytes")
    {
        resources.readBytes = 100;
        call(resources, 1200, contractID, scFunc, {sc7, sc16}, false);
    }
    SECTION("insufficient refundable fee")
    {
        call(resources, 0, contractID, scFunc, {sc7, sc16}, false);
    }
    SECTION("invalid footprint keys")
    {
        auto invalidFootprint = [&](xdr::xvector<stellar::LedgerKey>&
                                        footprint) {
            auto acc = root.create(
                "acc", app->getLedgerManager().getLastMinBalance(1));
            SECTION("valid")
            {
                // add a valid trustline to the footprint to make sure the
                // initial tx is valid.
                footprint.emplace_back(
                    trustlineKey(root.getPublicKey(), makeAsset(acc, "USD")));
                REQUIRE(isValid(resources, contractID, scFunc, {sc7, sc16}));
            }
            SECTION("native asset trustline")
            {
                footprint.emplace_back(
                    trustlineKey(root.getPublicKey(), makeNativeAsset()));
                REQUIRE(!isValid(resources, contractID, scFunc, {sc7, sc16}));
            }
            SECTION("issuer trustline")
            {
                footprint.emplace_back(
                    trustlineKey(root.getPublicKey(), makeAsset(root, "USD")));
                REQUIRE(!isValid(resources, contractID, scFunc, {sc7, sc16}));
            }
            auto invalidAssets = testutil::getInvalidAssets(root);
            for (size_t i = 0; i < invalidAssets.size(); ++i)
            {
                auto key = trustlineKey(acc.getPublicKey(), invalidAssets[i]);
                SECTION("invalid asset " + std::to_string(i))
                {
                    footprint.emplace_back(key);
                    REQUIRE(
                        !isValid(resources, contractID, scFunc, {sc7, sc16}));
                }
            }
            SECTION("offer")
            {
                footprint.emplace_back(offerKey(root, 1));
                REQUIRE(!isValid(resources, contractID, scFunc, {sc7, sc16}));
            }
            SECTION("data")
            {
                footprint.emplace_back(dataKey(root, "name"));
                REQUIRE(!isValid(resources, contractID, scFunc, {sc7, sc16}));
            }
            SECTION("claimable balance")
            {
                footprint.emplace_back(
                    claimableBalanceKey(ClaimableBalanceID{}));
                REQUIRE(!isValid(resources, contractID, scFunc, {sc7, sc16}));
            }
            SECTION("liquidity pool")
            {
                footprint.emplace_back(liquidityPoolKey(PoolID{}));
                REQUIRE(!isValid(resources, contractID, scFunc, {sc7, sc16}));
            }
            SECTION("config setting")
            {
                footprint.emplace_back(configSettingKey(ConfigSettingID{}));
                REQUIRE(!isValid(resources, contractID, scFunc, {sc7, sc16}));
            }
        };

        SECTION("readOnly")
        {
            invalidFootprint(resources.footprint.readOnly);
        }
        SECTION("readWrite")
        {
            invalidFootprint(resources.footprint.readWrite);
        }
    }
}

TEST_CASE("contract storage", "[tx][soroban]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    overrideSorobanNetworkConfigForTest(*app);
    auto root = TestAccount::createRoot(*app);
    auto const contractDataWasm = rust_bridge::get_test_wasm_contract_data();
    auto contractKeys = deployContractWithSourceAccount(*app, contractDataWasm);
    auto const& contractID = contractKeys[0].contractData().contract;
    LedgerTxn ltxCfg(app->getLedgerTxnRoot());
    SorobanNetworkConfig const& sorobanConfig =
        app->getLedgerManager().getSorobanNetworkConfig(ltxCfg);
    auto const& stateExpirationSettings =
        sorobanConfig.stateExpirationSettings();
    ltxCfg.commit();
    auto checkContractData = [&](SCVal const& key, ContractDataDurability type,
                                 SCVal const* val) {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        auto ltxe = loadContractData(ltx, contractID, key, type);
        if (val)
        {
            REQUIRE(ltxe);

            auto const& body = ltxe.current().data.contractData().body;
            REQUIRE(body.bodyType() == DATA_ENTRY);
            REQUIRE(body.data().val == *val);
        }
        else
        {
            REQUIRE(!ltxe);
        }
    };

    auto createTx = [&](xdr::xvector<LedgerKey> const& readOnly,
                        xdr::xvector<LedgerKey> const& readWrite,
                        uint32_t writeBytes, SCAddress const& contractAddress,
                        SCSymbol const& functionName,
                        std::vector<SCVal> const& args)
        -> std::tuple<TransactionFrameBasePtr, std::shared_ptr<LedgerTxn>,
                      std::shared_ptr<TransactionMetaFrame>> {
        Operation op;
        op.body.type(INVOKE_HOST_FUNCTION);
        auto& ihf = op.body.invokeHostFunctionOp().hostFunction;
        ihf.type(HOST_FUNCTION_TYPE_INVOKE_CONTRACT);
        ihf.invokeContract().contractAddress = contractAddress;
        ihf.invokeContract().functionName = functionName;
        ihf.invokeContract().args.assign(args.begin(), args.end());
        SorobanResources resources;
        resources.footprint.readOnly = readOnly;
        resources.footprint.readWrite = readWrite;
        resources.instructions = 4'000'000;
        resources.readBytes = 5000;
        resources.writeBytes = writeBytes;

        auto tx = sorobanTransactionFrameFromOps(
            app->getNetworkID(), root, {op}, {}, resources, 200'000, 40'000);
        auto ltx = std::make_shared<LedgerTxn>(app->getLedgerTxnRoot());
        auto txm = std::make_shared<TransactionMetaFrame>(
            ltx->loadHeader().current().ledgerVersion);
        REQUIRE(tx->checkValid(*app, *ltx, 0, 0, 0));
        return {tx, ltx, txm};
    };

    auto putWithFootprint = [&](std::string const& key, uint64_t val,
                                xdr::xvector<LedgerKey> const& readOnly,
                                xdr::xvector<LedgerKey> const& readWrite,
                                uint32_t writeBytes, bool expectSuccess,
                                ContractDataDurability type) {
        auto keySymbol = makeSymbolSCVal(key);
        auto valU64 = makeU64(val);

        std::string funcStr;
        switch (type)
        {
        case ContractDataDurability::TEMPORARY:
            funcStr = "put_temporary";
            break;
        case ContractDataDurability::PERSISTENT:
            funcStr = "put_persistent";
            break;
        }

        auto [tx, ltx, txm] =
            createTx(readOnly, readWrite, writeBytes, contractID,
                     makeSymbol(funcStr), {keySymbol, valU64});

        if (expectSuccess)
        {
            REQUIRE(tx->apply(*app, *ltx, *txm));
            ltx->commit();
            checkContractData(keySymbol, type, &valU64);
        }
        else
        {
            REQUIRE(!tx->apply(*app, *ltx, *txm));
            ltx->commit();
        }
    };

    auto put = [&](std::string const& key, uint64_t val,
                   ContractDataDurability type) {
        putWithFootprint(key, val, contractKeys,
                         {contractDataKey(contractID, makeSymbolSCVal(key),
                                          type, DATA_ENTRY)},
                         1000, true, type);
    };

    auto hasWithFootprint = [&](std::string const& key,
                                xdr::xvector<LedgerKey> const& readOnly,
                                uint32_t writeBytes, bool expectSuccess,
                                ContractDataDurability type) -> bool {
        auto keySymbol = makeSymbolSCVal(key);

        std::string funcStr;
        switch (type)
        {
        case ContractDataDurability::TEMPORARY:
            funcStr = "has_temporary";
            break;
        case ContractDataDurability::PERSISTENT:
            funcStr = "has_persistent";
            break;
        }

        auto [tx, ltx, txm] = createTx(readOnly, {}, writeBytes, contractID,
                                       makeSymbol(funcStr), {keySymbol});

        if (expectSuccess)
        {
            REQUIRE(tx->apply(*app, *ltx, *txm));
            ltx->commit();
            return txm->getXDR().v3().sorobanMeta->returnValue.b();
        }
        else
        {
            REQUIRE(!tx->apply(*app, *ltx, *txm));
            ltx->commit();
        }
        return false;
    };

    auto has = [&](std::string const& key,
                   ContractDataDurability type) -> bool {
        auto readOnly = contractKeys;
        readOnly.emplace_back(contractDataKey(contractID, makeSymbolSCVal(key),
                                              type, DATA_ENTRY));
        return hasWithFootprint(key, readOnly, 0, true, type);
    };

    auto getWithFootprint =
        [&](std::string const& key, xdr::xvector<LedgerKey> const& readOnly,
            xdr::xvector<LedgerKey> const& readWrite, uint32_t writeBytes,
            bool expectSuccess, ContractDataDurability type) {
            auto keySymbol = makeSymbolSCVal(key);

            std::string funcStr;
            switch (type)
            {
            case ContractDataDurability::TEMPORARY:
                funcStr = "get_temporary";
                break;
            case ContractDataDurability::PERSISTENT:
                funcStr = "get_persistent";
                break;
            }

            auto [tx, ltx, txm] =
                createTx(readOnly, readWrite, writeBytes, contractID,
                         makeSymbol(funcStr), {keySymbol});

            if (expectSuccess)
            {
                REQUIRE(tx->apply(*app, *ltx, *txm));
                ltx->commit();
            }
            else
            {
                REQUIRE(!tx->apply(*app, *ltx, *txm));
                ltx->commit();
            }
        };

    auto get = [&](std::string const& key, ContractDataDurability type,
                   bool expectSuccess) {
        getWithFootprint(key, contractKeys,
                         {contractDataKey(contractID, makeSymbolSCVal(key),
                                          type, DATA_ENTRY)},
                         1000, expectSuccess, type);
    };

    auto bumpWithFootprint = [&](std::string const& key, uint32_t bumpAmount,
                                 xdr::xvector<LedgerKey> const& readOnly,
                                 xdr::xvector<LedgerKey> const& readWrite,
                                 bool expectSuccess,
                                 ContractDataDurability type) {
        auto keySymbol = makeSymbolSCVal(key);
        auto bumpAmountU32 = makeU32(bumpAmount);

        std::string funcStr;
        switch (type)
        {
        case ContractDataDurability::TEMPORARY:
            funcStr = "bump_temporary";
            break;
        case ContractDataDurability::PERSISTENT:
            funcStr = "bump_persistent";
            break;
        }

        // TODO: Better bytes to write value
        auto [tx, ltx, txm] =
            createTx(readOnly, readWrite, 1000, contractID, makeSymbol(funcStr),
                     {keySymbol, bumpAmountU32});

        if (expectSuccess)
        {
            REQUIRE(tx->apply(*app, *ltx, *txm));
        }
        else
        {
            REQUIRE(!tx->apply(*app, *ltx, *txm));
        }

        ltx->commit();
    };

    auto bump = [&](std::string const& key, ContractDataDurability type,
                    uint32_t bumpAmount) {
        bumpWithFootprint(key, bumpAmount, contractKeys,
                          {contractDataKey(contractID, makeSymbolSCVal(key),
                                           type, DATA_ENTRY)},
                          true, type);
    };

    auto runExpirationOp = [&](TestAccount& root, TransactionFrameBasePtr tx,
                               int64_t refundableFee,
                               int64_t expectedRefundableFeeCharged) {
        int64_t initBalance = root.getBalance();
        // Apply the transaction and process the refund.
        {
            LedgerTxn ltx(app->getLedgerTxnRoot());
            REQUIRE(tx->checkValid(*app, ltx, 0, 0, 0));
        }
        // Initially we store in result the charge for resources plus
        // minimum inclusion  fee bid (currently equivalent to the network
        // `baseFee` of 100).
        int64_t baseCharged = (tx->getFullFee() - tx->getInclusionFee()) + 100;
        REQUIRE(tx->getResult().feeCharged == baseCharged);
        // Charge the fee.
        {
            LedgerTxn ltx(app->getLedgerTxnRoot());
            // Imitate surge pricing by charging at a higher rate than base
            // fee.
            tx->processFeeSeqNum(ltx, 300);
            ltx.commit();
        }
        // The resource and the base fee are charged, with additional
        // surge pricing fee.
        int64_t balanceAfterFeeCharged = root.getBalance();
        REQUIRE(initBalance - balanceAfterFeeCharged ==
                baseCharged + /* surge pricing additional fee */ 200);
        {
            LedgerTxn ltx(app->getLedgerTxnRoot());
            TransactionMetaFrame txm(ltx.loadHeader().current().ledgerVersion);
            REQUIRE(tx->apply(*app, ltx, txm));
            tx->processPostApply(*app, ltx, txm);
            ltx.commit();

            auto changesAfter = txm.getChangesAfter();
            REQUIRE(changesAfter.size() == 2);
            REQUIRE(changesAfter[1].updated().data.account().balance -
                        changesAfter[0].state().data.account().balance ==
                    refundableFee - expectedRefundableFeeCharged);
        }

        // The account should receive a refund for unspent refundable fee.
        REQUIRE(root.getBalance() - balanceAfterFeeCharged ==
                refundableFee - expectedRefundableFeeCharged);
    };

    auto bumpOp = [&](uint32_t bumpAmount,
                      xdr::xvector<LedgerKey> const& readOnly,
                      int64_t expectedRefundableFeeCharged) {
        Operation bumpOp;
        bumpOp.body.type(BUMP_FOOTPRINT_EXPIRATION);
        bumpOp.body.bumpFootprintExpirationOp().ledgersToExpire = bumpAmount;

        SorobanResources bumpResources;
        bumpResources.footprint.readOnly = readOnly;
        bumpResources.instructions = 0;
        bumpResources.readBytes = 5000;
        bumpResources.writeBytes = 0;

        auto tx =
            sorobanTransactionFrameFromOps(app->getNetworkID(), root, {bumpOp},
                                           {}, bumpResources, 100'000, 1200);

        runExpirationOp(root, tx, 1200, expectedRefundableFeeCharged);
    };

    auto restoreOp = [&](xdr::xvector<LedgerKey> const& readWrite,
                         int64_t expectedRefundableFeeCharged) {
        Operation restoreOp;
        restoreOp.body.type(RESTORE_FOOTPRINT);

        SorobanResources bumpResources;
        bumpResources.footprint.readWrite = readWrite;
        bumpResources.instructions = 0;
        bumpResources.readBytes = 5000;
        bumpResources.writeBytes = 5000;

        // submit operation
        auto tx = sorobanTransactionFrameFromOps(app->getNetworkID(), root,
                                                 {restoreOp}, {}, bumpResources,
                                                 100'000, 1'200);
        runExpirationOp(root, tx, 1200, expectedRefundableFeeCharged);
    };

    auto delWithFootprint = [&](std::string const& key,
                                xdr::xvector<LedgerKey> const& readOnly,
                                xdr::xvector<LedgerKey> const& readWrite,
                                bool expectSuccess,
                                ContractDataDurability type) {
        auto keySymbol = makeSymbolSCVal(key);

        std::string funcStr;
        switch (type)
        {
        case ContractDataDurability::TEMPORARY:
            funcStr = "del_temporary";
            break;
        case ContractDataDurability::PERSISTENT:
            funcStr = "del_persistent";
            break;
        }

        // TODO: Better bytes to write value
        auto [tx, ltx, txm] = createTx(readOnly, readWrite, 1000, contractID,
                                       makeSymbol(funcStr), {keySymbol});

        if (expectSuccess)
        {
            REQUIRE(tx->apply(*app, *ltx, *txm));
            ltx->commit();
            checkContractData(keySymbol, type, nullptr);
        }
        else
        {
            REQUIRE(!tx->apply(*app, *ltx, *txm));
            ltx->commit();
        }
    };

    auto del = [&](std::string const& key, ContractDataDurability type) {
        delWithFootprint(key, contractKeys,
                         {contractDataKey(contractID, makeSymbolSCVal(key),
                                          type, DATA_ENTRY)},
                         true, type);
    };

    auto checkContractDataExpirationLedger =
        [&](std::string const& key, ContractDataDurability type,
            uint32_t expectedExpirationLedger, uint32_t flags = 0) {
            auto le = loadStorageEntry(*app, contractID, key, type);
            REQUIRE(le.data.contractData().body.data().flags == flags);
            REQUIRE(getExpirationLedger(le) == expectedExpirationLedger);
        };

    auto checkContractDataExpirationState =
        [&](std::string const& key, ContractDataDurability type,
            uint32_t ledgerSeq, bool expectedIsLive) {
            auto le = loadStorageEntry(*app, contractID, key, type);
            REQUIRE(isLive(le, ledgerSeq) == expectedIsLive);

            // Make sure entry is accessible/inaccessible
            get(key, type, expectedIsLive);
        };

    auto checkKeyExpirationLedger = [&](LedgerKey const& key,
                                        uint32_t ledgerSeq,
                                        uint32_t expectedExpirationLedger) {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        auto ltxe = ltx.load(key);
        REQUIRE(ltxe);
        REQUIRE(expectedExpirationLedger ==
                getExpirationLedger(ltxe.current()));
    };

    auto ledgerSeq = getLedgerSeq(*app);

    // Autobump is disabled by default, enable it for some tests
    auto enableAutobump = [&]() {
        uint32_t autobumpAmount = 10;
        modifySorobanNetworkConfig(
            *app, [autobumpAmount](SorobanNetworkConfig& cfg) {
                cfg.mStateExpirationSettings.autoBumpLedgers = autobumpAmount;
            });
        return autobumpAmount;
    };

    SECTION("default limits")
    {
        put("key1", 0, ContractDataDurability::PERSISTENT);
        put("key2", 21, ContractDataDurability::PERSISTENT);

        // Failure: insufficient write bytes
        putWithFootprint(
            "key2", 88888, contractKeys,
            {contractDataKey(contractID, makeSymbolSCVal("key2"),
                             ContractDataDurability::PERSISTENT, DATA_ENTRY)},
            1, false, ContractDataDurability::PERSISTENT);

        put("key1", 9, ContractDataDurability::PERSISTENT);
        put("key2", UINT64_MAX, ContractDataDurability::PERSISTENT);

        del("key1", ContractDataDurability::PERSISTENT);
        del("key2", ContractDataDurability::PERSISTENT);
    }

    SECTION("failure: entry exceeds max size")
    {
        modifySorobanNetworkConfig(*app, [](SorobanNetworkConfig& cfg) {
            cfg.mMaxContractDataEntrySizeBytes = 1;
        });
        // this fails due to the contract code itself exceeding the entry limit
        putWithFootprint(
            "key2", 2, contractKeys,
            {contractDataKey(contractID, makeSymbolSCVal("key2"),
                             ContractDataDurability::PERSISTENT, DATA_ENTRY)},
            1000, false, ContractDataDurability::PERSISTENT);
    }

    SECTION("Same ScVal key, different types")
    {
        // Check that each type is in their own keyspace
        uint64_t uniqueVal = 0;
        uint64_t temporaryVal = 2;

        put("key", uniqueVal, ContractDataDurability::PERSISTENT);
        put("key", temporaryVal, TEMPORARY);

        auto uniqueScVal = makeU64(uniqueVal);
        auto temporaryScVal = makeU64(temporaryVal);
        auto keySymbol = makeSymbolSCVal("key");

        checkContractData(keySymbol, ContractDataDurability::PERSISTENT,
                          &uniqueScVal);
        checkContractData(keySymbol, TEMPORARY, &temporaryScVal);

        put("key2", 3, ContractDataDurability::PERSISTENT);
        auto key2Symbol = makeSymbolSCVal("key2");
        auto uniqueScVal2 = makeU64(3);

        checkContractData(key2Symbol, ContractDataDurability::PERSISTENT,
                          &uniqueScVal2);
        checkContractData(key2Symbol, TEMPORARY, nullptr);
    }

    SECTION("contract instance and wasm expiration")
    {
        uint32_t originalExpectedExpiration =
            stateExpirationSettings.minPersistentEntryExpiration + ledgerSeq -
            1;

        for (uint32_t i = app->getLedgerManager().getLastClosedLedgerNum();
             i <= originalExpectedExpiration + 1; ++i)
        {
            closeLedgerOn(*app, i, 2, 1, 2016);
        }

        // Contract instance and code are expired, an TX should fail
        putWithFootprint("temp", 0, contractKeys,
                         {contractDataKey(contractID, makeSymbolSCVal("temp"),
                                          TEMPORARY, DATA_ENTRY)},
                         1000, /*expectSuccess*/ false,
                         ContractDataDurability::TEMPORARY);

        ledgerSeq = getLedgerSeq(*app);
        auto newExpectedExpiration =
            stateExpirationSettings.minPersistentEntryExpiration + ledgerSeq -
            1;

        SECTION("restore contract instance and wasm")
        {
            // Restore Instance and WASM
            restoreOp(contractKeys, 54);

            // Instance should now be useable
            putWithFootprint(
                "temp", 0, contractKeys,
                {contractDataKey(contractID, makeSymbolSCVal("temp"), TEMPORARY,
                                 DATA_ENTRY)},
                1000, /*expectSuccess*/ true,
                ContractDataDurability::TEMPORARY);

            checkKeyExpirationLedger(contractKeys[0], ledgerSeq,
                                     newExpectedExpiration);
            checkKeyExpirationLedger(contractKeys[1], ledgerSeq,
                                     newExpectedExpiration);
        }

        SECTION("restore contract instance, not wasm")
        {
            // Only restore contract instance
            restoreOp({contractKeys[0]}, 3);

            // invocation should fail
            putWithFootprint(
                "temp", 0, contractKeys,
                {contractDataKey(contractID, makeSymbolSCVal("temp"), TEMPORARY,
                                 DATA_ENTRY)},
                1000, /*expectSuccess*/ false,
                ContractDataDurability::TEMPORARY);

            checkKeyExpirationLedger(contractKeys[0], ledgerSeq,
                                     newExpectedExpiration);
            checkKeyExpirationLedger(contractKeys[1], ledgerSeq,
                                     originalExpectedExpiration);
        }

        SECTION("restore contract wasm, not instance")
        {
            // Only restore WASM
            restoreOp({contractKeys[1]}, 51);

            // invocation should fail
            putWithFootprint(
                "temp", 0, contractKeys,
                {contractDataKey(contractID, makeSymbolSCVal("temp"), TEMPORARY,
                                 DATA_ENTRY)},
                1000, /*expectSuccess*/ false,
                ContractDataDurability::TEMPORARY);

            checkKeyExpirationLedger(contractKeys[0], ledgerSeq,
                                     originalExpectedExpiration);
            checkKeyExpirationLedger(contractKeys[1], ledgerSeq,
                                     newExpectedExpiration);
        }

        SECTION("lifetime extensions")
        {
            // Restore Instance and WASM
            restoreOp(contractKeys, 54);

            auto instanceBumpAmount = 10'000;
            auto wasmBumpAmount = 15'000;

            // bump instance
            bumpOp(instanceBumpAmount, {contractKeys[0]}, 4);

            // bump WASM
            bumpOp(wasmBumpAmount, {contractKeys[1]}, 135);

            checkKeyExpirationLedger(contractKeys[0], ledgerSeq,
                                     ledgerSeq + instanceBumpAmount);
            checkKeyExpirationLedger(contractKeys[1], ledgerSeq,
                                     ledgerSeq + wasmBumpAmount);
        }
    }

    SECTION("contract storage expiration")
    {
        put("unique", 0, ContractDataDurability::PERSISTENT);
        put("temp", 0, TEMPORARY);

        auto expectedTempExpiration =
            stateExpirationSettings.minTempEntryExpiration + ledgerSeq - 1;
        auto expectedPersistentExpiration =
            stateExpirationSettings.minPersistentEntryExpiration + ledgerSeq -
            1;

        // Check for expected minimum lifetime values
        checkContractDataExpirationLedger("unique",
                                          ContractDataDurability::PERSISTENT,
                                          expectedPersistentExpiration);
        checkContractDataExpirationLedger("temp", TEMPORARY,
                                          expectedTempExpiration);

        // Close ledgers until temp entry expires
        uint32 currLedger = app->getLedgerManager().getLastClosedLedgerNum();
        for (; currLedger <= expectedTempExpiration + 1; ++currLedger)
        {
            closeLedgerOn(*app, currLedger, 2, 1, 2016);
        }
        REQUIRE(app->getLedgerManager().getLastClosedLedgerNum() ==
                expectedTempExpiration + 1);

        // Check that temp entry has expired
        checkContractDataExpirationState(
            "temp", TEMPORARY, app->getLedgerManager().getLastClosedLedgerNum(),
            false);

        checkContractDataExpirationState(
            "unique", ContractDataDurability::PERSISTENT,
            app->getLedgerManager().getLastClosedLedgerNum(), true);

        // Check that we can recreate an expired TEMPORARY entry
        putWithFootprint("temp", 0, contractKeys,
                         {contractDataKey(contractID, makeSymbolSCVal("temp"),
                                          TEMPORARY, DATA_ENTRY)},
                         1000, /*expectSuccess*/ true,
                         ContractDataDurability::TEMPORARY);

        // Recreated entry should be live
        ledgerSeq = getLedgerSeq(*app);
        checkContractDataExpirationState(
            "temp", TEMPORARY, app->getLedgerManager().getLastClosedLedgerNum(),
            true);
        checkContractDataExpirationLedger(
            "temp", TEMPORARY,
            stateExpirationSettings.minTempEntryExpiration + ledgerSeq - 1);

        // Close ledgers until PERSISTENT entry expires
        for (; currLedger <= expectedPersistentExpiration + 1; ++currLedger)
        {
            closeLedgerOn(*app, currLedger, 2, 1, 2016);
        }

        REQUIRE(app->getLedgerManager().getLastClosedLedgerNum() ==
                expectedPersistentExpiration + 1);
        checkContractDataExpirationState(
            "unique", ContractDataDurability::PERSISTENT,
            app->getLedgerManager().getLastClosedLedgerNum(), false);

        // Check that we can't recreate expired PERSISTENT
        putWithFootprint(
            "unique", 0, contractKeys,
            {contractDataKey(contractID, makeSymbolSCVal("unique"),
                             ContractDataDurability::PERSISTENT, DATA_ENTRY)},
            1000, /*expectSuccess*/ false, ContractDataDurability::PERSISTENT);
    }

    SECTION("autobump")
    {
        auto autobump = enableAutobump();

        put("rw", 0, ContractDataDurability::PERSISTENT);
        put("ro", 0, ContractDataDurability::PERSISTENT);

        auto readOnlySet = contractKeys;
        readOnlySet.emplace_back(
            contractDataKey(contractID, makeSymbolSCVal("ro"),
                            ContractDataDurability::PERSISTENT, DATA_ENTRY));

        auto readWriteSet = {contractDataKey(contractID, makeSymbolSCVal("rw"),
                                             ContractDataDurability::PERSISTENT,
                                             DATA_ENTRY)};

        // Invoke contract with all keys in footprint
        putWithFootprint("rw", 1, readOnlySet, readWriteSet, 1000, true,
                         ContractDataDurability::PERSISTENT);

        auto expectedInitialExpiration =
            stateExpirationSettings.minPersistentEntryExpiration + ledgerSeq -
            1;

        checkContractDataExpirationLedger("rw",
                                          ContractDataDurability::PERSISTENT,
                                          expectedInitialExpiration + autobump);
        checkContractDataExpirationLedger("ro",
                                          ContractDataDurability::PERSISTENT,
                                          expectedInitialExpiration + autobump);

        // Contract instance and Wasm should have minimum life and 3 invocations
        // worth of autobumps
        for (auto const& key : contractKeys)
        {
            checkKeyExpirationLedger(
                key, ledgerSeq, expectedInitialExpiration + (autobump * 3));
        }
    }

    SECTION("manual bump")
    {
        // Large bump, followed by smaller bump
        put("key", 0, ContractDataDurability::PERSISTENT);
        bump("key", ContractDataDurability::PERSISTENT, 10'000);
        checkContractDataExpirationLedger(
            "key", ContractDataDurability::PERSISTENT, ledgerSeq + 10'000);

        // Expiration already above 5'000, should be a no-op
        bump("key", ContractDataDurability::PERSISTENT, 5'000);
        checkContractDataExpirationLedger(
            "key", ContractDataDurability::PERSISTENT, ledgerSeq + 10'000);

        // Small bump followed by larger bump
        put("key2", 0, ContractDataDurability::PERSISTENT);
        bump("key2", ContractDataDurability::PERSISTENT, 5'000);
        checkContractDataExpirationLedger(
            "key2", ContractDataDurability::PERSISTENT, ledgerSeq + 5'000);

        put("key3", 0, ContractDataDurability::PERSISTENT);
        bump("key3", ContractDataDurability::PERSISTENT, 50'000);
        checkContractDataExpirationLedger(
            "key3", ContractDataDurability::PERSISTENT, ledgerSeq + 50'000);

        // Bump multiple keys to live 10100 ledger from now
        bumpOp(
            10100,
            {contractDataKey(contractID, makeSymbolSCVal("key"),
                             ContractDataDurability::PERSISTENT, DATA_ENTRY),
             contractDataKey(contractID, makeSymbolSCVal("key2"),
                             ContractDataDurability::PERSISTENT, DATA_ENTRY),
             contractDataKey(contractID, makeSymbolSCVal("key3"),
                             ContractDataDurability::PERSISTENT, DATA_ENTRY)},
            4);

        checkContractDataExpirationLedger(
            "key", ContractDataDurability::PERSISTENT, ledgerSeq + 10'100);
        checkContractDataExpirationLedger(
            "key2", ContractDataDurability::PERSISTENT, ledgerSeq + 10'100);

        // No change for key3 since expiration is already past 10100 ledgers
        // from now
        checkContractDataExpirationLedger(
            "key3", ContractDataDurability::PERSISTENT, ledgerSeq + 50'000);
    }

    SECTION("restore expired entry")
    {
        uint32_t initExpirationLedger =
            stateExpirationSettings.minPersistentEntryExpiration + ledgerSeq -
            1;

        // Bump instance and WASM so that they don't expire during the test
        bumpOp(10'000, contractKeys, 77);

        put("key", 0, ContractDataDurability::PERSISTENT);
        checkContractDataExpirationLedger(
            "key", ContractDataDurability::PERSISTENT, initExpirationLedger);

        // Crank until entry expires
        for (uint32 i = app->getLedgerManager().getLastClosedLedgerNum();
             i <= initExpirationLedger + 1; ++i)
        {
            closeLedgerOn(*app, i, 2, 1, 2016);
        }

        REQUIRE(app->getLedgerManager().getLastClosedLedgerNum() ==
                initExpirationLedger + 1);

        checkContractDataExpirationState(
            "key", ContractDataDurability::PERSISTENT,
            app->getLedgerManager().getLastClosedLedgerNum(), false);

        auto lk =
            contractDataKey(contractID, makeSymbolSCVal("key"),
                            ContractDataDurability::PERSISTENT, DATA_ENTRY);
        auto roKeys = contractKeys;
        roKeys.emplace_back(lk);

        // Trying to read this expired entry should fail.
        getWithFootprint("key", roKeys, {}, 1000, /*expectSuccess*/ false,
                         ContractDataDurability::PERSISTENT);

        // Recreation of persistent entries should fail.
        putWithFootprint("key", 0, contractKeys, {lk}, 1000,
                         /*expectSuccess*/ false,
                         ContractDataDurability::PERSISTENT);

        // Bump operation should skip expired entries
        bumpOp(1'000, {lk}, 0);

        // Make sure expirationLedger is unchanged by bumpOp
        checkContractDataExpirationLedger(
            "key", ContractDataDurability::PERSISTENT, initExpirationLedger);

        // Restore the entry
        restoreOp({lk}, 3);

        ledgerSeq = getLedgerSeq(*app);
        checkContractDataExpirationState(
            "key", ContractDataDurability::PERSISTENT, ledgerSeq, true);

        // Entry is considered to be restored on lclNum (as we didn't close an
        // additional ledger).
        checkContractDataExpirationLedger(
            "key", ContractDataDurability::PERSISTENT,
            ledgerSeq + stateExpirationSettings.minPersistentEntryExpiration -
                1);

        // Write to entry to check that is is live
        put("key", 1, ContractDataDurability::PERSISTENT);
    }

    SECTION("re-create expired temporary entry")
    {
        auto minBump = InitialSorobanNetworkConfig::MINIMUM_TEMP_ENTRY_LIFETIME;
        put("key", 0, ContractDataDurability::TEMPORARY);
        REQUIRE(has("key", ContractDataDurability::TEMPORARY));

        uint32_t initExpirationLedger = ledgerSeq + minBump - 1;
        checkContractDataExpirationLedger(
            "key", ContractDataDurability::TEMPORARY, initExpirationLedger);

        for (size_t i = app->getLedgerManager().getLastClosedLedgerNum();
             i <= initExpirationLedger + 1; ++i)
        {
            closeLedgerOn(*app, i, 2, 1, 2016);
        }
        REQUIRE(app->getLedgerManager().getLastClosedLedgerNum() ==
                initExpirationLedger + 1);
        checkContractDataExpirationState(
            "key", ContractDataDurability::TEMPORARY,
            app->getLedgerManager().getLastClosedLedgerNum(), false);

        // Entry has expired
        REQUIRE(!has("key", ContractDataDurability::TEMPORARY));

        // We can recreate an expired temp entry
        put("key", 0, ContractDataDurability::TEMPORARY);
        REQUIRE(has("key", ContractDataDurability::TEMPORARY));

        uint32_t newExpirationLedger =
            app->getLedgerManager().getLastClosedLedgerNum() + minBump - 1;
        checkContractDataExpirationLedger(
            "key", ContractDataDurability::TEMPORARY, newExpirationLedger);
    }

    SECTION("max expiration")
    {
        auto autobump = enableAutobump();
        REQUIRE(autobump > 1);

        // Check that attempting to bump past max ledger results in error
        put("key", 0, ContractDataDurability::PERSISTENT);
        bumpWithFootprint(
            "key", UINT32_MAX, contractKeys,
            {contractDataKey(contractID, makeSymbolSCVal("key"),
                             ContractDataDurability::PERSISTENT, DATA_ENTRY)},
            false, ContractDataDurability::PERSISTENT);

        // Now bump to max
        bump("key", ContractDataDurability::PERSISTENT,
             stateExpirationSettings.maxEntryExpiration - 1);

        auto maxExpiration =
            ledgerSeq + stateExpirationSettings.maxEntryExpiration - 1;
        checkContractDataExpirationLedger(
            "key", ContractDataDurability::PERSISTENT, maxExpiration);

        // Manual bump to almost max, then autobump to check that autobump
        // doesn't go over max
        put("key2", 0, ContractDataDurability::PERSISTENT);
        bump("key2", ContractDataDurability::PERSISTENT,
             stateExpirationSettings.maxEntryExpiration - 2);
        checkContractDataExpirationLedger(
            "key2", ContractDataDurability::PERSISTENT, maxExpiration - 1);

        // Autobump should only add a single ledger to bring expiration to max
        put("key2", 1, ContractDataDurability::PERSISTENT);
        checkContractDataExpirationLedger(
            "key2", ContractDataDurability::PERSISTENT, maxExpiration);
    }
    SECTION("footprint tests")
    {
        put("key1", 0, ContractDataDurability::PERSISTENT);
        put("key2", 21, ContractDataDurability::PERSISTENT);
        SECTION("unused readWrite key")
        {
            auto acc = root.create(
                "acc", app->getLedgerManager().getLastMinBalance(1));

            REQUIRE(doesAccountExist(*app, acc));

            putWithFootprint(
                "key1", 0, contractKeys,
                {contractDataKey(contractID, makeSymbolSCVal("key1"),
                                 ContractDataDurability::PERSISTENT,
                                 DATA_ENTRY),
                 accountKey(acc)},
                1000, true, ContractDataDurability::PERSISTENT);
            // make sure account still exists and hasn't change
            REQUIRE(doesAccountExist(*app, acc));
        }
        SECTION("incorrect footprint")
        {
            // Failure: contract data isn't in footprint
            putWithFootprint("key1", 88, contractKeys, {}, 1000, false,
                             ContractDataDurability::PERSISTENT);
            delWithFootprint("key1", contractKeys, {}, false,
                             ContractDataDurability::PERSISTENT);

            // Failure: contract data is read only
            auto readOnlyFootprint = contractKeys;
            readOnlyFootprint.push_back(contractDataKey(
                contractID, makeSymbolSCVal("key2"),
                ContractDataDurability::PERSISTENT, DATA_ENTRY));
            putWithFootprint("key2", 888888, readOnlyFootprint, {}, 1000, false,
                             ContractDataDurability::PERSISTENT);
            delWithFootprint("key2", readOnlyFootprint, {}, false,
                             ContractDataDurability::PERSISTENT);
        }
    }
}

TEST_CASE("temp entry eviction", "[tx][soroban]")
{
    VirtualClock clock;

    Config cfg = getTestConfig();
    TmpDirManager tdm(std::string("soroban-storage-meta-") +
                      binToHex(randomBytes(8)));
    TmpDir td = tdm.tmpDir("soroban-meta-ok");
    std::string metaPath = td.getName() + "/stream.xdr";

    cfg.METADATA_OUTPUT_STREAM = metaPath;

    auto app = createTestApplication(clock, cfg);
    auto root = TestAccount::createRoot(*app);
    auto const contractDataWasm = rust_bridge::get_test_wasm_contract_data();
    auto contractKeys = deployContractWithSourceAccount(*app, contractDataWasm);
    auto const& contractID = contractKeys[0].contractData().contract;

    // TODO:de-duplicate code?
    auto createTx =
        [&](xdr::xvector<LedgerKey> const& readOnly,
            xdr::xvector<LedgerKey> const& readWrite, uint32_t writeBytes,
            SCAddress const& contractAddress, SCSymbol const& functionName,
            std::vector<SCVal> const& args) -> TransactionFrameBasePtr {
        Operation op;
        op.body.type(INVOKE_HOST_FUNCTION);
        auto& ihf = op.body.invokeHostFunctionOp().hostFunction;
        ihf.type(HOST_FUNCTION_TYPE_INVOKE_CONTRACT);
        ihf.invokeContract().contractAddress = contractAddress;
        ihf.invokeContract().functionName = functionName;
        ihf.invokeContract().args.assign(args.begin(), args.end());
        SorobanResources resources;
        resources.footprint.readOnly = readOnly;
        resources.footprint.readWrite = readWrite;
        resources.instructions = 4'000'000;
        resources.readBytes = 5000;
        resources.writeBytes = writeBytes;
        resources.contractEventsSizeBytes = 0;

        auto tx = sorobanTransactionFrameFromOps(
            app->getNetworkID(), root, {op}, {}, resources, 200'000, 40'000);
        return tx;
    };

    auto putWithFootprint = [&](std::string const& key, uint64_t val,
                                xdr::xvector<LedgerKey> const& readOnly,
                                xdr::xvector<LedgerKey> const& readWrite,
                                uint32_t writeBytes, bool expectSuccess,
                                ContractDataDurability type) {
        auto keySymbol = makeSymbolSCVal(key);
        auto valU64 = makeU64(val);

        std::string funcStr;
        switch (type)
        {
        case ContractDataDurability::TEMPORARY:
            funcStr = "put_temporary";
            break;
        case ContractDataDurability::PERSISTENT:
            funcStr = "put_persistent";
            break;
        }

        auto tx = createTx(readOnly, readWrite, writeBytes, contractID,
                           makeSymbol(funcStr), {keySymbol, valU64});

        closeLedger(*app, {tx});
    };

    auto checkContractDataExpirationState =
        [&](std::string const& key, ContractDataDurability type,
            uint32_t ledgerSeq, bool expectedIsLive) {
            auto le = loadStorageEntry(*app, contractID, key, type);
            REQUIRE(isLive(le, ledgerSeq) == expectedIsLive);
        };

    auto lk = contractDataKey(contractID, makeSymbolSCVal("temp"), TEMPORARY,
                              DATA_ENTRY);
    putWithFootprint("temp", 0, contractKeys, {lk}, 1000, true, TEMPORARY);

    // Close ledgers until temp entry is evicted
    uint32 currLedger = app->getLedgerManager().getLastClosedLedgerNum();
    for (; currLedger <= 4096; ++currLedger)
    {
        closeLedgerOn(*app, currLedger, 2, 1, 2016);
    }

    // Check that temp entry has expired
    checkContractDataExpirationState(
        "temp", TEMPORARY, app->getLedgerManager().getLastClosedLedgerNum(),
        false);

    // close one more ledger to trigger the eviction
    closeLedgerOn(*app, 4097, 2, 1, 2016);

    XDRInputFileStream in;
    in.open(metaPath);
    LedgerCloseMeta lcm;
    uint32_t maxSeq = 0;
    bool evicted = false;
    while (in.readOne(lcm))
    {
        REQUIRE(lcm.v() == 2);
        if (lcm.v2().ledgerHeader.header.ledgerSeq == 4097)
        {
            REQUIRE(lcm.v2().evictedTemporaryLedgerKeys.size() == 1);
            REQUIRE(lcm.v2().evictedTemporaryLedgerKeys[0] == lk);
            evicted = true;
        }
        else
        {
            REQUIRE(lcm.v2().evictedTemporaryLedgerKeys.empty());
        }
    }

    REQUIRE(evicted);
}

TEST_CASE("failed invocation with diagnostics", "[tx][soroban]")
{
    // This test calls the add_i32 contract with two numbers that cause an
    // overflow. Because we have diagnostics on, we will see two events - The
    // diagnostic "fn_call" event, and the event that the add_i32 contract
    // emits.

    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS = true;
    auto app = createTestApplication(clock, cfg);
    overrideSorobanNetworkConfigForTest(*app);
    auto root = TestAccount::createRoot(*app);

    auto const addI32Wasm = rust_bridge::get_test_wasm_add_i32();
    auto contractKeys = deployContractWithSourceAccount(*app, addI32Wasm);
    auto const& contractID = contractKeys[0].contractData().contract;

    auto sc1 = makeI32(7);
    auto scMax = makeI32(INT32_MAX);

    Operation op;
    op.body.type(INVOKE_HOST_FUNCTION);
    auto& ihf = op.body.invokeHostFunctionOp().hostFunction;
    ihf.type(HOST_FUNCTION_TYPE_INVOKE_CONTRACT);
    ihf.invokeContract().contractAddress = contractID;
    ihf.invokeContract().functionName = makeSymbol("add");
    ihf.invokeContract().args = {sc1, scMax};
    SorobanResources resources;
    resources.footprint.readOnly = contractKeys;
    resources.instructions = 2'000'000;
    resources.readBytes = 2000;
    resources.writeBytes = 1000;

    auto tx = sorobanTransactionFrameFromOps(app->getNetworkID(), root, {op},
                                             {}, resources, 100'000, 1200);
    LedgerTxn ltx(app->getLedgerTxnRoot());
    TransactionMetaFrame txm(ltx.loadHeader().current().ledgerVersion);
    REQUIRE(tx->checkValid(*app, ltx, 0, 0, 0));
    REQUIRE(!tx->apply(*app, ltx, txm));
    ltx.commit();

    auto const& opEvents = txm.getXDR().v3().sorobanMeta->diagnosticEvents;
    REQUIRE(opEvents.size() == 21);

    auto const& call_ev = opEvents.at(0);
    REQUIRE(!call_ev.inSuccessfulContractCall);
    REQUIRE(call_ev.event.type == ContractEventType::DIAGNOSTIC);
    REQUIRE(call_ev.event.body.v0().data.type() == SCV_VEC);

    auto const& contract_ev = opEvents.at(1);
    REQUIRE(!contract_ev.inSuccessfulContractCall);
    REQUIRE(contract_ev.event.type == ContractEventType::CONTRACT);
    REQUIRE(contract_ev.event.body.v0().data.type() == SCV_VEC);

    auto const& read_entry_ev = opEvents.at(2);
    REQUIRE(!read_entry_ev.inSuccessfulContractCall);
    REQUIRE(read_entry_ev.event.type == ContractEventType::DIAGNOSTIC);
    auto const& v0 = read_entry_ev.event.body.v0();
    REQUIRE((v0.topics.size() == 2 && v0.topics.at(0).sym() == "core_metrics" &&
             v0.topics.at(1).sym() == "read_entry"));
    REQUIRE(v0.data.type() == SCV_U64);
}

TEST_CASE("complex contract", "[tx][soroban]")
{
    auto complexTest = [&](bool enableDiagnostics) {
        VirtualClock clock;
        auto cfg = getTestConfig();
        cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS = enableDiagnostics;
        auto app = createTestApplication(clock, cfg);
        overrideSorobanNetworkConfigForTest(*app);
        auto root = TestAccount::createRoot(*app);

        auto const complexWasm = rust_bridge::get_test_wasm_complex();

        auto contractKeys = deployContractWithSourceAccount(*app, complexWasm);
        auto const& contractID = contractKeys[0].contractData().contract;

        Operation op;
        op.body.type(INVOKE_HOST_FUNCTION);
        auto& ihf = op.body.invokeHostFunctionOp().hostFunction;
        ihf.type(HOST_FUNCTION_TYPE_INVOKE_CONTRACT);
        ihf.invokeContract().contractAddress = contractID;
        ihf.invokeContract().functionName = makeSymbol("go");

        // Contract writes a single `data` CONTRACT_DATA entry.
        LedgerKey dataKey(LedgerEntryType::CONTRACT_DATA);
        dataKey.contractData().contract = contractID;
        dataKey.contractData().key = makeSymbolSCVal("data");

        SorobanResources resources;
        resources.footprint.readOnly = contractKeys;
        resources.footprint.readWrite = {dataKey};
        resources.instructions = 4'000'000;
        resources.readBytes = 3000;
        resources.writeBytes = 1000;

        auto verifyDiagnosticEvents =
            [&](xdr::xvector<DiagnosticEvent> events) {
                REQUIRE(events.size() == 22);

                auto call_ev = events.at(0);
                REQUIRE(call_ev.event.type == ContractEventType::DIAGNOSTIC);
                REQUIRE(call_ev.event.body.v0().data.type() == SCV_VOID);

                auto contract_ev = events.at(1);
                REQUIRE(contract_ev.event.type == ContractEventType::CONTRACT);
                REQUIRE(contract_ev.event.body.v0().data.type() == SCV_BYTES);

                auto return_ev = events.at(2);
                REQUIRE(return_ev.event.type == ContractEventType::DIAGNOSTIC);
                REQUIRE(return_ev.event.body.v0().data.type() == SCV_VOID);

                auto const& metrics_ev = events.back();
                REQUIRE(metrics_ev.event.type == ContractEventType::DIAGNOSTIC);
                auto const& v0 = metrics_ev.event.body.v0();
                REQUIRE(v0.topics.size() == 2);
                REQUIRE(v0.topics.at(0).sym() == "core_metrics");
                REQUIRE(v0.topics.at(1).sym() == "max_emit_event_byte");
                REQUIRE(v0.data.type() == SCV_U64);
            };

        SECTION("single op")
        {
            auto tx = sorobanTransactionFrameFromOps(
                app->getNetworkID(), root, {op}, {}, resources, 200'000, 1200);
            LedgerTxn ltx(app->getLedgerTxnRoot());
            TransactionMetaFrame txm(ltx.loadHeader().current().ledgerVersion);
            REQUIRE(tx->checkValid(*app, ltx, 0, 0, 0));
            REQUIRE(tx->apply(*app, ltx, txm));
            ltx.commit();

            // Contract should have emitted a single event carrying a `Bytes`
            // value.
            REQUIRE(txm.getXDR().v3().sorobanMeta->events.size() == 1);
            REQUIRE(txm.getXDR().v3().sorobanMeta->events.at(0).type ==
                    ContractEventType::CONTRACT);
            REQUIRE(txm.getXDR()
                        .v3()
                        .sorobanMeta->events.at(0)
                        .body.v0()
                        .data.type() == SCV_BYTES);

            if (enableDiagnostics)
            {
                verifyDiagnosticEvents(
                    txm.getXDR().v3().sorobanMeta->diagnosticEvents);
            }
            else
            {
                REQUIRE(
                    txm.getXDR().v3().sorobanMeta->diagnosticEvents.size() ==
                    0);
            }
        }
    };

    SECTION("diagnostics enabled")
    {
        complexTest(true);
    }
    SECTION("diagnostics disabled")
    {
        complexTest(false);
    }
}

TEST_CASE("Stellar asset contract XLM transfer",
          "[tx][soroban][invariant][conservationoflumens]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    overrideSorobanNetworkConfigForTest(*app);
    auto root = TestAccount::createRoot(*app);
    auto xlm = txtest::makeNativeAsset();

    // Create XLM contract
    HashIDPreimage preImage;
    preImage.type(ENVELOPE_TYPE_CONTRACT_ID);
    preImage.contractID().contractIDPreimage.type(
        CONTRACT_ID_PREIMAGE_FROM_ASSET);
    preImage.contractID().contractIDPreimage.fromAsset() = xlm;
    preImage.contractID().networkID = app->getNetworkID();
    auto contractID = makeContractAddress(xdrSha256(preImage));

    Operation createOp;
    createOp.body.type(INVOKE_HOST_FUNCTION);
    auto& createHF = createOp.body.invokeHostFunctionOp();
    createHF.hostFunction.type(HOST_FUNCTION_TYPE_CREATE_CONTRACT);
    auto& createContractArgs = createHF.hostFunction.createContract();

    ContractExecutable exec;
    exec.type(CONTRACT_EXECUTABLE_TOKEN);
    createContractArgs.contractIDPreimage.type(CONTRACT_ID_PREIMAGE_FROM_ASSET);
    createContractArgs.contractIDPreimage.fromAsset() = xlm;
    createContractArgs.executable = exec;

    SorobanResources createResources;
    createResources.instructions = 400'000;
    createResources.readBytes = 1000;
    createResources.writeBytes = 1000;

    LedgerKey contractExecutableKey(CONTRACT_DATA);
    contractExecutableKey.contractData().contract = contractID;
    contractExecutableKey.contractData().key =
        SCVal(SCValType::SCV_LEDGER_KEY_CONTRACT_INSTANCE);
    contractExecutableKey.contractData().durability =
        ContractDataDurability::PERSISTENT;
    contractExecutableKey.contractData().bodyType = DATA_ENTRY;

    createResources.footprint.readWrite = {contractExecutableKey};

    {
        // submit operation
        auto tx = sorobanTransactionFrameFromOps(
            app->getNetworkID(), root, {createOp}, {}, createResources, 200'000,
            1200);

        LedgerTxn ltx(app->getLedgerTxnRoot());
        TransactionMetaFrame txm(ltx.loadHeader().current().ledgerVersion);
        REQUIRE(tx->checkValid(*app, ltx, 0, 0, 0));
        REQUIRE(tx->apply(*app, ltx, txm));
        ltx.commit();
    }

    // transfer 10 XLM from root to contractID
    SCAddress fromAccount(SC_ADDRESS_TYPE_ACCOUNT);
    fromAccount.accountId() = root.getPublicKey();
    SCVal from(SCV_ADDRESS);
    from.address() = fromAccount;

    SCVal to =
        makeContractAddressSCVal(makeContractAddress(sha256("contract")));

    auto fn = makeSymbol("transfer");
    Operation transfer;
    transfer.body.type(INVOKE_HOST_FUNCTION);
    auto& ihf = transfer.body.invokeHostFunctionOp().hostFunction;
    ihf.type(HOST_FUNCTION_TYPE_INVOKE_CONTRACT);
    ihf.invokeContract().contractAddress = contractID;
    ihf.invokeContract().functionName = fn;
    ihf.invokeContract().args = {from, to, makeI128(10)};

    // build auth
    SorobanAuthorizedInvocation ai;
    ai.function.type(SOROBAN_AUTHORIZED_FUNCTION_TYPE_CONTRACT_FN);
    ai.function.contractFn() = ihf.invokeContract();

    SorobanAuthorizationEntry a;
    a.credentials.type(SOROBAN_CREDENTIALS_SOURCE_ACCOUNT);
    a.rootInvocation = ai;
    transfer.body.invokeHostFunctionOp().auth = {a};

    SorobanResources resources;
    resources.instructions = 2'000'000;
    resources.readBytes = 2000;
    resources.writeBytes = 1072;

    LedgerKey accountLedgerKey(ACCOUNT);
    accountLedgerKey.account().accountID = root.getPublicKey();

    LedgerKey balanceLedgerKey(CONTRACT_DATA);
    balanceLedgerKey.contractData().contract = contractID;
    SCVec balance = {makeSymbolSCVal("Balance"), to};
    SCVal balanceKey(SCValType::SCV_VEC);
    balanceKey.vec().activate() = balance;
    balanceLedgerKey.contractData().key = balanceKey;
    balanceLedgerKey.contractData().durability =
        ContractDataDurability::PERSISTENT;
    balanceLedgerKey.contractData().bodyType = DATA_ENTRY;

    resources.footprint.readOnly = {contractExecutableKey};

    resources.footprint.readWrite = {accountLedgerKey, balanceLedgerKey};

    {
        // submit operation
        auto tx = sorobanTransactionFrameFromOps(app->getNetworkID(), root,
                                                 {transfer}, {}, resources,
                                                 250'000, 1200);

        LedgerTxn ltx(app->getLedgerTxnRoot());
        TransactionMetaFrame txm(ltx.loadHeader().current().ledgerVersion);
        REQUIRE(tx->checkValid(*app, ltx, 0, 0, 0));
        REQUIRE(tx->apply(*app, ltx, txm));
        ltx.commit();
    }
}

TEST_CASE("errors roll back", "[tx][soroban]")
{
    // This tests that various sorts of error created inside a contract
    // cause the invocation to fail and that in turn aborts the tx.

    auto call_fn_check_failure = [&](std::string const& name) {
        VirtualClock clock;
        auto app = createTestApplication(clock, getTestConfig());
        overrideSorobanNetworkConfigForTest(*app);
        auto root = TestAccount::createRoot(*app);

        auto const errWasm = rust_bridge::get_test_wasm_err();
        auto contractKeys = deployContractWithSourceAccount(*app, errWasm);
        auto const& contractID = contractKeys[0].contractData().contract;

        Operation op;
        op.body.type(INVOKE_HOST_FUNCTION);
        auto& ihf = op.body.invokeHostFunctionOp().hostFunction;
        ihf.type(HOST_FUNCTION_TYPE_INVOKE_CONTRACT);
        ihf.invokeContract().contractAddress = contractID;
        ihf.invokeContract().functionName = makeSymbol(name);

        SorobanResources resources;
        resources.footprint.readOnly = contractKeys;
        resources.instructions = 2'000'000;
        resources.readBytes = 3000;

        auto tx = sorobanTransactionFrameFromOps(
            app->getNetworkID(), root, {op}, {}, resources, 100'000, 1200);
        LedgerTxn ltx(app->getLedgerTxnRoot());
        TransactionMetaFrame txm(ltx.loadHeader().current().ledgerVersion);
        REQUIRE(tx->checkValid(*app, ltx, 0, 0, 0));
        REQUIRE(!tx->apply(*app, ltx, txm));
        REQUIRE(tx->getResult()
                    .result.results()
                    .at(0)
                    .tr()
                    .invokeHostFunctionResult()
                    .code() == INVOKE_HOST_FUNCTION_TRAPPED);
        REQUIRE(tx->getResultCode() == txFAILED);
    };

    for (auto name :
         {"err_eek", "err_err", "ok_err", "ok_val_err", "err", "val"})
    {
        call_fn_check_failure(name);
    }
}

static void
overrideNetworkSettingsToMin(Application& app)
{
    LedgerTxn ltx(app.getLedgerTxnRoot());

    ltx.load(configSettingKey(
                 ConfigSettingID::CONFIG_SETTING_CONTRACT_MAX_SIZE_BYTES))
        .current()
        .data.configSetting()
        .contractMaxSizeBytes() =
        MinimumSorobanNetworkConfig::MAX_CONTRACT_SIZE;
    ltx.load(configSettingKey(
                 ConfigSettingID::CONFIG_SETTING_CONTRACT_DATA_KEY_SIZE_BYTES))
        .current()
        .data.configSetting()
        .contractDataKeySizeBytes() =
        MinimumSorobanNetworkConfig::MAX_CONTRACT_DATA_KEY_SIZE_BYTES;
    ltx
        .load(configSettingKey(
            ConfigSettingID::CONFIG_SETTING_CONTRACT_DATA_ENTRY_SIZE_BYTES))
        .current()
        .data.configSetting()
        .contractDataEntrySizeBytes() =
        MinimumSorobanNetworkConfig::MAX_CONTRACT_DATA_ENTRY_SIZE_BYTES;
    auto& bandwidth =
        ltx.load(configSettingKey(
                     ConfigSettingID::CONFIG_SETTING_CONTRACT_BANDWIDTH_V0))
            .current()
            .data.configSetting()
            .contractBandwidth();

    bandwidth.txMaxSizeBytes = MinimumSorobanNetworkConfig::TX_MAX_SIZE_BYTES;
    bandwidth.ledgerMaxTxsSizeBytes = bandwidth.txMaxSizeBytes;

    auto& compute =
        ltx.load(configSettingKey(
                     ConfigSettingID::CONFIG_SETTING_CONTRACT_COMPUTE_V0))
            .current()
            .data.configSetting()
            .contractCompute();
    compute.txMaxInstructions =
        MinimumSorobanNetworkConfig::TX_MAX_INSTRUCTIONS;
    compute.ledgerMaxInstructions = compute.txMaxInstructions;
    compute.txMemoryLimit = MinimumSorobanNetworkConfig::MEMORY_LIMIT;

    auto& costEntry =
        ltx.load(configSettingKey(
                     ConfigSettingID::CONFIG_SETTING_CONTRACT_LEDGER_COST_V0))
            .current()
            .data.configSetting()
            .contractLedgerCost();
    costEntry.txMaxReadLedgerEntries =
        MinimumSorobanNetworkConfig::TX_MAX_READ_LEDGER_ENTRIES;
    costEntry.txMaxReadBytes = MinimumSorobanNetworkConfig::TX_MAX_READ_BYTES;

    costEntry.txMaxWriteLedgerEntries =
        MinimumSorobanNetworkConfig::TX_MAX_WRITE_LEDGER_ENTRIES;
    costEntry.txMaxWriteBytes = MinimumSorobanNetworkConfig::TX_MAX_WRITE_BYTES;

    costEntry.ledgerMaxReadLedgerEntries = costEntry.txMaxReadLedgerEntries;
    costEntry.ledgerMaxReadBytes = costEntry.txMaxReadBytes;
    costEntry.ledgerMaxWriteLedgerEntries = costEntry.txMaxWriteLedgerEntries;
    costEntry.ledgerMaxWriteBytes = costEntry.txMaxWriteBytes;

    auto& exp = ltx.load(configSettingKey(
                             ConfigSettingID::CONFIG_SETTING_STATE_EXPIRATION))
                    .current()
                    .data.configSetting()
                    .stateExpirationSettings();
    exp.maxEntryExpiration =
        MinimumSorobanNetworkConfig::MAXIMUM_ENTRY_LIFETIME;
    exp.minPersistentEntryExpiration =
        MinimumSorobanNetworkConfig::MINIMUM_PERSISTENT_ENTRY_LIFETIME;
    exp.persistentRentRateDenominator = 1;
    exp.tempRentRateDenominator = 1;
    exp.maxEntriesToExpire = 1;
    exp.bucketListSizeWindowSampleSize = 1;
    exp.evictionScanSize = 1;
    exp.startingEvictionScanLevel = 1;
    exp.autoBumpLedgers = 0;

    ltx.load(configSettingKey(ConfigSettingID::CONFIG_SETTING_STATE_EXPIRATION))
        .current()
        .data.configSetting()
        .stateExpirationSettings() = exp;

    ltx.commit();

    auto& events =
        ltx.load(configSettingKey(
                     ConfigSettingID::CONFIG_SETTING_CONTRACT_EVENTS_V0))
            .current()
            .data.configSetting()
            .contractEvents();
    events.txMaxContractEventsSizeBytes =
        MinimumSorobanNetworkConfig::TX_MAX_CONTRACT_EVENTS_SIZE_BYTES;

    ltx.commit();
    // submit a no-op upgrade so the cached soroban settings are updated.
    auto upgrade = LedgerUpgrade{LEDGER_UPGRADE_MAX_SOROBAN_TX_SET_SIZE};
    upgrade.newMaxSorobanTxSetSize() = 1;
    executeUpgrade(app, upgrade);
}

TEST_CASE("settings upgrade", "[tx][soroban][upgrades]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS = true;
    cfg.EXPERIMENTAL_BUCKETLIST_DB = false;
    auto app = createTestApplication(clock, cfg);
    auto root = TestAccount::createRoot(*app);
    auto& lm = app->getLedgerManager();

    auto runTest = [&]() {
        {
            LedgerTxn ltx(app->getLedgerTxnRoot());

            // make sure LedgerManager picked up cached values by looking at
            // a couple settings
            auto const& networkConfig = lm.getSorobanNetworkConfig(ltx);

            REQUIRE(networkConfig.txMaxReadBytes() ==
                    MinimumSorobanNetworkConfig::TX_MAX_READ_BYTES);
            REQUIRE(networkConfig.ledgerMaxReadBytes() ==
                    networkConfig.txMaxReadBytes());
        }

        auto const writeByteWasm = rust_bridge::get_write_bytes();

        SorobanResources uploadResources{};
        uploadResources.instructions = 200'000;
        uploadResources.readBytes = 1000;
        uploadResources.writeBytes = 1000;

        SorobanResources createResources{};
        createResources.instructions = 200'000;
        createResources.readBytes = 1000;
        createResources.writeBytes = 300;
        auto contractKeys = deployContractWithSourceAccountWithResources(
            *app, writeByteWasm, uploadResources, createResources);
        auto const& contractID = contractKeys[0].contractData().contract;

        // build upgrade

        // This test assumes that all settings including and after
        // CONFIG_SETTING_BUCKETLIST_SIZE_WINDOW are not upgradeable, so they
        // won't be included in the upgrade.
        xdr::xvector<ConfigSettingEntry> updatedEntries;
        for (uint32_t i = 0;
             i < static_cast<uint32_t>(CONFIG_SETTING_BUCKETLIST_SIZE_WINDOW);
             ++i)
        {
            LedgerTxn ltx(app->getLedgerTxnRoot());
            auto costEntry =
                ltx.load(configSettingKey(static_cast<ConfigSettingID>(i)));
            updatedEntries.emplace_back(
                costEntry.current().data.configSetting());
        }

        // Update one of the settings. The rest will be the same so will not get
        // upgraded, but this will still test that the limits work when writing
        // all settings to the contract.
        auto& cost = updatedEntries.at(static_cast<uint32_t>(
            ConfigSettingID::CONFIG_SETTING_CONTRACT_LEDGER_COST_V0));
        // check a couple settings to make sure they're at the minimum
        REQUIRE(cost.contractLedgerCost().txMaxReadLedgerEntries ==
                MinimumSorobanNetworkConfig::TX_MAX_READ_LEDGER_ENTRIES);
        REQUIRE(cost.contractLedgerCost().txMaxReadBytes ==
                MinimumSorobanNetworkConfig::TX_MAX_READ_BYTES);
        REQUIRE(cost.contractLedgerCost().txMaxWriteBytes ==
                MinimumSorobanNetworkConfig::TX_MAX_WRITE_BYTES);
        REQUIRE(cost.contractLedgerCost().ledgerMaxReadLedgerEntries ==
                cost.contractLedgerCost().txMaxReadLedgerEntries);
        REQUIRE(cost.contractLedgerCost().ledgerMaxReadBytes ==
                cost.contractLedgerCost().txMaxReadBytes);
        REQUIRE(cost.contractLedgerCost().ledgerMaxWriteBytes ==
                cost.contractLedgerCost().txMaxWriteBytes);
        cost.contractLedgerCost().feeRead1KB = 1000;

        ConfigUpgradeSet upgradeSet;
        upgradeSet.updatedEntry = updatedEntries;

        auto xdr = xdr::xdr_to_opaque(upgradeSet);
        auto upgrade_hash = sha256(xdr);

        // write upgrade
        Operation op;
        op.body.type(INVOKE_HOST_FUNCTION);
        auto& ihf = op.body.invokeHostFunctionOp().hostFunction;
        ihf.type(HOST_FUNCTION_TYPE_INVOKE_CONTRACT);
        ihf.invokeContract().contractAddress = contractID;
        ihf.invokeContract().functionName = makeSymbol("write");
        ihf.invokeContract().args.emplace_back(makeBytes(xdr));

        LedgerKey upgrade(CONTRACT_DATA);
        upgrade.contractData().durability = TEMPORARY;
        upgrade.contractData().contract = contractID;
        upgrade.contractData().bodyType = DATA_ENTRY;
        upgrade.contractData().key =
            makeBytes(xdr::xdr_to_opaque(upgrade_hash));

        SorobanResources resources{};
        resources.footprint.readOnly = contractKeys;
        resources.footprint.readWrite = {upgrade};
        resources.instructions = 2'000'000;
        resources.readBytes = 3000;
        resources.writeBytes = 2000;

        auto tx = sorobanTransactionFrameFromOps(
            app->getNetworkID(), root, {op}, {}, resources, 20'000'000, 30'000);

        {
            LedgerTxn ltx(app->getLedgerTxnRoot());
            REQUIRE(tx->checkValid(*app, ltx, 0, 0, 0));
            TransactionMetaFrame txm(ltx.loadHeader().current().ledgerVersion);
            REQUIRE(tx->apply(*app, ltx, txm));
            ltx.commit();
        }

        // arm the upgrade through commandHandler. This isn't required
        // because we'll trigger the upgrade through externalizeValue, but
        // this will test the submission and deserialization code.
        ConfigUpgradeSetKey key;
        key.contentHash = upgrade_hash;
        key.contractID = contractID.contractId();

        auto& commandHandler = app->getCommandHandler();

        std::string command = "mode=set&configupgradesetkey=";
        command += decoder::encode_b64(xdr::xdr_to_opaque(key));
        command += "&upgradetime=2000-07-21T22:04:00Z";

        std::string ret;
        commandHandler.upgrades(command, ret);
        REQUIRE(ret == "");

        // trigger upgrade
        auto ledgerUpgrade = LedgerUpgrade{LEDGER_UPGRADE_CONFIG};
        ledgerUpgrade.newConfig() = key;

        auto const& lcl = lm.getLastClosedLedgerHeader();
        auto txSet = TxSetFrame::makeEmpty(lcl);
        auto lastCloseTime = lcl.header.scpValue.closeTime;
        app->getHerder().externalizeValue(
            txSet, lcl.header.ledgerSeq + 1, lastCloseTime,
            {LedgerTestUtils::toUpgradeType(ledgerUpgrade)});

        // validate upgrade succeeded
        {
            auto costKey = configSettingKey(
                ConfigSettingID::CONFIG_SETTING_CONTRACT_LEDGER_COST_V0);
            LedgerTxn ltx(app->getLedgerTxnRoot());
            auto costEntry = ltx.load(costKey);
            REQUIRE(costEntry.current()
                        .data.configSetting()
                        .contractLedgerCost()
                        .feeRead1KB == 1000);
        }
    };
    SECTION("from init settings")
    {
        runTest();
    }
    SECTION("from min settings")
    {
        overrideNetworkSettingsToMin(*app);
        runTest();
    }
}
#endif
