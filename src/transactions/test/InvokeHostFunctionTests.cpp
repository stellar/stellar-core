// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

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
#include "transactions/TransactionUtils.h"
#include "xdr/Stellar-contract.h"
#include "xdr/Stellar-ledger-entries.h"
#include <autocheck/autocheck.hpp>
#include <fmt/format.h>
#include <limits>
#include <type_traits>
#include <variant>

using namespace stellar;
using namespace stellar::txtest;

// This is an example WASM from the SDK that unpacks two SCV_I32 arguments, adds
// them with an overflow check, and re-packs them as an SCV_I32 if successful.
//
// To regenerate, check out the SDK, install a nightly toolchain with
// the rust-src component (to enable the 'tiny' build) using the following:
//
//  $ rustup component add rust-src --toolchain nightly
//
// clang-format off
// then do:
// $ `wasm-strip target-tiny/wasm32-unknown-unknown/release/example_add_i32.wasm` 
// $ xxd -i target-tiny/wasm32-unknown-unknown/release/example_add_i32.wasm
// clang-format on
std::vector<uint8_t> addI32Wasm{
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x0c, 0x02, 0x60,
    0x02, 0x7f, 0x7e, 0x00, 0x60, 0x02, 0x7e, 0x7e, 0x01, 0x7e, 0x03, 0x03,
    0x02, 0x00, 0x01, 0x05, 0x03, 0x01, 0x00, 0x10, 0x06, 0x19, 0x03, 0x7f,
    0x01, 0x41, 0x80, 0x80, 0xc0, 0x00, 0x0b, 0x7f, 0x00, 0x41, 0x80, 0x80,
    0xc0, 0x00, 0x0b, 0x7f, 0x00, 0x41, 0x80, 0x80, 0xc0, 0x00, 0x0b, 0x07,
    0x2b, 0x04, 0x06, 0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79, 0x02, 0x00, 0x03,
    0x61, 0x64, 0x64, 0x00, 0x01, 0x0a, 0x5f, 0x5f, 0x64, 0x61, 0x74, 0x61,
    0x5f, 0x65, 0x6e, 0x64, 0x03, 0x01, 0x0b, 0x5f, 0x5f, 0x68, 0x65, 0x61,
    0x70, 0x5f, 0x62, 0x61, 0x73, 0x65, 0x03, 0x02, 0x0a, 0x9d, 0x01, 0x02,
    0x3a, 0x01, 0x01, 0x7f, 0x23, 0x00, 0x41, 0x10, 0x6b, 0x22, 0x02, 0x24,
    0x00, 0x20, 0x02, 0x20, 0x01, 0x37, 0x03, 0x08, 0x20, 0x00, 0x20, 0x02,
    0x41, 0x08, 0x6a, 0x29, 0x03, 0x00, 0x22, 0x01, 0x42, 0x04, 0x88, 0x3e,
    0x02, 0x04, 0x20, 0x00, 0x20, 0x01, 0x42, 0x0f, 0x83, 0x42, 0x03, 0x52,
    0x36, 0x02, 0x00, 0x20, 0x02, 0x41, 0x10, 0x6a, 0x24, 0x00, 0x0b, 0x60,
    0x01, 0x03, 0x7f, 0x23, 0x00, 0x41, 0x20, 0x6b, 0x22, 0x02, 0x24, 0x00,
    0x20, 0x02, 0x41, 0x10, 0x6a, 0x20, 0x00, 0x10, 0x00, 0x02, 0x40, 0x20,
    0x02, 0x28, 0x02, 0x10, 0x0d, 0x00, 0x20, 0x02, 0x28, 0x02, 0x14, 0x21,
    0x04, 0x20, 0x02, 0x41, 0x08, 0x6a, 0x20, 0x01, 0x10, 0x00, 0x20, 0x02,
    0x28, 0x02, 0x08, 0x0d, 0x00, 0x20, 0x02, 0x28, 0x02, 0x0c, 0x22, 0x03,
    0x41, 0x00, 0x48, 0x20, 0x04, 0x20, 0x03, 0x20, 0x04, 0x6a, 0x22, 0x03,
    0x4a, 0x47, 0x0d, 0x00, 0x20, 0x02, 0x41, 0x20, 0x6a, 0x24, 0x00, 0x20,
    0x03, 0xad, 0x42, 0x04, 0x86, 0x42, 0x03, 0x84, 0x0f, 0x0b, 0x00, 0x0b};

