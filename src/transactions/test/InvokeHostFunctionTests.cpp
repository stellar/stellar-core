// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "xdr/Stellar-transaction.h"
#include <iterator>
#include <stdexcept>
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION

#include "crypto/SecretKey.h"
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
    SCVal val(SCValType::SCV_OBJECT);
    val.obj().activate().type(SCO_BYTES);
    val.obj()->bin().assign(begin, end);
    return val;
}

template <typename T>
SCVal
makeWasmRefScContractCode(T const& hash)
{
    SCVal val(SCValType::SCV_OBJECT);
    val.obj().activate().type(stellar::SCO_CONTRACT_CODE);
    val.obj()->contractCode().type(stellar::SCCONTRACT_CODE_WASM_REF);
    std::copy(hash.begin(), hash.end(),
              val.obj()->contractCode().wasm_id().begin());
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
makeSymbol(std::string const& str)
{
    SCVal val(SCV_SYMBOL);
    val.sym().assign(str.begin(), str.end());
    return val;
}

static void
submitOpToInstallContractCode(Application& app, Operation const& op,
                              Hash const& expectedWasmHash,
                              xdr::opaque_vec<SCVAL_LIMIT> const& expectedWasm)
{
    // submit operation
    auto root = TestAccount::createRoot(app);
    auto tx = transactionFrameFromOps(app.getNetworkID(), root, {op}, {});
    LedgerTxn ltx(app.getLedgerTxnRoot());
    TransactionMetaFrame txm(ltx.loadHeader().current().ledgerVersion);
    REQUIRE(tx->checkValid(ltx, 0, 0, 0));
    REQUIRE(tx->apply(app, ltx, txm));
    ltx.commit();

    // verify contract code is correct
    {
        LedgerTxn ltx2(app.getLedgerTxnRoot());
        auto ltxe2 = loadContractCode(ltx2, expectedWasmHash);
        REQUIRE(ltxe2);
        REQUIRE(ltxe2.current().data.contractCode().code == expectedWasm);
    }
}

static void
submitOpToCreateContract(Application& app, Operation const& op,
                         Hash const& contractID, Hash const& expectedWasmHash,
                         SCVal const& sourceRefKey)
{
    // submit operation
    auto root = TestAccount::createRoot(app);
    auto tx = transactionFrameFromOps(app.getNetworkID(), root, {op}, {});
    LedgerTxn ltx(app.getLedgerTxnRoot());
    TransactionMetaFrame txm(ltx.loadHeader().current().ledgerVersion);
    REQUIRE(tx->checkValid(ltx, 0, 0, 0));
    REQUIRE(tx->apply(app, ltx, txm));
    ltx.commit();

    // verify contract code reference is correct
    {
        LedgerTxn ltx2(app.getLedgerTxnRoot());
        auto ltxe2 = loadContractData(ltx2, contractID, sourceRefKey);
        REQUIRE(ltxe2);
        REQUIRE(ltxe2.current().data.contractData().val ==
                makeWasmRefScContractCode(expectedWasmHash));
    }
}

static xdr::xvector<LedgerKey>
deployContractWithSourceAccount(Application& app, RustBuf const& contractWasm,
                                uint256 salt = sha256("salt"))
{
    auto root = TestAccount::createRoot(app);

    // Install contract code
    Operation installOp;
    installOp.body.type(INVOKE_HOST_FUNCTION);
    auto& installHF = installOp.body.invokeHostFunctionOp();
    installHF.function.type(HOST_FUNCTION_TYPE_INSTALL_CONTRACT_CODE);
    auto& installContractArgs = installHF.function.installContractCodeArgs();
    installContractArgs.code.assign(contractWasm.data.begin(),
                                    contractWasm.data.end());

    LedgerKey contractCodeLedgerKey;
    contractCodeLedgerKey.type(CONTRACT_CODE);
    contractCodeLedgerKey.contractCode().hash = xdrSha256(installContractArgs);
    installHF.footprint.readWrite = {contractCodeLedgerKey};
    submitOpToInstallContractCode(app, installOp,
                                  contractCodeLedgerKey.contractCode().hash,
                                  installContractArgs.code);

    // Deploy the contract instance
    HashIDPreimage preImage;
    preImage.type(ENVELOPE_TYPE_CONTRACT_ID_FROM_SOURCE_ACCOUNT);
    preImage.sourceAccountContractID().sourceAccount = root.getPublicKey();
    preImage.sourceAccountContractID().salt = salt;
    preImage.sourceAccountContractID().networkID = app.getNetworkID();
    auto contractID = xdrSha256(preImage);
    Operation createOp;
    createOp.body.type(INVOKE_HOST_FUNCTION);
    auto& createHF = createOp.body.invokeHostFunctionOp();
    createHF.function.type(HOST_FUNCTION_TYPE_CREATE_CONTRACT);
    auto& createContractArgs = createHF.function.createContractArgs();
    createContractArgs.contractID.type(CONTRACT_ID_FROM_SOURCE_ACCOUNT);
    createContractArgs.contractID.salt() = salt;

    createContractArgs.source.type(SCCONTRACT_CODE_WASM_REF);
    createContractArgs.source.wasm_id() =
        contractCodeLedgerKey.contractCode().hash;

    SCVal scContractSourceRefKey(SCValType::SCV_STATIC);
    scContractSourceRefKey.ic() = SCStatic::SCS_LEDGER_KEY_CONTRACT_CODE;

    LedgerKey contractSourceRefLedgerKey;
    contractSourceRefLedgerKey.type(CONTRACT_DATA);
    contractSourceRefLedgerKey.contractData().contractID = contractID;
    contractSourceRefLedgerKey.contractData().key = scContractSourceRefKey;

    createHF.footprint.readOnly = {contractCodeLedgerKey};
    createHF.footprint.readWrite = {contractSourceRefLedgerKey};

    submitOpToCreateContract(app, createOp, contractID,
                             contractCodeLedgerKey.contractCode().hash,
                             scContractSourceRefKey);
    return {contractSourceRefLedgerKey, contractCodeLedgerKey};
}

TEST_CASE("invoke host function", "[tx][contract]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    auto root = TestAccount::createRoot(*app);

    auto const addI32Wasm = rust_bridge::get_test_wasm_add_i32();
    auto const contractDataWasm = rust_bridge::get_test_wasm_contract_data();

    {
        auto addI32 = [&]() {
            auto contractKeys =
                deployContractWithSourceAccount(*app, addI32Wasm);
            auto const& contractID = contractKeys[0].contractData().contractID;

            auto call = [&](SCVec const& parameters, bool success) {
                Operation op;
                op.body.type(INVOKE_HOST_FUNCTION);
                auto& ihf = op.body.invokeHostFunctionOp();
                ihf.function.type(HOST_FUNCTION_TYPE_INVOKE_CONTRACT);
                ihf.function.invokeArgs() = parameters;
                ihf.footprint.readOnly = contractKeys;

                auto tx = transactionFrameFromOps(app->getNetworkID(), root,
                                                  {op}, {});
                LedgerTxn ltx(app->getLedgerTxnRoot());
                TransactionMetaFrame txm(
                    ltx.loadHeader().current().ledgerVersion);
                REQUIRE(tx->checkValid(ltx, 0, 0, 0));
                if (success)
                {
                    REQUIRE(tx->apply(*app, ltx, txm));
                }
                else
                {
                    REQUIRE(!tx->apply(*app, ltx, txm));
                }
                ltx.commit();
                SCVal resultVal;
                resultVal.type(stellar::SCV_STATUS);
                resultVal.status().type(SCStatusType::SST_UNKNOWN_ERROR);
                if (tx->getResult().result.code() == txSUCCESS &&
                    !tx->getResult().result.results().empty())
                {
                    auto const& ores = tx->getResult().result.results().at(0);
                    if (ores.tr().type() == INVOKE_HOST_FUNCTION &&
                        ores.tr().invokeHostFunctionResult().code() ==
                            INVOKE_HOST_FUNCTION_SUCCESS)
                    {
                        resultVal =
                            ores.tr().invokeHostFunctionResult().success();
                    }
                }
                return resultVal;
            };

            auto scContractID =
                makeBinary(contractID.begin(), contractID.end());
            auto scFunc = makeSymbol("add");
            auto sc7 = makeI32(7);
            auto sc16 = makeI32(16);

            // Too few parameters for call
            call({}, false);
            call({scContractID}, false);

            // To few parameters for "add"
            call({scContractID, scFunc}, false);
            call({scContractID, scFunc, sc7}, false);

            // Correct function call
            call({scContractID, scFunc, sc7, sc16}, true);
            REQUIRE(app->getMetrics()
                        .NewTimer({"host-fn", "invoke-contract", "exec"})
                        .count() != 0);
            REQUIRE(
                app->getMetrics()
                    .NewMeter({"host-fn", "invoke-contract", "success"}, "call")
                    .count() != 0);

            // Too many parameters for "add"
            call({scContractID, scFunc, sc7, sc16, makeI32(0)}, false);
        };
        SECTION("create with source -  add i32")
        {
            addI32();
        }
    }

    SECTION("contract data")
    {
        auto contractKeys =
            deployContractWithSourceAccount(*app, contractDataWasm);
        auto const& contractID = contractKeys[0].contractData().contractID;

        auto checkContractData = [&](SCVal const& key, SCVal const* val) {
            LedgerTxn ltx(app->getLedgerTxnRoot());
            auto ltxe = loadContractData(ltx, contractID, key);
            if (val)
            {
                REQUIRE(ltxe);
                REQUIRE(ltxe.current().data.contractData().val == *val);
            }
            else
            {
                REQUIRE(!ltxe);
            }
        };

        auto putWithFootprint = [&](std::string const& key,
                                    std::string const& val,
                                    xdr::xvector<LedgerKey> const& readOnly,
                                    xdr::xvector<LedgerKey> const& readWrite,
                                    bool success) {
            auto keySymbol = makeSymbol(key);
            auto valSymbol = makeSymbol(val);

            Operation op;
            op.body.type(INVOKE_HOST_FUNCTION);
            auto& ihf = op.body.invokeHostFunctionOp();
            ihf.function.type(HOST_FUNCTION_TYPE_INVOKE_CONTRACT);
            ihf.function.invokeArgs().emplace_back(
                makeBinary(contractID.begin(), contractID.end()));
            ihf.function.invokeArgs().emplace_back(makeSymbol("put"));
            ihf.function.invokeArgs().emplace_back(keySymbol);
            ihf.function.invokeArgs().emplace_back(valSymbol);
            ihf.footprint.readOnly = readOnly;
            ihf.footprint.readWrite = readWrite;

            auto tx =
                transactionFrameFromOps(app->getNetworkID(), root, {op}, {});
            LedgerTxn ltx(app->getLedgerTxnRoot());
            TransactionMetaFrame txm(ltx.loadHeader().current().ledgerVersion);
            REQUIRE(tx->checkValid(ltx, 0, 0, 0));
            if (success)
            {
                REQUIRE(tx->apply(*app, ltx, txm));
                ltx.commit();
                checkContractData(keySymbol, &valSymbol);
            }
            else
            {
                REQUIRE(!tx->apply(*app, ltx, txm));
                ltx.commit();
            }
        };

        auto put = [&](std::string const& key, std::string const& val) {
            putWithFootprint(key, val, contractKeys,
                             {contractDataKey(contractID, makeSymbol(key))},
                             true);
        };

        auto delWithFootprint = [&](std::string const& key,
                                    xdr::xvector<LedgerKey> const& readOnly,
                                    xdr::xvector<LedgerKey> const& readWrite,
                                    bool success) {
            auto keySymbol = makeSymbol(key);

            Operation op;
            op.body.type(INVOKE_HOST_FUNCTION);
            auto& ihf = op.body.invokeHostFunctionOp();
            ihf.function.type(HOST_FUNCTION_TYPE_INVOKE_CONTRACT);
            ihf.function.invokeArgs().emplace_back(
                makeBinary(contractID.begin(), contractID.end()));
            ihf.function.invokeArgs().emplace_back(makeSymbol("del"));
            ihf.function.invokeArgs().emplace_back(keySymbol);
            ihf.footprint.readOnly = readOnly;
            ihf.footprint.readWrite = readWrite;

            auto tx =
                transactionFrameFromOps(app->getNetworkID(), root, {op}, {});
            LedgerTxn ltx(app->getLedgerTxnRoot());
            TransactionMetaFrame txm(ltx.loadHeader().current().ledgerVersion);
            REQUIRE(tx->checkValid(ltx, 0, 0, 0));
            if (success)
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
                             {contractDataKey(contractID, makeSymbol(key))},
                             true);
        };

        put("key1", "val1a");
        put("key2", "val2a");

        // Failure: contract data isn't in footprint
        putWithFootprint("key1", "val1b", contractKeys, {}, false);
        delWithFootprint("key1", contractKeys, {}, false);

        // Failure: contract data is read only
        auto readOnlyFootprint = contractKeys;
        readOnlyFootprint.push_back(
            contractDataKey(contractID, makeSymbol("key2")));
        putWithFootprint("key2", "val2b", readOnlyFootprint, {}, false);
        delWithFootprint("key2", readOnlyFootprint, {}, false);

        put("key1", "val1c");
        put("key2", "val2c");

        del("key1");
        del("key2");
    }
}

