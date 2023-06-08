// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "xdr/Stellar-transaction.h"
#include <iterator>
#include <stdexcept>
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION

#include "crypto/SecretKey.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "rust/RustBridge.h"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/InvokeHostFunctionOpFrame.h"
#include "transactions/SignatureUtils.h"
#include "transactions/TransactionUtils.h"
#include "util/Decoder.h"
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
    SCVal val(SCValType::SCV_CONTRACT_EXECUTABLE);
    val.exec().type(SCContractExecutableType::SCCONTRACT_EXECUTABLE_WASM_REF);
    val.exec().wasm_id() = hash;
    return val;
}

SCVal
makeContractAddress(Hash const& hash)
{
    SCVal val(SCValType::SCV_ADDRESS);
    val.address().type(SC_ADDRESS_TYPE_CONTRACT);
    val.address().contractId() = hash;
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
makeSymbol(std::string const& str)
{
    SCVal val(SCV_SYMBOL);
    val.sym().assign(str.begin(), str.end());
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
makeVoid()
{
    SCVal val(SCV_VOID);
    return val;
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
    REQUIRE(body.leType() == DATA_ENTRY);
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
    auto ltxe = loadContractData(ltx2, contractID, executableKey,
                                 CONTRACT_INSTANCE_CONTRACT_DATA_TYPE);
    REQUIRE(ltxe);

    auto const& cd = ltxe.current().data.contractData();
    REQUIRE(cd.type == CONTRACT_INSTANCE_CONTRACT_DATA_TYPE);
    REQUIRE(cd.body.leType() == DATA_ENTRY);
    REQUIRE(cd.body.data().val == makeWasmRefScContractCode(expectedWasmHash));
}

static xdr::xvector<LedgerKey>
deployContractWithSourceAccount(Application& app, RustBuf const& contractWasm,
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

    SorobanResources uploadResources;
    uploadResources.footprint.readWrite = {contractCodeLedgerKey};
    uploadResources.instructions = 200'000;
    uploadResources.readBytes = 1000;
    uploadResources.writeBytes = 5000;
    uploadResources.extendedMetaDataSizeBytes = 6000;
    submitTxToUploadWasm(app, uploadOp, uploadResources,
                         contractCodeLedgerKey.contractCode().hash,
                         uploadHF.wasm(), 100'000, 1'200);

    // Check lifetimes for contract code
    {
        LedgerTxn ltx(app.getLedgerTxnRoot());
        auto networkConfig =
            app.getLedgerManager().getSorobanNetworkConfig(ltx);
        auto lcl = app.getLedgerManager().getLastClosedLedgerNum();
        auto expectedExpiration = networkConfig.stateExpirationSettings()
                                      .minRestorableEntryExpiration +
                                  lcl;

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
    createContractArgs.executable.type(SCCONTRACT_EXECUTABLE_WASM_REF);
    createContractArgs.executable.wasm_id() =
        contractCodeLedgerKey.contractCode().hash;

    SorobanAuthorizationEntry auth;
    auth.credentials.type(SOROBAN_CREDENTIALS_SOURCE_ACCOUNT);
    auth.rootInvocation.function.type(
        SOROBAN_AUTHORIZED_FUNCTION_TYPE_CREATE_CONTRACT_HOST_FN);
    auth.rootInvocation.function.createContractHostFn().contractIDPreimage =
        idPreimage;
    auth.rootInvocation.function.createContractHostFn().executable.type(
        SCCONTRACT_EXECUTABLE_WASM_REF);
    auth.rootInvocation.function.createContractHostFn().executable.wasm_id() =
        contractCodeLedgerKey.contractCode().hash;
    createOp.body.invokeHostFunctionOp().auth = {auth};

    SCVal scContractSourceRefKey(SCValType::SCV_LEDGER_KEY_CONTRACT_EXECUTABLE);

    LedgerKey contractSourceRefLedgerKey;
    contractSourceRefLedgerKey.type(CONTRACT_DATA);
    contractSourceRefLedgerKey.contractData().contractID = contractID;
    contractSourceRefLedgerKey.contractData().key = scContractSourceRefKey;
    contractSourceRefLedgerKey.contractData().type =
        CONTRACT_INSTANCE_CONTRACT_DATA_TYPE;

    SorobanResources createResources;
    createResources.footprint.readOnly = {contractCodeLedgerKey};
    createResources.footprint.readWrite = {contractSourceRefLedgerKey};
    createResources.instructions = 200'000;
    createResources.readBytes = 5000;
    createResources.writeBytes = 5000;
    createResources.extendedMetaDataSizeBytes = 6000;

    submitTxToCreateContract(
        app, createOp, createResources, contractID, scContractSourceRefKey,
        contractCodeLedgerKey.contractCode().hash, 100'000, 1200);

    // Check lifetimes for contract instance
    LedgerTxn ltx(app.getLedgerTxnRoot());
    auto networkConfig = app.getLedgerManager().getSorobanNetworkConfig(ltx);
    auto lcl = app.getLedgerManager().getLastClosedLedgerNum();
    auto expectedExpiration =
        networkConfig.stateExpirationSettings().minRestorableEntryExpiration +
        lcl;

    auto instanceLtxe = ltx.load(contractSourceRefLedgerKey);
    REQUIRE(instanceLtxe);
    REQUIRE(instanceLtxe.current().data.contractData().expirationLedgerSeq ==
            expectedExpiration);

    return {contractSourceRefLedgerKey, contractCodeLedgerKey};
}

TEST_CASE("basic contract invocation", "[tx][soroban]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.EXPERIMENTAL_BUCKETLIST_DB = false;
    auto app = createTestApplication(clock, cfg);
    auto root = TestAccount::createRoot(*app);
    int64_t initBalance = root.getBalance();

    auto const addI32Wasm = rust_bridge::get_test_wasm_add_i32();

    auto contractKeys = deployContractWithSourceAccount(*app, addI32Wasm);
    auto const& contractID = contractKeys[0].contractData().contractID;
    auto call = [&](SorobanResources const& resources, SCVec const& parameters,
                    bool success) {
        Operation op;
        op.body.type(INVOKE_HOST_FUNCTION);
        auto& ihf = op.body.invokeHostFunctionOp().hostFunction;
        ihf.type(HOST_FUNCTION_TYPE_INVOKE_CONTRACT);
        ihf.invokeContract() = parameters;

        auto tx = sorobanTransactionFrameFromOps(
            app->getNetworkID(), root, {op}, {}, resources, 100'000, 1200);
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
            REQUIRE(tx->getFeeBid() == 62'956);
            // Initially we store in result the charge for resources plus
            // minimum inclusion  fee bid (currently equivalent to the network
            // `baseFee` of 100).
            int64_t baseCharged = (tx->getFullFee() - tx->getFeeBid()) + 100;
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
                        568);
            }
            // The account should receive a refund for unspent metadata fee.
            REQUIRE(root.getBalance() - balanceAfterFeeCharged == 568);
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
                auto changesAfter = txm.getChangesAfter();
                REQUIRE(changesAfter.size() == 2);
                REQUIRE(changesAfter[1].updated().data.account().balance -
                            changesAfter[0].state().data.account().balance ==
                        586);
            }
            // The account should receive a full refund for metadata
            // in case of tx failure.
            REQUIRE(root.getBalance() - balanceAfterFeeCharged == 586);
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
                resultVal = txm.getXDR().v3().returnValue;

                InvokeHostFunctionSuccessPreImage success2;
                success2.returnValue = resultVal;
                success2.events = txm.getXDR().v3().events;

                REQUIRE(ores.tr().invokeHostFunctionResult().success() ==
                        xdrSha256(success2));
            }
        }
        return resultVal;
    };

    auto scContractID = makeContractAddress(contractID);
    auto scFunc = makeSymbol("add");
    auto sc7 = makeI32(7);
    auto sc16 = makeI32(16);
    SorobanResources resources;
    resources.footprint.readOnly = contractKeys;
    resources.instructions = 2'000'000;
    resources.readBytes = 2000;
    resources.writeBytes = 1000;
    resources.extendedMetaDataSizeBytes = 3000;

    SECTION("correct invocation")
    {
        call(resources, {scContractID, scFunc, sc7, sc16}, true);
        REQUIRE(app->getMetrics()
                    .NewTimer({"soroban", "host-fn-op", "exec"})
                    .count() != 0);
        REQUIRE(app->getMetrics()
                    .NewMeter({"soroban", "host-fn-op", "success"}, "call")
                    .count() != 0);
    }

    SECTION("incorrect invocation parameters")
    {
        // Too few parameters
        call(resources, {}, false);
        call(resources, {scContractID}, false);
        call(resources, {scContractID, scFunc}, false);
        call(resources, {scContractID, scFunc, sc7}, false);
        // Too many parameters
        call(resources, {scContractID, scFunc, sc7, sc16, makeI32(0)}, false);
    }

    SECTION("insufficient instructions")
    {
        resources.instructions = 10000;
        call(resources, {scContractID, scFunc, sc7, sc16}, false);
    }
    SECTION("insufficient read bytes")
    {
        resources.readBytes = 100;
        call(resources, {scContractID, scFunc, sc7, sc16}, false);
    }
}

TEST_CASE("contract storage", "[tx][soroban]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    auto root = TestAccount::createRoot(*app);

    auto const contractDataWasm = rust_bridge::get_test_wasm_contract_data();

    auto contractKeys = deployContractWithSourceAccount(*app, contractDataWasm);
    auto const& contractID = contractKeys[0].contractData().contractID;

    auto checkContractData = [&](SCVal const& key, ContractDataType type,
                                 SCVal const* val) {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        auto ltxe = loadContractData(ltx, contractID, key, type);
        if (val)
        {
            REQUIRE(ltxe);

            auto const& body = ltxe.current().data.contractData().body;
            REQUIRE(body.leType() == DATA_ENTRY);
            REQUIRE(body.data().val == *val);
        }
        else
        {
            REQUIRE(!ltxe);
        }
    };

    auto checkContractDataLifetime = [&](std::string const& key,
                                         ContractDataType type,
                                         uint32_t expectedExpiration,
                                         uint32_t flags = 0) {
        auto keySymbol = makeSymbol(key);
        LedgerTxn ltx(app->getLedgerTxnRoot());
        auto ltxe = loadContractData(ltx, contractID, keySymbol, type);
        REQUIRE(ltxe);
        REQUIRE(getExpirationLedger(ltxe.current()) == expectedExpiration);
        REQUIRE(ltxe.current().data.contractData().body.data().flags == flags);
    };

    auto createTx = [&](xdr::xvector<LedgerKey> const& readOnly,
                        xdr::xvector<LedgerKey> const& readWrite,
                        uint32_t writeBytes, SCVec const& invokeVector)
        -> std::tuple<TransactionFrameBasePtr, std::shared_ptr<LedgerTxn>,
                      std::shared_ptr<TransactionMetaFrame>> {
        Operation op;
        op.body.type(INVOKE_HOST_FUNCTION);
        auto& ihf = op.body.invokeHostFunctionOp().hostFunction;
        ihf.type(HOST_FUNCTION_TYPE_INVOKE_CONTRACT);
        ihf.invokeContract() = invokeVector;
        SorobanResources resources;
        resources.footprint.readOnly = readOnly;
        resources.footprint.readWrite = readWrite;
        resources.instructions = 2'000'000;
        resources.readBytes = 5000;
        resources.writeBytes = writeBytes;
        resources.extendedMetaDataSizeBytes = 3000;

        auto tx = sorobanTransactionFrameFromOps(
            app->getNetworkID(), root, {op}, {}, resources, 120'000, 1200);
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
                                ContractDataType type,
                                std::optional<uint32_t> flags) {
        auto keySymbol = makeSymbol(key);
        auto valU64 = makeU64(val);
        auto scFlags = flags ? makeU32(*flags) : makeVoid();

        std::string funcStr;
        switch (type)
        {
        case TEMPORARY:
            funcStr = "put_temporary";
            break;
        case MERGEABLE:
            funcStr = "put_mergeable";
            break;
        case EXCLUSIVE:
            funcStr = "put_exclusive";
            break;
        }

        auto [tx, ltx, txm] =
            createTx(readOnly, readWrite, writeBytes,
                     {makeContractAddress(contractID), makeSymbol(funcStr),
                      keySymbol, valU64, scFlags});

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

    auto put = [&](std::string const& key, uint64_t val, ContractDataType type,
                   std::optional<uint32_t> flags = std::nullopt) {
        putWithFootprint(
            key, val, contractKeys,
            {contractDataKey(contractID, makeSymbol(key), type, DATA_ENTRY)},
            1000, true, type, flags);
    };

    auto bumpWithFootprint = [&](std::string const& key, uint32_t bumpAmount,
                                 xdr::xvector<LedgerKey> const& readOnly,
                                 xdr::xvector<LedgerKey> const& readWrite,
                                 bool expectSuccess, ContractDataType type) {
        auto keySymbol = makeSymbol(key);
        auto bumpAmountU32 = makeU32(bumpAmount);

        std::string funcStr;
        switch (type)
        {
        case TEMPORARY:
            funcStr = "bump_temporary";
            break;
        case MERGEABLE:
            funcStr = "bump_mergeable";
            break;
        case EXCLUSIVE:
            funcStr = "bump_exclusive";
            break;
        }

        // TODO: Better bytes to write value
        auto [tx, ltx, txm] =
            createTx(readOnly, readWrite, 1000,
                     {makeContractAddress(contractID), makeSymbol(funcStr),
                      keySymbol, bumpAmountU32});

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

    auto bump = [&](std::string const& key, ContractDataType type,
                    uint32_t bumpAmount) {
        bumpWithFootprint(
            key, bumpAmount, contractKeys,
            {contractDataKey(contractID, makeSymbol(key), type, DATA_ENTRY)},
            true, type);
    };

    auto delWithFootprint = [&](std::string const& key,
                                xdr::xvector<LedgerKey> const& readOnly,
                                xdr::xvector<LedgerKey> const& readWrite,
                                bool expectSuccess, ContractDataType type) {
        auto keySymbol = makeSymbol(key);

        std::string funcStr;
        switch (type)
        {
        case TEMPORARY:
            funcStr = "del_temporary";
            break;
        case MERGEABLE:
            funcStr = "del_mergeable";
            break;
        case EXCLUSIVE:
            funcStr = "del_exclusive";
            break;
        }

        // TODO: Better bytes to write value
        auto [tx, ltx, txm] = createTx(
            readOnly, readWrite, 1000,
            {makeContractAddress(contractID), makeSymbol(funcStr), keySymbol});

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

    auto del = [&](std::string const& key, ContractDataType type) {
        delWithFootprint(
            key, contractKeys,
            {contractDataKey(contractID, makeSymbol(key), type, DATA_ENTRY)},
            true, type);
    };

    SECTION("default limits")
    {

        put("key1", 0, MERGEABLE);
        put("key2", 21, MERGEABLE);

        // Failure: contract data isn't in footprint
        putWithFootprint("key1", 88, contractKeys, {}, 1000, false, MERGEABLE,
                         std::nullopt);
        delWithFootprint("key1", contractKeys, {}, false, MERGEABLE);

        // Failure: contract data is read only
        auto readOnlyFootprint = contractKeys;
        readOnlyFootprint.push_back(contractDataKey(
            contractID, makeSymbol("key2"), MERGEABLE, DATA_ENTRY));
        putWithFootprint("key2", 888888, readOnlyFootprint, {}, 1000, false,
                         MERGEABLE, std::nullopt);
        delWithFootprint("key2", readOnlyFootprint, {}, false, MERGEABLE);

        // Failure: insufficient write bytes
        putWithFootprint("key2", 88888, contractKeys,
                         {contractDataKey(contractID, makeSymbol("key2"),
                                          MERGEABLE, DATA_ENTRY)},
                         1, false, MERGEABLE, std::nullopt);

        put("key1", 9, MERGEABLE);
        put("key2", UINT64_MAX, MERGEABLE);

        del("key1", MERGEABLE);
        del("key2", MERGEABLE);
    }

    SorobanNetworkConfig refConfig;
    {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        refConfig = app->getLedgerManager().getSorobanNetworkConfig(ltx);
    }
    SECTION("failure: entry exceeds max size")
    {
        refConfig.maxContractDataKeySizeBytes() = 300;
        refConfig.maxContractDataEntrySizeBytes() = 1;
        app->getLedgerManager().setSorobanNetworkConfig(refConfig);
        // this fails due to the contract code itself exceeding the entry limit
        putWithFootprint("key2", 2, contractKeys,
                         {contractDataKey(contractID, makeSymbol("key2"),
                                          MERGEABLE, DATA_ENTRY)},
                         1000, false, MERGEABLE, std::nullopt);
    }

    SECTION("Same ScVal key, different types")
    {
        // Check that each type is in their own keyspace
        uint64_t uniqueVal = 0;
        uint64_t recreatableVal = 1;
        uint64_t temporaryVal = 2;
        put("key", uniqueVal, EXCLUSIVE);
        put("key", recreatableVal, MERGEABLE);
        put("key", temporaryVal, TEMPORARY);
        auto uniqueScVal = makeU64(uniqueVal);
        auto recreatableScVal = makeU64(recreatableVal);
        auto temporaryScVal = makeU64(temporaryVal);
        auto keySymbol = makeSymbol("key");
        checkContractData(keySymbol, EXCLUSIVE, &uniqueScVal);
        checkContractData(keySymbol, MERGEABLE, &recreatableScVal);
        checkContractData(keySymbol, TEMPORARY, &temporaryScVal);

        put("key2", 3, EXCLUSIVE);
        auto key2Symbol = makeSymbol("key2");
        auto uniqueScVal2 = makeU64(3);
        checkContractData(key2Symbol, EXCLUSIVE, &uniqueScVal2);
        checkContractData(key2Symbol, MERGEABLE, nullptr);
        checkContractData(key2Symbol, TEMPORARY, nullptr);
    }

    auto const& stateExpirationSettings = refConfig.stateExpirationSettings();
    auto autoBump = stateExpirationSettings.autoBumpLedgers;
    auto lcl = app->getLedgerManager().getLastClosedLedgerNum();

    SECTION("Enforce rent minimums")
    {
        put("unique", 0, EXCLUSIVE);
        put("recreateable", 0, MERGEABLE);
        put("temp", 0, TEMPORARY);

        auto expectedTempLifetime =
            stateExpirationSettings.minTempEntryExpiration + lcl;
        auto expectedRestorableLifetime =
            stateExpirationSettings.minRestorableEntryExpiration + lcl;

        checkContractDataLifetime("unique", EXCLUSIVE,
                                  expectedRestorableLifetime);
        checkContractDataLifetime("recreateable", MERGEABLE,
                                  expectedRestorableLifetime);
        checkContractDataLifetime("temp", TEMPORARY, expectedTempLifetime);
    }

    SECTION("autobump")
    {
        put("rw", 0, EXCLUSIVE);
        put("ro", 0, EXCLUSIVE);

        uint32_t flags = NO_AUTOBUMP;
        put("nobump", 0, EXCLUSIVE, flags);

        auto readOnlySet = contractKeys;
        readOnlySet.emplace_back(contractDataKey(contractID, makeSymbol("ro"),
                                                 EXCLUSIVE, DATA_ENTRY));

        auto readWriteSet = {contractDataKey(contractID, makeSymbol("nobump"),
                                             EXCLUSIVE, DATA_ENTRY),
                             contractDataKey(contractID, makeSymbol("rw"),
                                             EXCLUSIVE, DATA_ENTRY)};

        // Invoke contract with all keys in footprint
        putWithFootprint("rw", 1, readOnlySet, readWriteSet, 1000, true,
                         EXCLUSIVE, std::nullopt);

        auto expectedInitialLifetime =
            stateExpirationSettings.minRestorableEntryExpiration + lcl;

        checkContractDataLifetime("rw", EXCLUSIVE,
                                  expectedInitialLifetime + autoBump);
        checkContractDataLifetime("ro", EXCLUSIVE,
                                  expectedInitialLifetime + autoBump);
        checkContractDataLifetime("nobump", EXCLUSIVE, expectedInitialLifetime,
                                  flags);

        // Contract instance and WASM should have minimum life and 4 invocations
        // worth of autobumps
        LedgerTxn ltx(app->getLedgerTxnRoot());
        for (auto const& key : contractKeys)
        {
            uint32_t mult = key.type() == CONTRACT_CODE ? 5 : 4;
            auto ltxe = ltx.loadWithoutRecord(key);
            REQUIRE(ltxe);
            REQUIRE(getExpirationLedger(ltxe.current()) ==
                    expectedInitialLifetime + (autoBump * mult));
        }
    }

    // TODO: uncomment this when we implement the bump_contract_data host
    // function
    /* SECTION("manual bump")
    {
        put("key", 0, EXCLUSIVE);
        bump("key", EXCLUSIVE, 10'000);
        checkContractDataLifetime("key", EXCLUSIVE, 10'000 + lcl);

        // Lifetime already above 5'000, should be a nop (other than autobump)
        bump("key", EXCLUSIVE, 5'000);
        checkContractDataLifetime("key", EXCLUSIVE, 10'000 + autoBump + lcl);
    }

    SECTION("max lifetime")
    {
        // Check that manual bump doesn't go over max
        put("key", 0, EXCLUSIVE);
        bump("key", EXCLUSIVE, UINT32_MAX);

        auto maxLifetime = stateExpirationSettings.maxEntryExpiration + lcl;
        checkContractDataLifetime("key", EXCLUSIVE, maxLifetime);

        // Manual bump to almost max, then autobump to check that autobump
        // doesn't go over max
        put("key2", 0, EXCLUSIVE);
        bump("key2", EXCLUSIVE, stateExpirationSettings.maxEntryExpiration - 1);
        checkContractDataLifetime("key2", EXCLUSIVE, maxLifetime - 1);

        // Autobump should only add a single ledger to bring lifetime to max
        put("key2", 1, EXCLUSIVE);
        checkContractDataLifetime("key2", EXCLUSIVE, maxLifetime);
    } */

    // WIP

    // SECTION("read-only bumps use EXPIRATION_EXTENSION")
    // {
    //     put("ro", 0, EXCLUSIVE);

    //     // Create a second entry, but put "key" in the readonly set. Check
    //     ltx
    //     // before commiting to make sure EXPIRATION_EXTENSION is written
    //     instead
    //     // of a DATA_ENTRY
    //     auto keySymbol = makeSymbol("key2");
    //     auto valU64 = makeU64(0);
    //     auto readOnly = contractKeys;
    //     readOnly.emplace_back(
    //         contractDataKey(contractID, makeSymbol("ro"), EXCLUSIVE,
    //         DATA_ENTRY));

    //     auto [tx, ltx, txm] = createTx(
    //         readOnly,
    //         {contractDataKey(contractID, keySymbol, EXCLUSIVE, DATA_ENTRY)},
    //         1000, {makeContractAddress(contractID),
    //          makeSymbol("put_exclusive"), keySymbol, valU64, makeVoid()});

    //     REQUIRE(tx->apply(*app, *ltx, *txm));

    //     auto expectedExpiration =
    //         stateExpirationSettings.minRestorableEntryExpiration + lcl +
    //         autoBump;

    //     std::vector<LedgerEntry> init;
    //     std::vector<LedgerEntry> live;
    //     std::vector<LedgerKey> dead;
    //     ltx->getAllEntries(init, live, dead);

    //     auto roSymbol = makeSymbol("ro");
    //     auto dataKey =
    //         contractDataKey(contractID, roSymbol, EXCLUSIVE, DATA_ENTRY);
    //     auto extKey =
    //         contractDataKey(contractID, roSymbol, EXCLUSIVE,
    //         EXPIRATION_EXTENSION);

    //     // EXPIRATION_EXTENSION should never be init
    //     for (auto const& e : init)
    //     {
    //         auto k = LedgerEntryKey(e);
    //         REQUIRE((k != dataKey && k != extKey));
    //     }

    //     bool foundExtension = false;
    //     for (auto const& e : live)
    //     {
    //         auto k = LedgerEntryKey(e);
    //         REQUIRE(k != dataKey);
    //         if (k == extKey)
    //         {
    //             foundExtension = true;
    //             REQUIRE(getExpirationLedger(e) == expectedExpiration);
    //         }
    //     }

    //     REQUIRE(foundExtension);
    // }
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
    auto root = TestAccount::createRoot(*app);

    auto const addI32Wasm = rust_bridge::get_test_wasm_add_i32();
    auto contractKeys = deployContractWithSourceAccount(*app, addI32Wasm);
    auto const& contractID = contractKeys[0].contractData().contractID;

    auto sc1 = makeI32(7);
    auto scMax = makeI32(INT32_MAX);
    SCVec parameters = {makeContractAddress(contractID), makeSymbol("add"), sc1,
                        scMax};

    Operation op;
    op.body.type(INVOKE_HOST_FUNCTION);
    auto& ihf = op.body.invokeHostFunctionOp().hostFunction;
    ihf.type(HOST_FUNCTION_TYPE_INVOKE_CONTRACT);
    ihf.invokeContract() = parameters;
    SorobanResources resources;
    resources.footprint.readOnly = contractKeys;
    resources.instructions = 2'000'000;
    resources.readBytes = 2000;
    resources.writeBytes = 1000;
    resources.extendedMetaDataSizeBytes = 3000;

    auto tx = sorobanTransactionFrameFromOps(app->getNetworkID(), root, {op},
                                             {}, resources, 100'000, 1200);
    LedgerTxn ltx(app->getLedgerTxnRoot());
    TransactionMetaFrame txm(ltx.loadHeader().current().ledgerVersion);
    REQUIRE(tx->checkValid(*app, ltx, 0, 0, 0));
    REQUIRE(!tx->apply(*app, ltx, txm));
    ltx.commit();

    auto const& opEvents = txm.getXDR().v3().diagnosticEvents;
    REQUIRE(opEvents.size() == 2);

    auto const& call_ev = opEvents.at(0);
    REQUIRE(!call_ev.inSuccessfulContractCall);
    REQUIRE(call_ev.event.type == ContractEventType::DIAGNOSTIC);
    REQUIRE(call_ev.event.body.v0().data.type() == SCV_VEC);

    auto const& contract_ev = opEvents.at(1);
    REQUIRE(!contract_ev.inSuccessfulContractCall);
    REQUIRE(contract_ev.event.type == ContractEventType::CONTRACT);
    REQUIRE(contract_ev.event.body.v0().data.type() == SCV_VEC);
}

TEST_CASE("complex contract", "[tx][soroban]")
{
    auto complexTest = [&](bool enableDiagnostics) {
        VirtualClock clock;
        auto cfg = getTestConfig();
        cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS = enableDiagnostics;
        auto app = createTestApplication(clock, cfg);
        auto root = TestAccount::createRoot(*app);

        auto const complexWasm = rust_bridge::get_test_wasm_complex();

        auto contractKeys = deployContractWithSourceAccount(*app, complexWasm);
        auto const& contractID = contractKeys[0].contractData().contractID;

        auto scFunc = makeSymbol("go");

        Operation op;
        op.body.type(INVOKE_HOST_FUNCTION);
        auto& ihf = op.body.invokeHostFunctionOp().hostFunction;
        ihf.type(HOST_FUNCTION_TYPE_INVOKE_CONTRACT);
        ihf.invokeContract() = {makeContractAddress(contractID), scFunc};

        // Contract writes a single `data` CONTRACT_DATA entry.
        LedgerKey dataKey(LedgerEntryType::CONTRACT_DATA);
        dataKey.contractData().contractID = contractID;
        dataKey.contractData().key = makeSymbol("data");

        SorobanResources resources;
        resources.footprint.readOnly = contractKeys;
        resources.footprint.readWrite = {dataKey};
        resources.instructions = 2'000'000;
        resources.readBytes = 3000;
        resources.writeBytes = 1000;
        resources.extendedMetaDataSizeBytes = 3000;

        auto verifyDiagnosticEvents =
            [&](xdr::xvector<DiagnosticEvent> events) {
                REQUIRE(events.size() == 3);

                auto call_ev = events.at(0);
                REQUIRE(call_ev.event.type == ContractEventType::DIAGNOSTIC);
                REQUIRE(call_ev.event.body.v0().data.type() == SCV_VOID);

                auto contract_ev = events.at(1);
                REQUIRE(contract_ev.event.type == ContractEventType::CONTRACT);
                REQUIRE(contract_ev.event.body.v0().data.type() == SCV_BYTES);

                auto return_ev = events.at(2);
                REQUIRE(return_ev.event.type == ContractEventType::DIAGNOSTIC);
                REQUIRE(return_ev.event.body.v0().data.type() == SCV_VOID);
            };

        SECTION("single op")
        {
            auto tx = sorobanTransactionFrameFromOps(
                app->getNetworkID(), root, {op}, {}, resources, 100'000, 1200);
            LedgerTxn ltx(app->getLedgerTxnRoot());
            TransactionMetaFrame txm(ltx.loadHeader().current().ledgerVersion);
            REQUIRE(tx->checkValid(*app, ltx, 0, 0, 0));
            REQUIRE(tx->apply(*app, ltx, txm));
            ltx.commit();

            // Contract should have emitted a single event carrying a `Bytes`
            // value.
            REQUIRE(txm.getXDR().v3().events.size() == 1);
            REQUIRE(txm.getXDR().v3().events.at(0).type ==
                    ContractEventType::CONTRACT);
            REQUIRE(txm.getXDR().v3().events.at(0).body.v0().data.type() ==
                    SCV_BYTES);

            if (enableDiagnostics)
            {
                verifyDiagnosticEvents(txm.getXDR().v3().diagnosticEvents);
            }
            else
            {
                REQUIRE(txm.getXDR().v3().diagnosticEvents.size() == 0);
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

TEST_CASE("Stellar asset contract XLM transfer", "[tx][soroban]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    auto root = TestAccount::createRoot(*app);
    auto xlm = txtest::makeNativeAsset();

    // Create XLM contract
    HashIDPreimage preImage;
    preImage.type(ENVELOPE_TYPE_CONTRACT_ID);
    preImage.contractID().contractIDPreimage.type(
        CONTRACT_ID_PREIMAGE_FROM_ASSET);
    preImage.contractID().contractIDPreimage.fromAsset() = xlm;
    preImage.contractID().networkID = app->getNetworkID();
    auto contractID = xdrSha256(preImage);

    Operation createOp;
    createOp.body.type(INVOKE_HOST_FUNCTION);
    auto& createHF = createOp.body.invokeHostFunctionOp();
    createHF.hostFunction.type(HOST_FUNCTION_TYPE_CREATE_CONTRACT);
    auto& createContractArgs = createHF.hostFunction.createContract();

    SCContractExecutable exec;
    exec.type(SCCONTRACT_EXECUTABLE_TOKEN);
    createContractArgs.contractIDPreimage.type(CONTRACT_ID_PREIMAGE_FROM_ASSET);
    createContractArgs.contractIDPreimage.fromAsset() = xlm;
    createContractArgs.executable = exec;
    SorobanResources createResources;
    createResources.instructions = 200'000;
    createResources.readBytes = 1000;
    createResources.writeBytes = 1000;
    createResources.extendedMetaDataSizeBytes = 3000;
    {
        LedgerFootprint lfp1;
        auto key1 = LedgerKey(CONTRACT_DATA);
        key1.contractData().contractID = contractID;
        key1.contractData().key = makeSymbol(
            "METADATA"); // TODO:WHY DOES THIS PASS WITHOUT THE TYPE SPECIFIED

        auto key2 = LedgerKey(CONTRACT_DATA);
        key2.contractData().contractID = contractID;
        SCVec vec = {makeSymbol("AssetInfo")};
        SCVal vecKey(SCValType::SCV_VEC);
        vecKey.vec().activate() = vec;
        key2.contractData().key = vecKey;

        SCVal scContractSourceRefKey(
            SCValType::SCV_LEDGER_KEY_CONTRACT_EXECUTABLE);
        auto key3 = LedgerKey(CONTRACT_DATA);
        key3.contractData().contractID = contractID;
        key3.contractData().key = scContractSourceRefKey;

        lfp1.readWrite = {key1, key2, key3};
        createResources.footprint = lfp1;
    }

    {
        // submit operation
        auto tx = sorobanTransactionFrameFromOps(
            app->getNetworkID(), root, {createOp}, {}, createResources, 100'000,
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

    SCAddress toContract(SC_ADDRESS_TYPE_CONTRACT);
    toContract.contractId() = sha256("contract");
    SCVal to(SCV_ADDRESS);
    to.address() = toContract;

    auto fn = makeSymbol("transfer");
    Operation transfer;
    transfer.body.type(INVOKE_HOST_FUNCTION);
    auto& ihf = transfer.body.invokeHostFunctionOp().hostFunction;
    ihf.type(HOST_FUNCTION_TYPE_INVOKE_CONTRACT);
    ihf.invokeContract() = {makeContractAddress(contractID), fn, from, to,
                            makeI128(10)};

    // build auth
    SorobanAuthorizedInvocation ai;
    ai.function.type(SOROBAN_AUTHORIZED_FUNCTION_TYPE_CONTRACT_FN);
    SCAddress contractAddress(SC_ADDRESS_TYPE_CONTRACT);
    contractAddress.contractId() = contractID;
    ai.function.contractFn().contractAddress = contractAddress;
    ai.function.contractFn().functionName = fn.sym();
    ai.function.contractFn().args = {from, to, makeI128(10)};

    SorobanAuthorizationEntry a;
    a.credentials.type(SOROBAN_CREDENTIALS_SOURCE_ACCOUNT);
    a.rootInvocation = ai;
    transfer.body.invokeHostFunctionOp().auth = {a};

    SorobanResources resources;
    resources.instructions = 2'000'000;
    resources.readBytes = 2000;
    resources.writeBytes = 1072;
    resources.extendedMetaDataSizeBytes = 3000;
    {
        auto key1 = LedgerKey(CONTRACT_DATA);
        key1.contractData().contractID = contractID;
        key1.contractData().key = makeSymbol("METADATA");
        key1.contractData().type = MERGEABLE;
        key1.contractData().leType = DATA_ENTRY;

        auto key2 = LedgerKey(CONTRACT_DATA);
        key2.contractData().contractID = contractID;
        SCVec assetInfo = {makeSymbol("AssetInfo")};
        SCVal assetInfoSCVal(SCValType::SCV_VEC);
        assetInfoSCVal.vec().activate() = assetInfo;
        key2.contractData().key = assetInfoSCVal;
        key2.contractData().type = EXCLUSIVE;
        key2.contractData().leType = DATA_ENTRY;

        SCVal scContractSourceRefKey(
            SCValType::SCV_LEDGER_KEY_CONTRACT_EXECUTABLE);
        auto key3 = LedgerKey(CONTRACT_DATA);
        key3.contractData().contractID = contractID;
        key3.contractData().key = scContractSourceRefKey;
        key3.contractData().type = EXCLUSIVE;
        key3.contractData().leType = DATA_ENTRY;

        LedgerKey accountKey(ACCOUNT);
        accountKey.account().accountID = root.getPublicKey();

        // build balance key
        LedgerKey key4(ACCOUNT);
        key4.account().accountID = root.getPublicKey();

        auto key5 = LedgerKey(CONTRACT_DATA);
        key5.contractData().contractID = contractID;

        SCVec balance = {makeSymbol("Balance"), to};
        SCVal balanceKey(SCValType::SCV_VEC);
        balanceKey.vec().activate() = balance;
        key5.contractData().key = balanceKey;
        key5.contractData().type = MERGEABLE;
        key5.contractData().leType = DATA_ENTRY;

        SCNonceKey nonce;
        nonce.nonce_address = fromAccount;
        SCVal nonceKey(SCV_LEDGER_KEY_NONCE);
        nonceKey.nonce_key() = nonce;

        // build nonce key
        auto key6 = LedgerKey(CONTRACT_DATA);
        key6.contractData().contractID = contractID;
        key6.contractData().key = nonceKey;
        key6.contractData().type = EXCLUSIVE;
        key6.contractData().leType = DATA_ENTRY;

        resources.footprint.readWrite = {key1, key2, key3, key4, key5, key6};
    }

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

#endif
