// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <iterator>
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
makeContract(T begin, T end)
{
    SCVal val(SCValType::SCV_OBJECT);
    val.obj().activate().type(stellar::SCO_CONTRACT_CODE);
    val.obj()->contractCode().type(stellar::SCCONTRACT_CODE_WASM);
    val.obj()->contractCode().wasm().assign(begin, end);
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
submitOpToCreateContract(Application& app, Operation const& op,
                         RustBuf const& contract, Hash const& contractID,
                         SCVal const& wasmKey, bool expectSuccess,
                         bool expectEntry)
{
    // submit operation
    auto root = TestAccount::createRoot(app);
    auto tx = transactionFrameFromOps(app.getNetworkID(), root, {op}, {});
    LedgerTxn ltx(app.getLedgerTxnRoot());
    TransactionMetaFrame txm(ltx.loadHeader().current().ledgerVersion);
    REQUIRE(tx->checkValid(ltx, 0, 0, 0));
    REQUIRE(tx->apply(app, ltx, txm) == expectSuccess);
    ltx.commit();

    auto contractCodeObj =
        makeContract(contract.data.begin(), contract.data.end());
    // verify contract code is correct
    LedgerTxn ltx2(app.getLedgerTxnRoot());
    auto ltxe2 = loadContractData(ltx2, contractID, wasmKey);
    REQUIRE(static_cast<bool>(ltxe2) == expectEntry);
    // FIXME: it's a little weird that we put a contractBin in and get a
    // contractCodeObj out. This is probably a residual error from before an API
    // change. See https://github.com/stellar/rs-soroban-env/issues/369
    REQUIRE((!expectEntry ||
             ltxe2.current().data.contractData().val == contractCodeObj));
}

static LedgerKey
createContractFromEd25519(Application& app, RustBuf const& contract,
                          uint256 const& salt, PublicKey const& pub,
                          Signature const& sig, bool expectSuccess,
                          bool expectEntry)
{
    HashIDPreimage preImage;
    preImage.type(ENVELOPE_TYPE_CONTRACT_ID_FROM_ED25519);
    preImage.ed25519ContractID().ed25519 = pub.ed25519();
    preImage.ed25519ContractID().salt = salt;
    auto contractID = xdrSha256(preImage);

    // Create operation
    Operation op;
    op.body.type(INVOKE_HOST_FUNCTION);
    auto& ihf = op.body.invokeHostFunctionOp();
    ihf.function = HOST_FN_CREATE_CONTRACT_WITH_ED25519;

    auto contractBin = makeBinary(contract.data.begin(), contract.data.end());
    ihf.parameters = {contractBin, makeBinary(salt.begin(), salt.end()),
                      makeBinary(pub.ed25519().begin(), pub.ed25519().end()),
                      makeBinary(sig.begin(), sig.end())};

    SCVal wasmKey(SCValType::SCV_STATIC);
    wasmKey.ic() = SCStatic::SCS_LEDGER_KEY_CONTRACT_CODE;

    LedgerKey lk;
    lk.type(CONTRACT_DATA);
    lk.contractData().contractID = contractID;
    lk.contractData().key = wasmKey;

    ihf.footprint.readWrite = {lk};

    submitOpToCreateContract(app, op, contract, contractID, wasmKey,
                             expectSuccess, expectEntry);
    return lk;
}

static LedgerKey
createContractFromSource(Application& app, RustBuf const& contract,
                         uint256 const& salt, bool expectSuccess,
                         bool expectEntry)
{
    auto root = TestAccount::createRoot(app);

    HashIDPreimage preImage;
    preImage.type(ENVELOPE_TYPE_CONTRACT_ID_FROM_SOURCE_ACCOUNT);
    preImage.sourceAccountContractID().sourceAccount = root.getPublicKey();
    preImage.sourceAccountContractID().salt = salt;
    auto contractID = xdrSha256(preImage);

    // Create operation
    Operation op;
    op.body.type(INVOKE_HOST_FUNCTION);
    auto& ihf = op.body.invokeHostFunctionOp();
    ihf.function = HOST_FN_CREATE_CONTRACT_WITH_SOURCE_ACCOUNT;

    auto contractBin = makeBinary(contract.data.begin(), contract.data.end());
    ihf.parameters = {contractBin, makeBinary(salt.begin(), salt.end())};

    SCVal wasmKey(SCValType::SCV_STATIC);
    wasmKey.ic() = SCStatic::SCS_LEDGER_KEY_CONTRACT_CODE;

    LedgerKey lk;
    lk.type(CONTRACT_DATA);
    lk.contractData().contractID = contractID;
    lk.contractData().key = wasmKey;

    ihf.footprint.readWrite = {lk};

    submitOpToCreateContract(app, op, contract, contractID, wasmKey,
                             expectSuccess, expectEntry);
    return lk;
}

static LedgerKey
deployContract(Application& app, RustBuf const& contract, HostFunction fn,
               uint256 salt = sha256("salt"))
{
    switch (fn)
    {
    case HostFunction::HOST_FN_CREATE_CONTRACT_WITH_ED25519:
    {
        auto key = SecretKey::fromSeed(sha256("a1"));

        // create signature
        auto const& separator =
            "create_contract_from_ed25519(contract: Vec<u8>, "
            "salt: u256, key: u256, sig: Vec<u8>)";
        SHA256 hasher;
        hasher.add(separator);
        hasher.add(salt);
        hasher.add(contract);

        auto sig = SignatureUtils::sign(key, hasher.finish()).signature;
        return createContractFromEd25519(app, contract, salt,
                                         key.getPublicKey(), sig, true, true);
    }
    case HostFunction::HOST_FN_CREATE_CONTRACT_WITH_SOURCE_ACCOUNT:
    {
        return createContractFromSource(app, contract, salt, true, true);
    }
    case HostFunction::HOST_FN_INVOKE_CONTRACT:
        throw std::runtime_error("Invalid HostFunction");
    default:
        throw std::runtime_error("Unknown HostFunction");
    }
}

TEST_CASE("invoke host function", "[tx][contract]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    auto root = TestAccount::createRoot(*app);

    auto const addI32Wasm = rust_bridge::get_test_wasm_add_i32();
    auto const contractDataWasm = rust_bridge::get_test_wasm_contract_data();

    {
        auto addI32 = [&](HostFunction fn) {
            auto contract = deployContract(*app, addI32Wasm, fn);
            auto const& contractID = contract.contractData().contractID;

            auto call = [&](SCVec const& parameters, bool success) {
                Operation op;
                op.body.type(INVOKE_HOST_FUNCTION);
                auto& ihf = op.body.invokeHostFunctionOp();
                ihf.function = HOST_FN_INVOKE_CONTRACT;
                ihf.parameters = parameters;
                ihf.footprint.readOnly = {contract};

                auto tx = transactionFrameFromOps(app->getNetworkID(), root,
                                                  {op}, {});
                LedgerTxn ltx(app->getLedgerTxnRoot());
                TransactionMetaFrame txm(ltx.loadHeader().current().ledgerVersion);
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

            // Too many parameters for "add"
            call({scContractID, scFunc, sc7, sc16, makeI32(0)}, false);
        };
        SECTION("create with ed25519 -  add i32")
        {
            addI32(HostFunction::HOST_FN_CREATE_CONTRACT_WITH_ED25519);
        }
        SECTION("create with source -  add i32")
        {
            addI32(HostFunction::HOST_FN_CREATE_CONTRACT_WITH_SOURCE_ACCOUNT);
        }
    }

    SECTION("contract data")
    {
        auto contract =
            deployContract(*app, contractDataWasm,
                           HostFunction::HOST_FN_CREATE_CONTRACT_WITH_ED25519);
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
            ihf.function = HOST_FN_INVOKE_CONTRACT;
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
            ihf.function = HOST_FN_INVOKE_CONTRACT;
            ihf.parameters.emplace_back(
                makeBinary(contractID.begin(), contractID.end()));
            ihf.parameters.emplace_back(makeSymbol("del"));
            ihf.parameters.emplace_back(keySymbol);
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

    SECTION("create contract failures")
    {
        // create signature
        auto const& separator =
            "create_contract_from_ed25519(contract: Vec<u8>, "
            "salt: u256, key: u256, sig: Vec<u8>)";
        uint256 salt = sha256("salt");
        auto key = SecretKey::fromSeed(sha256("a1"));

        {
            SHA256 hasher;
            hasher.add(separator);
            hasher.add(salt);
            hasher.add(addI32Wasm);
            auto sig = SignatureUtils::sign(key, hasher.finish()).signature;

            // public key is different than the one that created the signature
            auto new_pub = SecretKey::fromSeed(sha256("a2"));
            createContractFromEd25519(*app, addI32Wasm, salt,
                                      new_pub.getPublicKey(), sig, false,
                                      false);
        }

        {
            // bad separator
            SHA256 hasher;
            hasher.add("bad_separator");
            hasher.add(salt);
            hasher.add(addI32Wasm);
            auto sig = SignatureUtils::sign(key, hasher.finish()).signature;

            createContractFromEd25519(*app, addI32Wasm, salt,
                                      key.getPublicKey(), sig, false, false);
        }

        {
            // Incorrect salt was hashed
            SHA256 hasher;
            hasher.add(separator);
            hasher.add(sha256("wrong_salt"));
            hasher.add(addI32Wasm);
            auto sig = SignatureUtils::sign(key, hasher.finish()).signature;

            createContractFromEd25519(*app, addI32Wasm, salt,
                                      key.getPublicKey(), sig, false, false);
        }

        {
            // Incorrect contract was hashed
            SHA256 hasher;
            hasher.add(separator);
            hasher.add(salt);
            hasher.add(contractDataWasm);
            auto sig = SignatureUtils::sign(key, hasher.finish()).signature;

            createContractFromEd25519(*app, addI32Wasm, salt,
                                      key.getPublicKey(), sig, false, false);
        }

        {
            // duplicate contract
            SHA256 hasher;
            hasher.add(separator);
            hasher.add(salt);
            hasher.add(addI32Wasm);

            auto sig = SignatureUtils::sign(key, hasher.finish()).signature;
            createContractFromEd25519(*app, addI32Wasm, salt,
                                      key.getPublicKey(), sig, true, true);
            createContractFromEd25519(*app, addI32Wasm, salt,
                                      key.getPublicKey(), sig, false, true);
        }
    }
}

#endif