// This is an example WASM from the SDK that can put and delete arbitrary
// contract data.
// clang-format off
// $ wasm-strip target-tiny/wasm32-unknown-unknown/release/example_contract_data.wasm 
// $ xxd -i target-tiny/wasm32-unknown-unknown/release/example_contract_data.wasm
// clang-format on
std::vector<uint8_t> contractDataWasm{
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x11, 0x03, 0x60,
    0x02, 0x7e, 0x7e, 0x01, 0x7e, 0x60, 0x01, 0x7e, 0x01, 0x7e, 0x60, 0x02,
    0x7f, 0x7e, 0x00, 0x02, 0x0d, 0x02, 0x01, 0x6c, 0x01, 0x5f, 0x00, 0x00,
    0x01, 0x6c, 0x01, 0x32, 0x00, 0x01, 0x03, 0x04, 0x03, 0x02, 0x00, 0x01,
    0x05, 0x03, 0x01, 0x00, 0x10, 0x06, 0x19, 0x03, 0x7f, 0x01, 0x41, 0x80,
    0x80, 0xc0, 0x00, 0x0b, 0x7f, 0x00, 0x41, 0x80, 0x80, 0xc0, 0x00, 0x0b,
    0x7f, 0x00, 0x41, 0x80, 0x80, 0xc0, 0x00, 0x0b, 0x07, 0x31, 0x05, 0x06,
    0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79, 0x02, 0x00, 0x03, 0x70, 0x75, 0x74,
    0x00, 0x03, 0x03, 0x64, 0x65, 0x6c, 0x00, 0x04, 0x0a, 0x5f, 0x5f, 0x64,
    0x61, 0x74, 0x61, 0x5f, 0x65, 0x6e, 0x64, 0x03, 0x01, 0x0b, 0x5f, 0x5f,
    0x68, 0x65, 0x61, 0x70, 0x5f, 0x62, 0x61, 0x73, 0x65, 0x03, 0x02, 0x0a,
    0xd1, 0x01, 0x03, 0x38, 0x01, 0x01, 0x7f, 0x23, 0x00, 0x41, 0x10, 0x6b,
    0x22, 0x02, 0x24, 0x00, 0x20, 0x02, 0x20, 0x01, 0x37, 0x03, 0x08, 0x20,
    0x00, 0x20, 0x02, 0x41, 0x08, 0x6a, 0x29, 0x03, 0x00, 0x22, 0x01, 0x37,
    0x03, 0x08, 0x20, 0x00, 0x20, 0x01, 0x42, 0x0f, 0x83, 0x42, 0x09, 0x52,
    0xad, 0x37, 0x03, 0x00, 0x20, 0x02, 0x41, 0x10, 0x6a, 0x24, 0x00, 0x0b,
    0x57, 0x01, 0x02, 0x7f, 0x23, 0x00, 0x41, 0x20, 0x6b, 0x22, 0x02, 0x24,
    0x00, 0x20, 0x02, 0x41, 0x10, 0x6a, 0x20, 0x00, 0x10, 0x02, 0x02, 0x40,
    0x20, 0x02, 0x29, 0x03, 0x10, 0xa7, 0x0d, 0x00, 0x20, 0x02, 0x29, 0x03,
    0x18, 0x20, 0x02, 0x20, 0x01, 0x10, 0x02, 0x20, 0x02, 0x29, 0x03, 0x00,
    0xa7, 0x0d, 0x00, 0x20, 0x02, 0x29, 0x03, 0x08, 0x23, 0x00, 0x41, 0x10,
    0x6b, 0x22, 0x03, 0x24, 0x00, 0x10, 0x00, 0x1a, 0x20, 0x03, 0x41, 0x10,
    0x6a, 0x24, 0x00, 0x20, 0x02, 0x41, 0x20, 0x6a, 0x24, 0x00, 0x42, 0x05,
    0x0f, 0x0b, 0x00, 0x0b, 0x3e, 0x01, 0x02, 0x7f, 0x23, 0x00, 0x41, 0x10,
    0x6b, 0x22, 0x01, 0x24, 0x00, 0x20, 0x01, 0x20, 0x00, 0x10, 0x02, 0x20,
    0x01, 0x29, 0x03, 0x00, 0xa7, 0x04, 0x40, 0x00, 0x0b, 0x20, 0x01, 0x29,
    0x03, 0x08, 0x23, 0x00, 0x41, 0x10, 0x6b, 0x22, 0x02, 0x24, 0x00, 0x10,
    0x01, 0x1a, 0x20, 0x02, 0x41, 0x10, 0x6a, 0x24, 0x00, 0x20, 0x01, 0x41,
    0x10, 0x6a, 0x24, 0x00, 0x42, 0x05, 0x0b};

