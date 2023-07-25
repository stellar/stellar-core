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
    auto const& contractID = contractKeys[0].contractData().contract;
    auto call = [&](SorobanResources const& resources, SCAddress const& address,
                    SCSymbol const& functionName,
                    std::vector<SCVal> const& args, bool success) {
        Operation op;
        op.body.type(INVOKE_HOST_FUNCTION);
        auto& ihf = op.body.invokeHostFunctionOp().hostFunction;
        ihf.type(HOST_FUNCTION_TYPE_INVOKE_CONTRACT);
        ihf.invokeContract().contractAddress = address;
        ihf.invokeContract().functionName = functionName;
        ihf.invokeContract().args.assign(args.begin(), args.end());

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
            REQUIRE(tx->getInclusionFee() == 65'117);
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
                        1182);
            }
            // The account should receive a refund for unspent refundable fee.
            REQUIRE(root.getBalance() - balanceAfterFeeCharged == 1182);
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
                        1200);
            }
            // The account should receive a full refund for metadata
            // in case of tx failure.
            REQUIRE(root.getBalance() - balanceAfterFeeCharged == 1200);
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
    resources.writeBytes = 1000;
    resources.extendedMetaDataSizeBytes = 3000;

    SECTION("correct invocation")
    {
        call(resources, contractID, scFunc, {sc7, sc16}, true);
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
            call(resources, address, scFunc, {sc7, sc16}, false);
        }
        SECTION("account address")
        {
            SCAddress address(SC_ADDRESS_TYPE_ACCOUNT);
            address.accountId() = root.getPublicKey();
            call(resources, address, scFunc, {sc7, sc16}, false);
        }
        SECTION("too few parameters")
        {
            call(resources, contractID, scFunc, {sc7}, false);
        }
        SECTION("too many parameters")
        {
            // Too many parameters
            call(resources, contractID, scFunc, {sc7, sc16, makeI32(0)}, false);
        }
    }

    SECTION("insufficient instructions")
    {
        resources.instructions = 10000;
        call(resources, contractID, scFunc, {sc7, sc16}, false);
    }
    SECTION("insufficient read bytes")
    {
        resources.readBytes = 100;
        call(resources, contractID, scFunc, {sc7, sc16}, false);
    }
}