TEST_CASE("complex contract with preflight", "[tx][contract]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    auto root = TestAccount::createRoot(*app);

    auto const complexWasm = rust_bridge::get_test_wasm_complex();

    auto contractKeys = deployContractWithSourceAccount(*app, complexWasm);
    auto const& contractID = contractKeys[0].contractData().contractID;

    auto scContractID = makeBinary(contractID.begin(), contractID.end());
    auto scFunc = makeSymbol("go");

    Operation op;
    op.body.type(INVOKE_HOST_FUNCTION);
    auto& ihf = op.body.invokeHostFunctionOp();
    ihf.function.type(HOST_FUNCTION_TYPE_INVOKE_CONTRACT);
    ihf.function.invokeArgs() = {scContractID, scFunc};

    AccountID source;
    auto res = InvokeHostFunctionOpFrame::preflight(*app, ihf, source);

    // Contract writes a single `data` CONTRACT_DATA entry.
    LedgerKey dataKey(LedgerEntryType::CONTRACT_DATA);
    dataKey.contractData().contractID = contractID;
    dataKey.contractData().key = makeSymbol("data");

    ihf.footprint.readOnly = contractKeys;
    ihf.footprint.readWrite.emplace_back(dataKey);

    auto tx = transactionFrameFromOps(app->getNetworkID(), root, {op}, {});
    LedgerTxn ltx(app->getLedgerTxnRoot());
    TransactionMetaFrame txm(ltx.loadHeader().current().ledgerVersion);
    REQUIRE(tx->checkValid(ltx, 0, 0, 0));
    REQUIRE(tx->apply(*app, ltx, txm));
    ltx.commit();
    txm.finalizeHashes();

    // Contract should have emitted a single event carrying a `Bytes` value.
    REQUIRE(txm.getXDR().v3().events.size() == 1);
    REQUIRE(txm.getXDR().v3().events.at(0).events.size() == 1);
    REQUIRE(txm.getXDR().v3().events.at(0).events.at(0).type ==
            ContractEventType::CONTRACT);
    REQUIRE(txm.getXDR()
                .v3()
                .events.at(0)
                .events.at(0)
                .body.v0()
                .data.obj()
                ->type() == SCO_BYTES);
}

#endif