template <typename T>
SCVal
makeBinary(T begin, T end)
{
    SCVal val(SCValType::SCV_OBJECT);
    val.obj().activate().type(SCO_BINARY);
    val.obj()->bin().assign(begin, end);
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

template <typename T>
static LedgerKey
deployContract(AbstractLedgerTxn& ltx, T begin, T end)
{
    SCVal wasmKey(SCValType::SCV_STATIC);
    wasmKey.ic() = SCStatic::SCS_LEDGER_KEY_CONTRACT_CODE_WASM;

    LedgerEntry le;
    le.data.type(CONTRACT_DATA);
    le.data.contractData().contractID = HashUtils::pseudoRandomForTesting();
    le.data.contractData().key = wasmKey;
    le.data.contractData().val = makeBinary(begin, end);

    ltx.create(le);
    return LedgerEntryKey(le);
}

template <typename T>
static LedgerKey
deployContract(Application& app, T begin, T end)
{
    LedgerTxn ltx(app.getLedgerTxnRoot());
    auto contract = deployContract(ltx, begin, end);
    ltx.commit();
    return contract;
}

TEST_CASE("invoke host function", "[tx][contract]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    auto root = TestAccount::createRoot(*app);

    SECTION("add i32")
    {
        auto contract =
            deployContract(*app, addI32Wasm.begin(), addI32Wasm.end());
        auto const& contractID = contract.contractData().contractID;

        auto call = [&](SCVec const& parameters, bool success) {
            Operation op;
            op.body.type(INVOKE_HOST_FUNCTION);
            auto& ihf = op.body.invokeHostFunctionOp();
            ihf.function = HOST_FN_CALL;
            ihf.parameters = parameters;
            ihf.footprint.readOnly = {contract};

            auto tx =
                transactionFrameFromOps(app->getNetworkID(), root, {op}, {});
            LedgerTxn ltx(app->getLedgerTxnRoot());
            TransactionMeta txm(2);
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
        };

        auto scContractID = makeBinary(contractID.begin(), contractID.end());
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

        // Too many parameters for "add"
        call({scContractID, scFunc, sc7, sc16, makeI32(0)}, false);
    }

    SECTION("contract data")
    {
        auto contract = deployContract(*app, contractDataWasm.begin(),
                                       contractDataWasm.end());
        auto const& contractID = contract.contractData().contractID;

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
            ihf.function = HOST_FN_CALL;
            ihf.parameters.emplace_back(
                makeBinary(contractID.begin(), contractID.end()));
            ihf.parameters.emplace_back(makeSymbol("put"));
            ihf.parameters.emplace_back(keySymbol);
            ihf.parameters.emplace_back(valSymbol);
            ihf.footprint.readOnly = readOnly;
            ihf.footprint.readWrite = readWrite;

            auto tx =
                transactionFrameFromOps(app->getNetworkID(), root, {op}, {});
            LedgerTxn ltx(app->getLedgerTxnRoot());
            TransactionMeta txm(2);
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
            putWithFootprint(key, val, {contract},
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
            ihf.function = HOST_FN_CALL;
            ihf.parameters.emplace_back(
                makeBinary(contractID.begin(), contractID.end()));
            ihf.parameters.emplace_back(makeSymbol("del"));
            ihf.parameters.emplace_back(keySymbol);
            ihf.footprint.readOnly = readOnly;
            ihf.footprint.readWrite = readWrite;

            auto tx =
                transactionFrameFromOps(app->getNetworkID(), root, {op}, {});
            LedgerTxn ltx(app->getLedgerTxnRoot());
            TransactionMeta txm(2);
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
            delWithFootprint(key, {contract},
                             {contractDataKey(contractID, makeSymbol(key))},
                             true);
        };

        put("key1", "val1a");
        put("key2", "val2a");

        // Failure: contract data isn't in footprint
        putWithFootprint("key1", "val1b", {contract}, {}, false);
        delWithFootprint("key1", {contract}, {}, false);

        // Failure: contract data is read only
        auto cdk = contractDataKey(contractID, makeSymbol("key2"));
        putWithFootprint("key2", "val2b", {contract, cdk}, {}, false);
        delWithFootprint("key2", {contract, cdk}, {}, false);

        put("key1", "val1c");
        put("key2", "val2c");

        del("key1");
        del("key2");
    }
}

#endif
