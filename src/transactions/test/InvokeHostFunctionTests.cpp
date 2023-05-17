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

template <typename T>
SCVal
makeBinary(T begin, T end)
{
    SCVal val(SCValType::SCV_BYTES);
    val.bytes().assign(begin, end);
    return val;
}

template <typename T>
SCVal
makeWasmRefScContractCode(T const& hash)
{
    SCVal val(SCValType::SCV_CONTRACT_EXECUTABLE);
    val.exec().type(SCContractExecutableType::SCCONTRACT_EXECUTABLE_WASM_REF);

    std::copy(hash.begin(), hash.end(), val.exec().wasm_id().begin());
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

static void
submitTxToDeployContract(Application& app, Operation const& deployOp,
                         SorobanResources const& resources,
                         Hash const& expectedWasmHash,
                         xdr::opaque_vec<SCVAL_LIMIT> const& expectedWasm,
                         Hash const& contractID, SCVal const& sourceRefKey,
                         uint32_t fee, uint32_t refundableFee)
{
    // submit operation
    auto root = TestAccount::createRoot(app);
    auto tx =
        sorobanTransactionFrameFromOps(app.getNetworkID(), root, {deployOp}, {},
                                       resources, fee, refundableFee);
    LedgerTxn ltx(app.getLedgerTxnRoot());
    TransactionMetaFrame txm(ltx.loadHeader().current().ledgerVersion);
    REQUIRE(tx->checkValid(app, ltx, 0, 0, 0));
    REQUIRE(tx->apply(app, ltx, txm));
    ltx.commit();

    // verify contract code is correct
    {
        LedgerTxn ltx2(app.getLedgerTxnRoot());
        auto ltxe2 = loadContractCode(ltx2, expectedWasmHash);
        REQUIRE(ltxe2);

        auto const& body = ltxe2.current().data.contractCode().body;
        REQUIRE(body.leType() == DATA_ENTRY);
        REQUIRE(body.code() == expectedWasm);
    }
    // verify contract code reference is correct
    {
        LedgerTxn ltx2(app.getLedgerTxnRoot());
        auto ltxe2 = loadContractData(ltx2, contractID, sourceRefKey);
        REQUIRE(ltxe2);

        auto const& body = ltxe2.current().data.contractData().body;
        REQUIRE(body.leType() == DATA_ENTRY);
        REQUIRE(body.data().val == makeWasmRefScContractCode(expectedWasmHash));
    }
}

static FootprintEntry
fe(LedgerKey const& k)
{
    FootprintEntry e;
    e.key = k;
    return e;
}

static xdr::xvector<LedgerKey>
deployContractWithSourceAccount(Application& app, RustBuf const& contractWasm,
                                uint256 salt = sha256("salt"))
{
    auto root = TestAccount::createRoot(app);

    // Upload contract code
    Operation deployOp;
    deployOp.body.type(INVOKE_HOST_FUNCTION);
    deployOp.body.invokeHostFunctionOp().functions.resize(2);
    auto& uploadHF = deployOp.body.invokeHostFunctionOp().functions[0];
    uploadHF.args.type(HOST_FUNCTION_TYPE_UPLOAD_CONTRACT_WASM);
    auto& uploadContractWasmArgs = uploadHF.args.uploadContractWasm();
    uploadContractWasmArgs.code.assign(contractWasm.data.begin(),
                                       contractWasm.data.end());

    LedgerKey contractCodeLedgerKey;
    contractCodeLedgerKey.type(CONTRACT_CODE);
    contractCodeLedgerKey.contractCode().hash =
        xdrSha256(uploadContractWasmArgs);

    // Deploy the contract instance
    HashIDPreimage preImage;
    preImage.type(ENVELOPE_TYPE_CONTRACT_ID_FROM_SOURCE_ACCOUNT);
    preImage.sourceAccountContractID().sourceAccount = root.getPublicKey();
    preImage.sourceAccountContractID().salt = salt;
    preImage.sourceAccountContractID().networkID = app.getNetworkID();
    auto contractID = xdrSha256(preImage);
    auto& createHF = deployOp.body.invokeHostFunctionOp().functions[1];
    createHF.args.type(HOST_FUNCTION_TYPE_CREATE_CONTRACT);
    auto& createContractArgs = createHF.args.createContract();
    createContractArgs.contractID.type(CONTRACT_ID_FROM_SOURCE_ACCOUNT);
    createContractArgs.contractID.salt() = salt;

    createContractArgs.executable.type(SCCONTRACT_EXECUTABLE_WASM_REF);
    createContractArgs.executable.wasm_id() =
        contractCodeLedgerKey.contractCode().hash;

    SCVal scContractSourceRefKey(SCValType::SCV_LEDGER_KEY_CONTRACT_EXECUTABLE);

    LedgerKey contractSourceRefLedgerKey;
    contractSourceRefLedgerKey.type(CONTRACT_DATA);
    contractSourceRefLedgerKey.contractData().contractID = contractID;
    contractSourceRefLedgerKey.contractData().key = scContractSourceRefKey;

    SorobanResources resources;

    resources.footprint.readWrite = {fe(contractCodeLedgerKey),
                                     fe(contractSourceRefLedgerKey)};
    resources.instructions = 200'000;
    resources.readBytes = 1000;
    resources.writeBytes = 5000;
    resources.extendedMetaDataSizeBytes = 6000;

    submitTxToDeployContract(app, deployOp, resources,
                             contractCodeLedgerKey.contractCode().hash,
                             uploadContractWasmArgs.code, contractID,
                             scContractSourceRefKey, 100'000, 1200);

    return {contractSourceRefLedgerKey, contractCodeLedgerKey};
}

TEST_CASE("basic contract invocation", "[tx][soroban]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    auto root = TestAccount::createRoot(*app);
    int64_t initBalance = root.getBalance();

    auto const addI32Wasm = rust_bridge::get_test_wasm_add_i32();

    auto contractKeys = deployContractWithSourceAccount(*app, addI32Wasm);
    auto const& contractID = contractKeys[0].contractData().contractID;
    auto call = [&](SorobanResources const& resources, SCVec const& parameters,
                    bool success) {
        Operation op;
        op.body.type(INVOKE_HOST_FUNCTION);
        auto& ihf = op.body.invokeHostFunctionOp().functions.emplace_back();
        ihf.args.type(HOST_FUNCTION_TYPE_INVOKE_CONTRACT);
        ihf.args.invokeContract() = parameters;

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
            REQUIRE(tx->getFeeBid() == 62'973);
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
        xdr::xvector<SCVal, 100> resultVals;
        if (tx->getResult().result.code() == txSUCCESS &&
            !tx->getResult().result.results().empty())
        {
            auto const& ores = tx->getResult().result.results().at(0);
            if (ores.tr().type() == INVOKE_HOST_FUNCTION &&
                ores.tr().invokeHostFunctionResult().code() ==
                    INVOKE_HOST_FUNCTION_SUCCESS)
            {
                resultVals = txm.getXDR().v3().returnValues;

                InvokeHostFunctionSuccessPreImage success2;
                success2.returnValues = resultVals;
                success2.events = txm.getXDR().v3().events;

                REQUIRE(ores.tr().invokeHostFunctionResult().success() ==
                        xdrSha256(success2));
            }
        }
        return resultVals;
    };

    auto scContractID = makeBinary(contractID.begin(), contractID.end());
    auto scFunc = makeSymbol("add");
    auto sc7 = makeI32(7);
    auto sc16 = makeI32(16);
    SorobanResources resources;
    for (auto const& k : contractKeys)
    {
        resources.footprint.readOnly.push_back(fe(k));
    }
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

    auto checkContractData = [&](SCVal const& key, SCVal const* val) {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        auto ltxe = loadContractData(ltx, contractID, key);
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

    auto putWithFootprint = [&](std::string const& key, uint64_t val,
                                xdr::xvector<LedgerKey> const& readOnly,
                                xdr::xvector<LedgerKey> const& readWrite,
                                uint32_t writeBytes, bool expectSuccess) {
        auto keySymbol = makeSymbol(key);
        auto valU64 = makeU64(val);

        Operation op;
        op.body.type(INVOKE_HOST_FUNCTION);
        auto& ihf = op.body.invokeHostFunctionOp().functions.emplace_back();
        ihf.args.type(HOST_FUNCTION_TYPE_INVOKE_CONTRACT);
        ihf.args.invokeContract() = {
            makeBinary(contractID.begin(), contractID.end()), makeSymbol("put"),
            keySymbol, valU64};
        SorobanResources resources;
        for (auto const& k : readOnly)
        {
            resources.footprint.readOnly.push_back(fe(k));
        }

        for (auto const& k : readWrite)
        {
            resources.footprint.readWrite.push_back(fe(k));
        }

        resources.instructions = 2'000'000;
        resources.readBytes = 5000;
        resources.writeBytes = writeBytes;
        resources.extendedMetaDataSizeBytes = 3000;

        auto tx = sorobanTransactionFrameFromOps(
            app->getNetworkID(), root, {op}, {}, resources, 100'000, 1200);
        LedgerTxn ltx(app->getLedgerTxnRoot());
        TransactionMetaFrame txm(ltx.loadHeader().current().ledgerVersion);
        REQUIRE(tx->checkValid(*app, ltx, 0, 0, 0));
        if (expectSuccess)
        {
            REQUIRE(tx->apply(*app, ltx, txm));
            ltx.commit();
            checkContractData(keySymbol, &valU64);
        }
        else
        {
            REQUIRE(!tx->apply(*app, ltx, txm));
            ltx.commit();
        }
    };

    auto put = [&](std::string const& key, uint64_t val) {
        putWithFootprint(key, val, contractKeys,
                         {contractDataKey(contractID, makeSymbol(key))}, 1000,
                         true);
    };

    auto delWithFootprint =
        [&](std::string const& key, xdr::xvector<LedgerKey> const& readOnly,
            xdr::xvector<LedgerKey> const& readWrite, bool expectSuccess) {
            auto keySymbol = makeSymbol(key);

            Operation op;
            op.body.type(INVOKE_HOST_FUNCTION);
            auto& ihf = op.body.invokeHostFunctionOp().functions.emplace_back();
            ihf.args.type(HOST_FUNCTION_TYPE_INVOKE_CONTRACT);
            ihf.args.invokeContract() = {
                makeBinary(contractID.begin(), contractID.end()),
                makeSymbol("del"), keySymbol};
            SorobanResources resources;
            for (auto const& k : readOnly)
            {
                resources.footprint.readOnly.push_back(fe(k));
            }

            for (auto const& k : readWrite)
            {
                resources.footprint.readWrite.push_back(fe(k));
            }

            resources.instructions = 2'000'000;
            resources.readBytes = 5000;
            resources.writeBytes = 1000;
            resources.extendedMetaDataSizeBytes = 3000;

            auto tx = sorobanTransactionFrameFromOps(
                app->getNetworkID(), root, {op}, {}, resources, 100'000, 1200);
            LedgerTxn ltx(app->getLedgerTxnRoot());
            TransactionMetaFrame txm(ltx.loadHeader().current().ledgerVersion);
            REQUIRE(tx->checkValid(*app, ltx, 0, 0, 0));
            if (expectSuccess)
            {
                REQUIRE(tx->apply(*app, ltx, txm));
                ltx.commit();
                checkContractData(keySymbol, nullptr);
            }
            else
            {
                REQUIRE(!tx->apply(*app, ltx, txm));
                ltx.commit();
            }
        };

    auto del = [&](std::string const& key) {
        delWithFootprint(key, contractKeys,
                         {contractDataKey(contractID, makeSymbol(key))}, true);
    };

    SECTION("default limits")
    {
        put("key1", 0);
        put("key2", 21);

        // Failure: contract data isn't in footprint
        putWithFootprint("key1", 88, contractKeys, {}, 1000, false);
        delWithFootprint("key1", contractKeys, {}, false);

        // Failure: contract data is read only
        auto readOnlyFootprint = contractKeys;
        readOnlyFootprint.push_back(
            contractDataKey(contractID, makeSymbol("key2")));
        putWithFootprint("key2", 888888, readOnlyFootprint, {}, 1000, false);
        delWithFootprint("key2", readOnlyFootprint, {}, false);

        // Failure: insufficient write bytes
        putWithFootprint("key2", 88888, contractKeys,
                         {contractDataKey(contractID, makeSymbol("key2"))}, 1,
                         false);

        put("key1", 9);
        put("key2", UINT64_MAX);

        del("key1");
        del("key2");
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
                         {contractDataKey(contractID, makeSymbol("key2"))},
                         1000, false);
    }
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
    auto scContractID = makeBinary(contractID.begin(), contractID.end());

    auto sc1 = makeI32(7);
    auto scMax = makeI32(INT32_MAX);
    SCVec parameters = {scContractID, makeSymbol("add"), sc1, scMax};

    Operation op;
    op.body.type(INVOKE_HOST_FUNCTION);
    auto& ihf = op.body.invokeHostFunctionOp().functions.emplace_back();
    ihf.args.type(HOST_FUNCTION_TYPE_INVOKE_CONTRACT);
    ihf.args.invokeContract() = parameters;
    SorobanResources resources;
    for (auto const& k : contractKeys)
    {
        resources.footprint.readOnly.push_back(fe(k));
    }

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
    REQUIRE(opEvents.size() == 3);

    auto const& call_ev = opEvents.at(0);
    REQUIRE(!call_ev.inSuccessfulContractCall);
    REQUIRE(call_ev.event.type == ContractEventType::DIAGNOSTIC);
    REQUIRE(call_ev.event.body.v0().data.type() == SCV_VEC);

    auto const& contract_ev = opEvents.at(1);
    REQUIRE(!contract_ev.inSuccessfulContractCall);
    REQUIRE(contract_ev.event.type == ContractEventType::CONTRACT);
    REQUIRE(contract_ev.event.body.v0().data.type() == SCV_VEC);

    auto const& error_er = opEvents.at(2);
    REQUIRE(!error_er.inSuccessfulContractCall);
    REQUIRE(error_er.event.type == ContractEventType::DIAGNOSTIC);
    REQUIRE(error_er.event.body.v0().data.type() == SCV_VEC);
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

        auto scContractID = makeBinary(contractID.begin(), contractID.end());
        auto scFunc = makeSymbol("go");

        Operation op;
        op.body.type(INVOKE_HOST_FUNCTION);
        auto& ihf = op.body.invokeHostFunctionOp().functions.emplace_back();
        ihf.args.type(HOST_FUNCTION_TYPE_INVOKE_CONTRACT);
        ihf.args.invokeContract() = {scContractID, scFunc};

        // Contract writes a single `data` CONTRACT_DATA entry.
        LedgerKey dataKey(LedgerEntryType::CONTRACT_DATA);
        dataKey.contractData().contractID = contractID;
        dataKey.contractData().key = makeSymbol("data");

        SorobanResources resources;

        for (auto const& k : contractKeys)
        {
            resources.footprint.readOnly.push_back(fe(k));
        }
        resources.footprint.readWrite = {fe(dataKey)};
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
    preImage.type(ENVELOPE_TYPE_CONTRACT_ID_FROM_ASSET);
    preImage.fromAsset().asset = xlm;
    preImage.fromAsset().networkID = app->getNetworkID();
    auto contractID = xdrSha256(preImage);

    Operation op;
    op.body.type(INVOKE_HOST_FUNCTION);
    op.body.invokeHostFunctionOp().functions.resize(2);
    auto& createHF = op.body.invokeHostFunctionOp().functions[0];
    createHF.args.type(HOST_FUNCTION_TYPE_CREATE_CONTRACT);
    auto& createContractArgs = createHF.args.createContract();

    SCContractExecutable exec;
    exec.type(SCCONTRACT_EXECUTABLE_TOKEN);
    createContractArgs.contractID.type(CONTRACT_ID_FROM_ASSET);
    createContractArgs.contractID.asset() = xlm;
    createContractArgs.executable = exec;

    // transfer 10 XLM from root to contractID
    auto scContractID = makeBinary(contractID.begin(), contractID.end());

    SCAddress fromAccount(SC_ADDRESS_TYPE_ACCOUNT);
    fromAccount.accountId() = root.getPublicKey();
    SCVal from(SCV_ADDRESS);
    from.address() = fromAccount;

    SCAddress toContract(SC_ADDRESS_TYPE_CONTRACT);
    toContract.contractId() = sha256("contract");
    SCVal to(SCV_ADDRESS);
    to.address() = toContract;

    auto fn = makeSymbol("transfer");
    auto& ihf = op.body.invokeHostFunctionOp().functions[1];
    ihf.args.type(HOST_FUNCTION_TYPE_INVOKE_CONTRACT);
    ihf.args.invokeContract() = {scContractID, fn, from, to, makeI128(10)};

    // build auth
    AuthorizedInvocation ai;
    ai.contractID = contractID;
    ai.functionName = fn.sym();
    ai.args = {from, to, makeI128(10)};

    ContractAuth a;
    a.rootInvocation = ai;
    ihf.auth = {a};

    SorobanResources resources;
    resources.instructions = 2'000'000;
    resources.readBytes = 2000;
    resources.writeBytes = 1000;
    resources.extendedMetaDataSizeBytes = 3000;
    {
        auto key1 = LedgerKey(CONTRACT_DATA);
        key1.contractData().contractID = contractID;
        key1.contractData().key = makeSymbol("METADATA");

        auto key2 = LedgerKey(CONTRACT_DATA);
        key2.contractData().contractID = contractID;
        SCVec assetInfo = {makeSymbol("AssetInfo")};
        SCVal assetInfoSCVal(SCValType::SCV_VEC);
        assetInfoSCVal.vec().activate() = assetInfo;
        key2.contractData().key = assetInfoSCVal;

        SCVal scContractSourceRefKey(
            SCValType::SCV_LEDGER_KEY_CONTRACT_EXECUTABLE);
        auto key3 = LedgerKey(CONTRACT_DATA);
        key3.contractData().contractID = contractID;
        key3.contractData().key = scContractSourceRefKey;

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

        SCNonceKey nonce;
        nonce.nonce_address = fromAccount;
        SCVal nonceKey(SCV_LEDGER_KEY_NONCE);
        nonceKey.nonce_key() = nonce;

        // build nonce key
        auto key6 = LedgerKey(CONTRACT_DATA);
        key6.contractData().contractID = contractID;
        key6.contractData().key = nonceKey;

        resources.footprint.readWrite = {fe(key1), fe(key2), fe(key3),
                                         fe(key4), fe(key5), fe(key6)};
    }

    {
        // submit operation
        auto tx = sorobanTransactionFrameFromOps(
            app->getNetworkID(), root, {op}, {}, resources, 250'000, 1200);

        LedgerTxn ltx(app->getLedgerTxnRoot());
        TransactionMetaFrame txm(ltx.loadHeader().current().ledgerVersion);
        REQUIRE(tx->checkValid(*app, ltx, 0, 0, 0));
        REQUIRE(tx->apply(*app, ltx, txm));
        ltx.commit();
    }
}

#endif