TEST_CASE("contract storage", "[tx][soroban]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    auto root = TestAccount::createRoot(*app);

    {
        // Autobump is disabled by default, so so enable it here for testing.
        LedgerTxn ltx(app->getLedgerTxnRoot());
        auto networkConfig =
            app->getLedgerManager().getSorobanNetworkConfig(ltx);
        auto stateExpirationSettings = networkConfig.stateExpirationSettings();
        stateExpirationSettings.autoBumpLedgers = 10;
        networkConfig.stateExpirationSettings() = stateExpirationSettings;
        app->getLedgerManager().setSorobanNetworkConfig(networkConfig);
    }

    auto const contractDataWasm = rust_bridge::get_test_wasm_contract_data();

    auto contractKeys = deployContractWithSourceAccount(*app, contractDataWasm);
    auto const& contractID = contractKeys[0].contractData().contract;
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

    auto getContractDataExpiration = [&](std::string const& key,
                                         ContractDataDurability type,
                                         uint32_t flags = 0) {
        auto keySymbol = makeSymbolSCVal(key);
        LedgerTxn ltx(app->getLedgerTxnRoot());
        auto ltxe = loadContractData(ltx, contractID, keySymbol, type);
        REQUIRE(ltxe);
        REQUIRE(ltxe.current().data.contractData().body.data().flags == flags);
        return getExpirationLedger(ltxe.current());
    };

    auto isEntryLive = [&](std::string const& key, ContractDataDurability type,
                           uint32_t ledgerSeq) {
        auto keySymbol = makeSymbolSCVal(key);
        LedgerTxn ltx(app->getLedgerTxnRoot());
        auto ltxe = loadContractData(ltx, contractID, keySymbol, type);
        REQUIRE(ltxe);
        return isLive(ltxe.current(), ledgerSeq);
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
        resources.extendedMetaDataSizeBytes = 3000;

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
        bumpResources.readBytes = 1000;
        bumpResources.writeBytes = 0;
        bumpResources.extendedMetaDataSizeBytes = 1000;

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
        bumpResources.readBytes = 1000;
        bumpResources.writeBytes = 1000;
        bumpResources.extendedMetaDataSizeBytes = 1000;

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

    SECTION("default limits")
    {
        put("key1", 0, ContractDataDurability::PERSISTENT);
        put("key2", 21, ContractDataDurability::PERSISTENT);

        // Failure: contract data isn't in footprint
        putWithFootprint("key1", 88, contractKeys, {}, 1000, false,
                         ContractDataDurability::PERSISTENT);
        delWithFootprint("key1", contractKeys, {}, false,
                         ContractDataDurability::PERSISTENT);

        // Failure: contract data is read only
        auto readOnlyFootprint = contractKeys;
        readOnlyFootprint.push_back(
            contractDataKey(contractID, makeSymbolSCVal("key2"),
                            ContractDataDurability::PERSISTENT, DATA_ENTRY));
        putWithFootprint("key2", 888888, readOnlyFootprint, {}, 1000, false,
                         ContractDataDurability::PERSISTENT);
        delWithFootprint("key2", readOnlyFootprint, {}, false,
                         ContractDataDurability::PERSISTENT);

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

    SorobanNetworkConfig refConfig;
    uint32_t ledgerSeq;
    {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        refConfig = app->getLedgerManager().getSorobanNetworkConfig(ltx);
        ledgerSeq = ltx.loadHeader().current().ledgerSeq;
    }
    SECTION("failure: entry exceeds max size")
    {
        refConfig.maxContractDataKeySizeBytes() = 300;
        refConfig.maxContractDataEntrySizeBytes() = 1;
        app->getLedgerManager().setSorobanNetworkConfig(refConfig);
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
        uint64_t recreatableVal = 1;
        uint64_t temporaryVal = 2;
        put("key", uniqueVal, ContractDataDurability::PERSISTENT);
        put("key", temporaryVal, TEMPORARY);
        auto uniqueScVal = makeU64(uniqueVal);
        auto recreatableScVal = makeU64(recreatableVal);
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

    auto const& stateExpirationSettings = refConfig.stateExpirationSettings();
    auto autoBump = stateExpirationSettings.autoBumpLedgers;

    SECTION("Enforce rent minimums and expiration")
    {
        put("unique", 0, ContractDataDurability::PERSISTENT);
        put("temp", 0, TEMPORARY);

        auto expectedTempExpiration =
            stateExpirationSettings.minTempEntryExpiration + ledgerSeq - 1;
        auto expectedPersistentExpiration =
            stateExpirationSettings.minPersistentEntryExpiration + ledgerSeq -
            1;

        // Check for expected minimum lifetime values
        REQUIRE(getContractDataExpiration("unique",
                                          ContractDataDurability::PERSISTENT) ==
                expectedPersistentExpiration);
        REQUIRE(getContractDataExpiration("temp", TEMPORARY) ==
                expectedTempExpiration);

        // Close ledgers until temp entry expires
        uint32 ledger = app->getLedgerManager().getLastClosedLedgerNum();
        for (; ledger <= expectedTempExpiration + 1; ++ledger)
        {
            closeLedgerOn(*app, ledger, 2, 1, 2016);
        }

        // Check that temp entry is not live
        REQUIRE(app->getLedgerManager().getLastClosedLedgerNum() ==
                expectedTempExpiration + 1);
        REQUIRE(!isEntryLive("temp", TEMPORARY,
                             app->getLedgerManager().getLastClosedLedgerNum()));
        REQUIRE(isEntryLive("unique", ContractDataDurability::PERSISTENT,
                            app->getLedgerManager().getLastClosedLedgerNum()));

        // Check that we can recreate an expired TEMPORARY entry
        putWithFootprint("temp", 0, contractKeys,
                         {contractDataKey(contractID, makeSymbolSCVal("temp"),
                                          TEMPORARY, DATA_ENTRY)},
                         1000, /*expectSuccess*/ true,
                         ContractDataDurability::TEMPORARY);

        // Close ledgers until PERSISTENT entry expires
        for (; ledger <= expectedPersistentExpiration + 1; ++ledger)
        {
            closeLedgerOn(*app, ledger, 2, 1, 2016);
        }

        REQUIRE(app->getLedgerManager().getLastClosedLedgerNum() ==
                expectedPersistentExpiration + 1);
        REQUIRE(!isEntryLive("unique", ContractDataDurability::PERSISTENT,
                             app->getLedgerManager().getLastClosedLedgerNum()));

        // Check that we can't recreate expired PERSISTENT
        putWithFootprint(
            "unique", 0, contractKeys,
            {contractDataKey(contractID, makeSymbolSCVal("unique"),
                             ContractDataDurability::PERSISTENT, DATA_ENTRY)},
            1000, /*expectSuccess*/ false, ContractDataDurability::PERSISTENT);
    }

    SECTION("autobump")
    {
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

        REQUIRE(getContractDataExpiration("rw",
                                          ContractDataDurability::PERSISTENT) ==
                expectedInitialExpiration + autoBump);
        REQUIRE(getContractDataExpiration("ro",
                                          ContractDataDurability::PERSISTENT) ==
                expectedInitialExpiration + autoBump);

        // Contract instance and Wasm should have minimum life and 3 invocations
        // worth of autobumps
        LedgerTxn ltx(app->getLedgerTxnRoot());
        for (auto const& key : contractKeys)
        {
            uint32_t mult = key.type() == CONTRACT_CODE ? 4 : 3;
            auto ltxe = ltx.loadWithoutRecord(key);
            REQUIRE(ltxe);
            REQUIRE(getExpirationLedger(ltxe.current()) ==
                    expectedInitialExpiration + (autoBump * mult));
        }
    }

    SECTION("manual bump")
    {
        put("key", 0, ContractDataDurability::PERSISTENT);
        bump("key", ContractDataDurability::PERSISTENT, 10'000);
        REQUIRE(getContractDataExpiration("key",
                                          ContractDataDurability::PERSISTENT) ==
                ledgerSeq + 10'000 - 1);

        // Expiration already above 5'000, should be a nop (other than autobump)
        bump("key", ContractDataDurability::PERSISTENT, 5'000);
        REQUIRE(getContractDataExpiration("key",
                                          ContractDataDurability::PERSISTENT) ==
                ledgerSeq + 10'000 - 1 + autoBump);

        put("key2", 0, ContractDataDurability::PERSISTENT);
        bump("key2", ContractDataDurability::PERSISTENT, 5'000);
        REQUIRE(getContractDataExpiration("key2",
                                          ContractDataDurability::PERSISTENT) ==
                ledgerSeq + 5'000 - 1);

        put("key3", 0, ContractDataDurability::PERSISTENT);
        bump("key3", ContractDataDurability::PERSISTENT, 50'000);
        REQUIRE(getContractDataExpiration("key3",
                                          ContractDataDurability::PERSISTENT) ==
                ledgerSeq + 50'000 - 1);

        // Bump to live 10100 ledger from now
        bumpOp(
            10100,
            {contractDataKey(contractID, makeSymbolSCVal("key"),
                             ContractDataDurability::PERSISTENT, DATA_ENTRY),
             contractDataKey(contractID, makeSymbolSCVal("key2"),
                             ContractDataDurability::PERSISTENT, DATA_ENTRY),
             contractDataKey(contractID, makeSymbolSCVal("key3"),
                             ContractDataDurability::PERSISTENT, DATA_ENTRY)},
            178);

        REQUIRE(getContractDataExpiration("key",
                                          ContractDataDurability::PERSISTENT) ==
                ledgerSeq + 10'100);
        REQUIRE(getContractDataExpiration("key2",
                                          ContractDataDurability::PERSISTENT) ==
                ledgerSeq + 10'100);

        // No change for key3 since expiration is already past 10100 ledgers
        // from now
        REQUIRE(getContractDataExpiration("key3",
                                          ContractDataDurability::PERSISTENT) ==
                ledgerSeq + 50'000 - 1);
    }
    SECTION("restore expired entry")
    {
        auto minBump =
            InitialSorobanNetworkConfig::MINIMUM_PERSISTENT_ENTRY_LIFETIME;
        put("key", 0, ContractDataDurability::PERSISTENT);
        uint32_t initExpirationLedger = ledgerSeq + minBump - 1;
        REQUIRE(getContractDataExpiration("key",
                                          ContractDataDurability::PERSISTENT) ==
                initExpirationLedger);
        for (uint32 i = app->getLedgerManager().getLastClosedLedgerNum();
             i <= initExpirationLedger + 1; ++i)
        {
            closeLedgerOn(*app, i, 2, 1, 2016);
        }
        REQUIRE(app->getLedgerManager().getLastClosedLedgerNum() ==
                initExpirationLedger + 1);
        REQUIRE(!isEntryLive("key", ContractDataDurability::PERSISTENT,
                             app->getLedgerManager().getLastClosedLedgerNum()));

        // Trying to use this expired entry should fail.
        putWithFootprint(
            "key", 0, contractKeys,
            {contractDataKey(contractID, makeSymbolSCVal("key"),
                             ContractDataDurability::PERSISTENT, DATA_ENTRY)},
            1000, /*expectSuccess*/ false, ContractDataDurability::PERSISTENT);

        // Restore the entry and then write to it.
        restoreOp(
            {contractDataKey(contractID, makeSymbolSCVal("key"),
                             ContractDataDurability::PERSISTENT, DATA_ENTRY)},
            61);
        REQUIRE(
            isEntryLive("key", ContractDataDurability::PERSISTENT,
                        app->getLedgerManager().getLastClosedLedgerNum() + 1));
        // Entry is considered to be restored on lclNum (as we didn't close an
        // additional ledger).
        REQUIRE(getContractDataExpiration("key",
                                          ContractDataDurability::PERSISTENT) ==
                app->getLedgerManager().getLastClosedLedgerNum() + minBump - 1);

        put("key", 1, ContractDataDurability::PERSISTENT);
    }
    SECTION("re-create expired temporary entry")
    {
        auto minBump = InitialSorobanNetworkConfig::MINIMUM_TEMP_ENTRY_LIFETIME;
        put("key", 0, ContractDataDurability::TEMPORARY);
        REQUIRE(has("key", ContractDataDurability::TEMPORARY));

        uint32_t initExpirationLedger = ledgerSeq + minBump + autoBump - 1;
        REQUIRE(getContractDataExpiration("key",
                                          ContractDataDurability::TEMPORARY) ==
                initExpirationLedger);

        for (size_t i = app->getLedgerManager().getLastClosedLedgerNum();
             i <= initExpirationLedger + 1; ++i)
        {
            closeLedgerOn(*app, i, 2, 1, 2016);
        }
        REQUIRE(app->getLedgerManager().getLastClosedLedgerNum() ==
                initExpirationLedger + 1);
        REQUIRE(!isEntryLive("key", ContractDataDurability::TEMPORARY,
                             app->getLedgerManager().getLastClosedLedgerNum()));

        // Entry has expired
        REQUIRE(!has("key", ContractDataDurability::TEMPORARY));

        // We can recreate an expired temp entry
        put("key", 0, ContractDataDurability::TEMPORARY);
        REQUIRE(has("key", ContractDataDurability::TEMPORARY));

        uint32_t newExpirationLedger =
            app->getLedgerManager().getLastClosedLedgerNum() + minBump +
            autoBump - 1;
        REQUIRE(getContractDataExpiration("key",
                                          ContractDataDurability::TEMPORARY) ==
                newExpirationLedger);
    }
    SECTION("max expiration")
    {
        // Check that manual bump doesn't go over max
        put("key", 0, ContractDataDurability::PERSISTENT);
        bump("key", ContractDataDurability::PERSISTENT, UINT32_MAX);

        auto maxExpiration =
            ledgerSeq + stateExpirationSettings.maxEntryExpiration - 1;
        REQUIRE(getContractDataExpiration("key",
                                          ContractDataDurability::PERSISTENT) ==
                maxExpiration);

        // Manual bump to almost max, then autobump to check that autobump
        // doesn't go over max
        put("key2", 0, ContractDataDurability::PERSISTENT);
        bump("key2", ContractDataDurability::PERSISTENT,
             stateExpirationSettings.maxEntryExpiration - 1);
        REQUIRE(getContractDataExpiration("key2",
                                          ContractDataDurability::PERSISTENT) ==
                maxExpiration - 1);

        // Autobump should only add a single ledger to bring expiration to max
        put("key2", 1, ContractDataDurability::PERSISTENT);
        REQUIRE(getContractDataExpiration("key2",
                                          ContractDataDurability::PERSISTENT) ==
                maxExpiration);
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
    resources.extendedMetaDataSizeBytes = 3000;

    auto tx = sorobanTransactionFrameFromOps(app->getNetworkID(), root, {op},
                                             {}, resources, 100'000, 1200);
    LedgerTxn ltx(app->getLedgerTxnRoot());
    TransactionMetaFrame txm(ltx.loadHeader().current().ledgerVersion);
    REQUIRE(tx->checkValid(*app, ltx, 0, 0, 0));
    REQUIRE(!tx->apply(*app, ltx, txm));
    ltx.commit();

    auto const& opEvents = txm.getXDR().v3().sorobanMeta->diagnosticEvents;
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
    createResources.extendedMetaDataSizeBytes = 3000;

    auto metadataKey = LedgerKey(CONTRACT_DATA);
    metadataKey.contractData().contract = contractID;
    metadataKey.contractData().key = makeSymbolSCVal("METADATA");
    metadataKey.contractData().durability = ContractDataDurability::PERSISTENT;
    metadataKey.contractData().bodyType = DATA_ENTRY;

    LedgerKey assetInfoLedgerKey(CONTRACT_DATA);
    assetInfoLedgerKey.contractData().contract = contractID;
    SCVec assetInfo = {makeSymbolSCVal("AssetInfo")};
    SCVal assetInfoSCVal(SCValType::SCV_VEC);
    assetInfoSCVal.vec().activate() = assetInfo;
    assetInfoLedgerKey.contractData().key = assetInfoSCVal;
    assetInfoLedgerKey.contractData().durability =
        ContractDataDurability::PERSISTENT;
    assetInfoLedgerKey.contractData().bodyType = DATA_ENTRY;

    LedgerKey contractExecutableKey(CONTRACT_DATA);
    contractExecutableKey.contractData().contract = contractID;
    contractExecutableKey.contractData().key =
        SCVal(SCValType::SCV_LEDGER_KEY_CONTRACT_INSTANCE);
    contractExecutableKey.contractData().durability =
        ContractDataDurability::PERSISTENT;
    contractExecutableKey.contractData().bodyType = DATA_ENTRY;

    createResources.footprint.readWrite = {contractExecutableKey, metadataKey,
                                           assetInfoLedgerKey};

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
    resources.extendedMetaDataSizeBytes = 3000;

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

    resources.footprint.readOnly = {metadataKey, assetInfoLedgerKey,
                                    contractExecutableKey};

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

#endif
