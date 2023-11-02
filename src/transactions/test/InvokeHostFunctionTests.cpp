// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Logging.h"
#include "xdr/Stellar-transaction.h"
#include <iterator>
#include <stdexcept>
#include <xdrpp/printer.h>

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
#include "transactions/test/SponsorshipTestUtils.h"
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
makeI128(uint64_t u64)
{
    Int128Parts p;
    p.hi = 0;
    p.lo = u64;

    SCVal val(SCV_I128);
    val.i128() = p;
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

static void
checkTTL(LedgerTxn& ltx, LedgerKey const& key, uint32_t expectedLiveUntilLedger)
{
    auto ttlKey = getTTLKey(key);
    auto ttlLtxe = ltx.loadWithoutRecord(ttlKey);
    REQUIRE(ttlLtxe);
    REQUIRE(ttlLtxe.current().data.ttl().liveUntilLedgerSeq ==
            expectedLiveUntilLedger);
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
                     xdr::opaque_vec<> const& expectedWasm,
                     uint32_t inclusionFee, uint32_t resourceFee)
{
    // submit operation
    auto root = TestAccount::createRoot(app);
    auto tx =
        sorobanTransactionFrameFromOps(app.getNetworkID(), root, {op}, {},
                                       resources, inclusionFee, resourceFee);
    LedgerTxn ltx(app.getLedgerTxnRoot());
    TransactionMetaFrame txm(ltx.loadHeader().current().ledgerVersion);
    REQUIRE(tx->checkValid(app, ltx, 0, 0, 0));
    REQUIRE(tx->apply(app, ltx, txm));
    ltx.commit();

    // verify contract code is correct
    LedgerTxn ltx2(app.getLedgerTxnRoot());
    auto ltxe = loadContractCode(ltx2, expectedWasmHash);
    REQUIRE(ltxe);
    REQUIRE(ltxe.current().data.contractCode().code == expectedWasm);
}

static void
submitTxToCreateContract(Application& app, Operation const& op,
                         SorobanResources const& resources,
                         Hash const& contractID, SCVal const& executableKey,
                         Hash const& expectedWasmHash, uint32_t inclusionFee,
                         uint32_t resourceFee)
{
    // submit operation
    auto root = TestAccount::createRoot(app);
    auto tx =
        sorobanTransactionFrameFromOps(app.getNetworkID(), root, {op}, {},
                                       resources, inclusionFee, resourceFee);
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
    REQUIRE(cd.val == makeWasmRefScContractCode(expectedWasmHash));
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
    submitTxToUploadWasm(
        app, uploadOp, uploadResources,
        contractCodeLedgerKey.contractCode().hash, uploadHF.wasm(), 1000,
        sorobanResourceFee(app, uploadResources,
                           1000 + contractWasm.data.size(), 40) +
            40'000);

    // Check ttls for contract code
    {
        LedgerTxn ltx(app.getLedgerTxnRoot());
        auto networkConfig =
            app.getLedgerManager().getSorobanNetworkConfig(ltx);
        auto ledgerSeq = ltx.loadHeader().current().ledgerSeq;
        auto expectedLiveUntilLedger =
            networkConfig.stateArchivalSettings().minPersistentTTL + ledgerSeq -
            1;

        auto codeLtxe = ltx.load(contractCodeLedgerKey);
        REQUIRE(codeLtxe);
        checkTTL(ltx, contractCodeLedgerKey, expectedLiveUntilLedger);
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
        contractCodeLedgerKey.contractCode().hash, 1000,
        sorobanResourceFee(app, createResources, 1000, 40) + 40'000);

    // Check ttls for contract instance
    LedgerTxn ltx(app.getLedgerTxnRoot());
    auto networkConfig = app.getLedgerManager().getSorobanNetworkConfig(ltx);
    auto ledgerSeq = ltx.loadHeader().current().ledgerSeq;
    auto expectedLiveUntilLedger =
        networkConfig.stateArchivalSettings().minPersistentTTL + ledgerSeq - 1;

    auto instanceLtxe = ltx.load(contractSourceRefLedgerKey);
    REQUIRE(instanceLtxe);
    checkTTL(ltx, contractSourceRefLedgerKey, expectedLiveUntilLedger);

    return {contractSourceRefLedgerKey, contractCodeLedgerKey};
}

static xdr::xvector<LedgerKey>
deployContractWithSourceAccount(Application& app, RustBuf const& contractWasm,
                                uint256 salt = sha256("salt"))
{
    SorobanResources uploadResources{};
    uploadResources.instructions = 200'000 + (contractWasm.data.size() * 6000);
    uploadResources.readBytes = 1000;
    uploadResources.writeBytes = 5000;

    SorobanResources createResources{};
    createResources.instructions = 200'000;
    createResources.readBytes = 5000;
    createResources.writeBytes = 5000;
    return deployContractWithSourceAccountWithResources(
        app, contractWasm, uploadResources, createResources, salt);
}

// Fee constants from rs-soroban-env/soroban-env-host/src/fees.rs
constexpr int64_t INSTRUCTION_INCREMENT = 10000;
constexpr int64_t DATA_SIZE_1KB_INCREMENT = 1024;
constexpr int64_t TX_BASE_RESULT_SIZE = 300;

static int64_t
computeFeePerIncrement(int64_t resourceVal, int64_t feeRate, int64_t increment)
{
    // ceiling division for (resourceVal * feeRate) / increment
    int64_t num = (resourceVal * feeRate);
    return (num + increment - 1) / increment;
};

static int64_t
getRentFeeForBytes(int64_t entrySize, uint32_t numLedgersToExtend,
                   SorobanNetworkConfig const& cfg, bool isPersistent)
{
    auto num = entrySize * cfg.feeWrite1KB() * numLedgersToExtend;
    auto storageCoef =
        isPersistent ? cfg.stateArchivalSettings().persistentRentRateDenominator
                     : cfg.stateArchivalSettings().tempRentRateDenominator;

    auto denom = DATA_SIZE_1KB_INCREMENT * storageCoef;

    // Ceiling division
    return (num + denom - 1) / denom;
}

static int64_t
getTTLEntryWriteFee(SorobanNetworkConfig const& cfg)
{
    LedgerEntry le;
    le.data.type(TTL);

    auto writeSize = xdr::xdr_size(le);
    auto writeFee = computeFeePerIncrement(writeSize, cfg.feeWrite1KB(),
                                           DATA_SIZE_1KB_INCREMENT);
    writeFee += cfg.feeWriteLedgerEntry();
    return writeFee;
}

static int64_t
getRentFeeForExtend(Application& app, LedgerKey const& key,
                    uint32_t newLifetime, SorobanNetworkConfig const& cfg)
{
    LedgerTxn ltx(app.getLedgerTxnRoot());
    auto ledgerSeq = ltx.getHeader().ledgerSeq;
    auto ttlKey = getTTLKey(key);
    auto ttlLtxe = ltx.loadWithoutRecord(ttlKey);
    releaseAssert(ttlLtxe);

    TTLEntry const& ttlEntry = ttlLtxe.current().data.ttl();

    uint32_t numLedgersToExtend = 0;
    if (isLive(ttlLtxe.current(), ledgerSeq))
    {
        auto ledgerToExtendTo = ledgerSeq + newLifetime;
        if (ttlEntry.liveUntilLedgerSeq >= ledgerToExtendTo)
        {
            return 0;
        }

        numLedgersToExtend = ledgerToExtendTo - ttlEntry.liveUntilLedgerSeq;
    }
    else
    {
        // Expired entries are skipped, pay no rent fee
        return 0;
    }

    auto txle = ltx.loadWithoutRecord(key);
    releaseAssert(txle);

    auto entrySize = xdr::xdr_size(txle.current());
    auto rentFee = getRentFeeForBytes(entrySize, numLedgersToExtend, cfg,
                                      isPersistentEntry(key));

    return rentFee + getTTLEntryWriteFee(cfg);
}

TEST_CASE("non-refundable resource metering", "[tx][soroban]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    overrideSorobanNetworkConfigForTest(*app);

    uniform_int_distribution<uint64_t> feeDist(1, 100'000);
    modifySorobanNetworkConfig(*app, [&feeDist](SorobanNetworkConfig& cfg) {
        cfg.mFeeRatePerInstructionsIncrement = feeDist(Catch::rng());
        cfg.mFeeReadLedgerEntry = feeDist(Catch::rng());
        cfg.mFeeWriteLedgerEntry = feeDist(Catch::rng());
        cfg.mFeeRead1KB = feeDist(Catch::rng());
        cfg.mFeeWrite1KB = feeDist(Catch::rng());
        cfg.mFeeHistorical1KB = feeDist(Catch::rng());
        cfg.mFeeTransactionSize1KB = feeDist(Catch::rng());
    });

    SorobanNetworkConfig const& cfg = [&] {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        return app->getLedgerManager().getSorobanNetworkConfig(ltx);
    }();

    Operation op;
    op.body.type(INVOKE_HOST_FUNCTION);
    auto& ihf = op.body.invokeHostFunctionOp().hostFunction;
    ihf.type(HOST_FUNCTION_TYPE_INVOKE_CONTRACT);

    SorobanResources resources;
    resources.instructions = 0;
    resources.readBytes = 0;
    resources.writeBytes = 0;

    auto root = TestAccount::createRoot(*app);

    // The following computations are copies of compute_transaction_resource_fee
    // in rs-soroban-env/soroban-env-host/src/fees.rs. This is reimplemented
    // here so that we can check if the Cxx bridge introduced any fee related
    // bugs.

    // Fees that depend on TX size, historicalFee and bandwidthFee
    auto txSizeFees = [&](TransactionFrameBasePtr tx) {
        int64_t txSize = static_cast<uint32>(xdr::xdr_size(tx->getEnvelope()));
        int64_t historicalFee = computeFeePerIncrement(
            txSize + TX_BASE_RESULT_SIZE, cfg.mFeeHistorical1KB,
            DATA_SIZE_1KB_INCREMENT);

        int64_t bandwidthFee = computeFeePerIncrement(
            txSize, cfg.feeTransactionSize1KB(), DATA_SIZE_1KB_INCREMENT);

        return historicalFee + bandwidthFee;
    };

    auto computeFee = [&] {
        return computeFeePerIncrement(resources.instructions,
                                      cfg.feeRatePerInstructionsIncrement(),
                                      INSTRUCTION_INCREMENT);
    };

    auto entryReadFee = [&] {
        return (resources.footprint.readOnly.size() +
                resources.footprint.readWrite.size()) *
               cfg.feeReadLedgerEntry();
    };

    auto entryWriteFee = [&] {
        return resources.footprint.readWrite.size() * cfg.feeWriteLedgerEntry();
    };

    auto readBytesFee = [&] {
        return computeFeePerIncrement(resources.readBytes, cfg.feeRead1KB(),
                                      DATA_SIZE_1KB_INCREMENT);
    };

    auto writeBytesFee = [&] {
        return computeFeePerIncrement(resources.writeBytes, cfg.feeWrite1KB(),
                                      DATA_SIZE_1KB_INCREMENT);
    };

    auto checkFees = [&](int64_t expectedNonRefundableFee,
                         std::shared_ptr<TransactionFrameBase> rootTX) {
        LedgerTxn ltx(app->getLedgerTxnRoot());

        // This will compute soroban fees
        REQUIRE(rootTX->checkValid(*app, ltx, 0, 0, 0));

        auto actualFeePair = std::dynamic_pointer_cast<TransactionFrame>(rootTX)
                                 ->getSorobanResourceFee();
        REQUIRE(actualFeePair);
        REQUIRE(expectedNonRefundableFee == actualFeePair->non_refundable_fee);

        auto inclusionFee = getMinInclusionFee(*rootTX, ltx.getHeader());

        // Check that minimum fee succeeds
        auto minimalTX = sorobanTransactionFrameFromOps(
            app->getNetworkID(), root, {op}, {}, resources, inclusionFee,
            expectedNonRefundableFee);
        REQUIRE(minimalTX->checkValid(*app, ltx, 1, 0, 0));

        // Check that just below minimum resource fee fails
        auto badTX = sorobanTransactionFrameFromOps(
            app->getNetworkID(), root, {op}, {}, resources, inclusionFee,
            expectedNonRefundableFee - 1);
        REQUIRE(!badTX->checkValid(*app, ltx, 2, 0, 0));

        // Check that just below minimum inclusion fee fails
        auto badTX2 = sorobanTransactionFrameFromOps(
            app->getNetworkID(), root, {op}, {}, resources, inclusionFee - 1,
            // It doesn't matter how high the resource fee is.
            expectedNonRefundableFee + 1'000'000);
        REQUIRE(!badTX2->checkValid(*app, ltx, 2, 0, 0));
    };

    // In the following tests, we isolate a single fee to test by zeroing out
    // every other resource, as much as is possible
    SECTION("tx size fees")
    {
        auto tx = sorobanTransactionFrameFromOps(
            app->getNetworkID(), root, {op}, {}, resources, 100,
            std::numeric_limits<uint32_t>::max() - 100);

        // Resources are all null, only include TX size based fees
        auto expectedFee = txSizeFees(tx);
        checkFees(expectedFee, tx);
    }

    SECTION("compute fee")
    {
        resources.instructions = feeDist(Catch::rng());

        auto tx = sorobanTransactionFrameFromOps(
            app->getNetworkID(), root, {op}, {}, resources, 100,
            std::numeric_limits<uint32_t>::max() - 100);

        // Should only have instruction and TX size fees
        auto expectedFee = txSizeFees(tx) + computeFee();
        checkFees(expectedFee, tx);
    }

    uniform_int_distribution<> numKeysDist(1, 10);
    SECTION("entry read/write fee")
    {
        SECTION("RO Only")
        {
            auto size = numKeysDist(Catch::rng());
            for (auto const& e :
                 LedgerTestUtils::generateValidUniqueLedgerEntriesWithTypes(
                     {CONTRACT_DATA, CONTRACT_CODE}, size))
            {
                resources.footprint.readOnly.emplace_back(LedgerEntryKey(e));
            }

            auto tx = sorobanTransactionFrameFromOps(
                app->getNetworkID(), root, {op}, {}, resources, 100,
                std::numeric_limits<uint32_t>::max() - 100);

            // Only read entry fees and TX size fees
            auto expectedFee = txSizeFees(tx) + entryReadFee();
            checkFees(expectedFee, tx);
        }

        SECTION("RW Only")
        {
            auto size = numKeysDist(Catch::rng());
            for (auto const& e :
                 LedgerTestUtils::generateValidUniqueLedgerEntriesWithTypes(
                     {CONTRACT_DATA, CONTRACT_CODE}, size))
            {
                resources.footprint.readWrite.emplace_back(LedgerEntryKey(e));
            }

            auto tx = sorobanTransactionFrameFromOps(
                app->getNetworkID(), root, {op}, {}, resources, 100,
                std::numeric_limits<uint32_t>::max() - 100);

            // Only read entry, write entry, and TX size fees
            auto expectedFee =
                txSizeFees(tx) + entryReadFee() + entryWriteFee();
            checkFees(expectedFee, tx);
        }

        SECTION("RW and RO")
        {
            auto size = numKeysDist(Catch::rng());
            for (auto const& e :
                 LedgerTestUtils::generateValidUniqueLedgerEntriesWithTypes(
                     {CONTRACT_DATA, CONTRACT_CODE}, size))
            {
                resources.footprint.readOnly.emplace_back(LedgerEntryKey(e));
            }

            size = numKeysDist(Catch::rng());
            for (auto const& e :
                 LedgerTestUtils::generateValidUniqueLedgerEntriesWithTypes(
                     {CONTRACT_DATA, CONTRACT_CODE}, size))
            {
                resources.footprint.readWrite.emplace_back(LedgerEntryKey(e));
            }

            auto tx = sorobanTransactionFrameFromOps(
                app->getNetworkID(), root, {op}, {}, resources, 100,
                std::numeric_limits<uint32_t>::max() - 100);

            // Only read entry, write entry, and TX size fees
            auto expectedFee =
                txSizeFees(tx) + entryReadFee() + entryWriteFee();
            checkFees(expectedFee, tx);
        }
    }

    uniform_int_distribution<uint32_t> bytesDist(1, 100'000);
    SECTION("readBytes fee")
    {
        resources.readBytes = bytesDist(Catch::rng());
        auto tx = sorobanTransactionFrameFromOps(
            app->getNetworkID(), root, {op}, {}, resources, 100,
            std::numeric_limits<uint32_t>::max() - 100);

        // Only readBytes and TX size fees
        auto expectedFee = txSizeFees(tx) + readBytesFee();
        checkFees(expectedFee, tx);
    }

    SECTION("writeBytes fee")
    {
        resources.writeBytes = bytesDist(Catch::rng());
        auto tx = sorobanTransactionFrameFromOps(
            app->getNetworkID(), root, {op}, {}, resources, 100,
            std::numeric_limits<uint32_t>::max() - 100);

        // Only writeBytes and TX size fees
        auto expectedFee = txSizeFees(tx) + writeBytesFee();
        checkFees(expectedFee, tx);
    }
}

TEST_CASE("basic contract invocation", "[tx][soroban]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
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

        auto tx = sorobanTransactionFrameFromOps(app->getNetworkID(), root,
                                                 {op}, {}, resources, 100,
                                                 DEFAULT_TEST_RESOURCE_FEE);

        LedgerTxn ltx(app->getLedgerTxnRoot());
        return tx->checkValid(*app, ltx, 0, 0, 0);
    };

    auto isExtendOpValid = [&](SorobanResources const& resources) -> bool {
        Operation op;
        op.body.type(EXTEND_FOOTPRINT_TTL);
        op.body.extendFootprintTTLOp().extendTo = 10;

        auto tx = sorobanTransactionFrameFromOps(app->getNetworkID(), root,
                                                 {op}, {}, resources, 100,
                                                 DEFAULT_TEST_RESOURCE_FEE);

        LedgerTxn ltx(app->getLedgerTxnRoot());
        return tx->checkValid(*app, ltx, 0, 0, 0);
    };

    auto isRestorationOpValid = [&](SorobanResources const& resources) -> bool {
        Operation op;
        op.body.type(RESTORE_FOOTPRINT);

        auto tx = sorobanTransactionFrameFromOps(app->getNetworkID(), root,
                                                 {op}, {}, resources, 100,
                                                 DEFAULT_TEST_RESOURCE_FEE);

        LedgerTxn ltx(app->getLedgerTxnRoot());
        return tx->checkValid(*app, ltx, 0, 0, 0);
    };

    auto call = [&](SorobanResources const& resources, uint32_t resourceFee,
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
                                           resources, 12'345, resourceFee);
        {
            LedgerTxn ltx(app->getLedgerTxnRoot());
            REQUIRE(tx->checkValid(*app, ltx, 0, 0, 0));
            ltx.commit();
        }
        TransactionMetaFrame txm(app->getLedgerManager()
                                     .getLastClosedLedgerHeader()
                                     .header.ledgerVersion);
        auto nonRefundableResourceFee = sorobanResourceFee(
            *app, resources, xdr::xdr_size(tx->getEnvelope()), 0);
        if (success)
        {
            REQUIRE(tx->getFullFee() == resourceFee + 12'345);
            REQUIRE(tx->getInclusionFee() == 12'345);
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
            auto const expectedRefund = 267'298;
            {
                LedgerTxn ltx(app->getLedgerTxnRoot());
                REQUIRE(tx->apply(*app, ltx, txm));
                tx->processPostApply(*app, ltx, txm);
                ltx.commit();
                auto changesAfter = txm.getChangesAfter();
                REQUIRE(changesAfter.size() == 2);
                REQUIRE(changesAfter[1].updated().data.account().balance -
                            changesAfter[0].state().data.account().balance ==
                        expectedRefund);
            }
            // The account should receive a refund for unspent refundable fee.
            REQUIRE(root.getBalance() - balanceAfterFeeCharged ==
                    expectedRefund);
            // Sanity-check the expected refund value. The exact computation is
            // tricky because of the rent bump fee.
            // NB: We don't use the computed value here in order
            // to check possible changes in fee computation algorithm (so that
            // the conditions above would fail, while this still succeeds).
            REQUIRE(sorobanResourceFee(
                        *app, resources, xdr::xdr_size(tx->getEnvelope()),
                        100 /* size of event emitted by the test contract */) ==
                    resourceFee - expectedRefund);
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
                if (resourceFee > 0)
                {
                    auto changesAfter = txm.getChangesAfter();
                    REQUIRE(changesAfter.size() == 2);
                    REQUIRE(
                        changesAfter[1].updated().data.account().balance -
                            changesAfter[0].state().data.account().balance ==
                        resourceFee - nonRefundableResourceFee);
                }
                // The account should receive a full refund for metadata
                // in case of tx failure.
                REQUIRE(root.getBalance() - balanceAfterFeeCharged ==
                        resourceFee - nonRefundableResourceFee);
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
        call(resources, 300'000, contractID, scFunc, {sc7, sc16}, true);
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
            call(resources, DEFAULT_TEST_RESOURCE_FEE, address, scFunc,
                 {sc7, sc16}, false);
        }
        SECTION("account address")
        {
            SCAddress address(SC_ADDRESS_TYPE_ACCOUNT);
            address.accountId() = root.getPublicKey();
            call(resources, DEFAULT_TEST_RESOURCE_FEE, address, scFunc,
                 {sc7, sc16}, false);
        }
        SECTION("too few parameters")
        {
            call(resources, DEFAULT_TEST_RESOURCE_FEE, contractID, scFunc,
                 {sc7}, false);
        }
        SECTION("too many parameters")
        {
            // Too many parameters
            call(resources, DEFAULT_TEST_RESOURCE_FEE, contractID, scFunc,
                 {sc7, sc16, makeI32(0)}, false);
        }
    }

    SECTION("insufficient instructions")
    {
        resources.instructions = 10000;
        call(resources, DEFAULT_TEST_RESOURCE_FEE, contractID, scFunc,
             {sc7, sc16}, false);
    }
    SECTION("insufficient read bytes")
    {
        resources.readBytes = 100;
        call(resources, DEFAULT_TEST_RESOURCE_FEE, contractID, scFunc,
             {sc7, sc16}, false);
    }
    SECTION("insufficient refundable fee")
    {
        call(resources, 32'701, contractID, scFunc, {sc7, sc16}, false);
    }
    SECTION("invalid footprint keys")
    {

        auto persistentKey =
            contractDataKey(contractID, makeSymbolSCVal("key1"),
                            ContractDataDurability::PERSISTENT);
        auto ttlKey = getTTLKey(persistentKey);
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
            SECTION("ttl entry")
            {
                footprint.emplace_back(ttlKey);
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
        SECTION("extendOp")
        {
            SECTION("valid")
            {
                REQUIRE(isExtendOpValid(resources));
            }
            SECTION("non empty RW set")
            {
                resources.footprint.readWrite.emplace_back(persistentKey);
                REQUIRE(!isExtendOpValid(resources));
            }
            SECTION("ttl entry")
            {
                resources.footprint.readOnly.emplace_back(ttlKey);
                REQUIRE(!isExtendOpValid(resources));
            }
            SECTION("non soroban entry")
            {
                auto entries =
                    LedgerTestUtils::generateValidLedgerEntryKeysWithExclusions(
                        {CONTRACT_CODE, CONTRACT_DATA}, 10);
                for (auto const& entry : entries)
                {
                    resources.footprint.readOnly.emplace_back(entry);
                    REQUIRE(!isExtendOpValid(resources));
                    resources.footprint.readOnly.pop_back();
                }
            }
        }
        SECTION("restoreOp")
        {
            resources.footprint.readWrite = resources.footprint.readOnly;
            resources.footprint.readOnly.clear();
            SECTION("valid")
            {
                REQUIRE(isRestorationOpValid(resources));
            }
            SECTION("non empty RO set")
            {
                resources.footprint.readOnly.emplace_back(persistentKey);
                REQUIRE(!isRestorationOpValid(resources));
            }
            SECTION("ttl entry")
            {
                resources.footprint.readWrite.emplace_back(ttlKey);
                REQUIRE(!isRestorationOpValid(resources));
            }
            SECTION("non soroban entry")
            {
                auto entries =
                    LedgerTestUtils::generateValidLedgerEntryKeysWithExclusions(
                        {CONTRACT_CODE, CONTRACT_DATA}, 10);
                for (auto const& entry : entries)
                {
                    resources.footprint.readWrite.emplace_back(entry);
                    REQUIRE(!isRestorationOpValid(resources));
                    resources.footprint.readWrite.pop_back();
                }
            }
        }
    }
}

TEST_CASE("refund account merged", "[tx][soroban][merge]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    auto app = createTestApplication(clock, cfg);
    overrideSorobanNetworkConfigForTest(*app);

    auto root = TestAccount::createRoot(*app);
    int64_t initBalance = root.getBalance();

    const int64_t startingBalance =
        app->getLedgerManager().getLastMinBalance(50);

    auto a1 = root.create("A", startingBalance);
    auto b1 = root.create("B", startingBalance);
    auto c1 = root.create("C", startingBalance);

    auto const addI32Wasm = rust_bridge::get_test_wasm_add_i32();

    SorobanResources uploadResources{};
    uploadResources.instructions = 200'000 + (addI32Wasm.data.size() * 6000);
    uploadResources.readBytes = 1000;
    uploadResources.writeBytes = 5000;

    // Upload contract code
    Operation uploadOp;
    uploadOp.body.type(INVOKE_HOST_FUNCTION);
    auto& uploadHF = uploadOp.body.invokeHostFunctionOp().hostFunction;
    uploadHF.type(HOST_FUNCTION_TYPE_UPLOAD_CONTRACT_WASM);
    uploadHF.wasm().assign(addI32Wasm.data.begin(), addI32Wasm.data.end());

    LedgerKey contractCodeLedgerKey;
    contractCodeLedgerKey.type(CONTRACT_CODE);
    contractCodeLedgerKey.contractCode().hash = sha256(uploadHF.wasm());
    uploadResources.footprint.readWrite = {contractCodeLedgerKey};

    // submit operation
    auto tx = sorobanTransactionFrameFromOps(app->getNetworkID(), a1,
                                             {uploadOp}, {}, uploadResources,
                                             100, DEFAULT_TEST_RESOURCE_FEE);

    auto mergeOp = accountMerge(b1);
    mergeOp.sourceAccount.activate() = toMuxedAccount(a1);

    auto classicMergeTx = c1.tx({mergeOp});
    classicMergeTx->addSignature(a1.getSecretKey());

    auto r = closeLedger(*app, {classicMergeTx, tx});
    checkTx(0, r, txSUCCESS);

    // The source account of the soroban tx was merged during the classic phase
    checkTx(1, r, txNO_ACCOUNT);
}

TEST_CASE("buying liabilities plus refund is greater than INT64_MAX",
          "[tx][soroban][offer]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    auto app = createTestApplication(clock, cfg);
    overrideSorobanNetworkConfigForTest(*app);

    auto root = TestAccount::createRoot(*app);
    int64_t initBalance = root.getBalance();

    const int64_t startingBalance =
        app->getLedgerManager().getLastMinBalance(50);

    auto a1 = root.create("A", startingBalance);
    auto b1 = root.create("B", startingBalance);

    auto native = txtest::makeNativeAsset();
    auto cur1 = txtest::makeAsset(root, "CUR1");
    a1.changeTrust(cur1, INT64_MAX);
    root.pay(a1, cur1, INT64_MAX);

    auto const addI32Wasm = rust_bridge::get_test_wasm_add_i32();

    SorobanResources uploadResources{};
    uploadResources.instructions = 200'000 + (addI32Wasm.data.size() * 6000);
    uploadResources.readBytes = 1000;
    uploadResources.writeBytes = 5000;

    // Upload contract code
    Operation uploadOp;
    uploadOp.body.type(INVOKE_HOST_FUNCTION);
    auto& uploadHF = uploadOp.body.invokeHostFunctionOp().hostFunction;
    uploadHF.type(HOST_FUNCTION_TYPE_UPLOAD_CONTRACT_WASM);
    uploadHF.wasm().assign(addI32Wasm.data.begin(), addI32Wasm.data.end());

    LedgerKey contractCodeLedgerKey;
    contractCodeLedgerKey.type(CONTRACT_CODE);
    contractCodeLedgerKey.contractCode().hash = sha256(uploadHF.wasm());
    uploadResources.footprint.readWrite = {contractCodeLedgerKey};

    auto a1PreBalance = a1.getBalance();
    // submit operation
    auto tx =
        sorobanTransactionFrameFromOps(app->getNetworkID(), a1, {uploadOp}, {},
                                       uploadResources, 10'000, 300'000);
    // There is no surge pricing, so only 100 stroops are charged from the
    // inclusion fee.
    auto feeChargedBeforeRefund =
        tx->getFullFee() - tx->getInclusionFee() + 100;
    // Create a maxmal possible offer that wouldn't overflow int64. The offer
    // is created *after* charging the transaction fee, so the amount is higher
    // than what it would be before fees are charged.
    auto offer =
        manageOffer(0, cur1, native, Price{1, 1},
                    INT64_MAX - (a1PreBalance - feeChargedBeforeRefund));
    offer.sourceAccount.activate() = toMuxedAccount(a1);

    // Create an offer on behalf of `a1`, but paid for from `b1`, so that
    // the offer fee doesn't need to be accounted for in fee computations.
    auto offerTx = b1.tx({offer});
    offerTx->addSignature(a1.getSecretKey());

    auto r = closeLedger(*app, {offerTx, tx});
    checkTx(0, r, txSUCCESS);
    checkTx(1, r, txSUCCESS);

    // After an offer is created, the sum of `a1` balance and buying liability
    // is `INT64_MAX`, thus any fee refund would cause an overflow, so no
    // refunds happen.
    REQUIRE(a1PreBalance - feeChargedBeforeRefund == a1.getBalance());
}

TEST_CASE("contract storage", "[tx][soroban]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    overrideSorobanNetworkConfigForTest(*app);
    auto root = TestAccount::createRoot(*app);
    auto acc = root.create("acc", app->getLedgerManager().getLastMinBalance(1));
    auto dummyAcc =
        root.create("dummyAcc", app->getLedgerManager().getLastMinBalance(1));
    root.setSequenceNumber(root.getLastSequenceNumber() - 2);
    auto const contractDataWasm = rust_bridge::get_test_wasm_contract_data();
    auto contractKeys = deployContractWithSourceAccount(*app, contractDataWasm);
    auto const& contractID = contractKeys[0].contractData().contract;
    LedgerTxn ltxCfg(app->getLedgerTxnRoot());
    SorobanNetworkConfig const& sorobanConfig =
        app->getLedgerManager().getSorobanNetworkConfig(ltxCfg);
    auto const& stateArchivalSettings = sorobanConfig.stateArchivalSettings();
    ltxCfg.commit();
    auto checkContractData = [&](SCVal const& key, ContractDataDurability type,
                                 SCVal const* val) {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        auto ltxe = loadContractData(ltx, contractID, key, type);
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

    auto createTx =
        [&](xdr::xvector<LedgerKey> const& readOnly,
            xdr::xvector<LedgerKey> const& readWrite, uint32_t writeBytes,
            SCAddress const& contractAddress, SCSymbol const& functionName,
            std::vector<SCVal> const& args,
            std::optional<uint32_t> refundableFeeOverride = std::nullopt)
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
        resources.readBytes = 10'000;
        resources.writeBytes = writeBytes;

        // Compute tx frame size before finalizing the resource fee.
        // That's a bit hacky, but we need the exact tx size here in order to
        // enable tests that rely on the exact refundable fee value.
        auto dummyTx = sorobanTransactionFrameFromOps(
            app->getNetworkID(), dummyAcc, {op}, {}, resources, 1000, 123);
        auto txSize = xdr::xdr_size(dummyTx->getEnvelope());
        uint32_t resourceFee = sorobanResourceFee(*app, resources, txSize, 0);
        if (refundableFeeOverride)
        {
            resourceFee += *refundableFeeOverride;
        }
        else
        {
            resourceFee += 40'000;
        }

        auto tx = sorobanTransactionFrameFromOps(
            app->getNetworkID(), root, {op}, {}, resources, 1000, resourceFee);
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
        auto valU64 = makeU64SCVal(val);

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
        putWithFootprint(
            key, val, contractKeys,
            {contractDataKey(contractID, makeSymbolSCVal(key), type)}, 1000,
            true, type);
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
        readOnly.emplace_back(
            contractDataKey(contractID, makeSymbolSCVal(key), type));
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
        getWithFootprint(
            key, contractKeys,
            {contractDataKey(contractID, makeSymbolSCVal(key), type)}, 1000,
            expectSuccess, type);
    };

    auto extendWithFootprint = [&](std::string const& key, uint32_t lowLifetime,
                                   uint32_t highLifetime,
                                   xdr::xvector<LedgerKey> const& readOnly,
                                   xdr::xvector<LedgerKey> const& readWrite,
                                   bool expectSuccess,
                                   ContractDataDurability type) {
        auto keySymbol = makeSymbolSCVal(key);
        auto lowLifetimeU32 = makeU32(lowLifetime);
        auto highLifetimeU32 = makeU32(highLifetime);

        std::string funcStr;
        switch (type)
        {
        case ContractDataDurability::TEMPORARY:
            funcStr = "extend_temporary";
            break;
        case ContractDataDurability::PERSISTENT:
            funcStr = "extend_persistent";
            break;
        }

        // TODO: Better bytes to write value
        auto [tx, ltx, txm] =
            createTx(readOnly, readWrite, 1000, contractID, makeSymbol(funcStr),
                     {keySymbol, lowLifetimeU32, highLifetimeU32});

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

    auto extendLowHigh = [&](std::string const& key,
                             ContractDataDurability type, uint32_t lowLifetime,
                             uint32_t highLifetime) {
        extendWithFootprint(
            key, lowLifetime, highLifetime, contractKeys,
            {contractDataKey(contractID, makeSymbolSCVal(key), type)}, true,
            type);
    };

    auto extendExact = [&](std::string const& key, ContractDataDurability type,
                           uint32_t extendAmount) {
        extendLowHigh(key, type, extendAmount, extendAmount);
    };

    auto increaseEntrySizeAndExtend =
        [&](std::string const& key, uint32_t numKiloBytes, uint32_t lowLifetime,
            uint32_t highLifetime, xdr::xvector<LedgerKey> const& readOnly,
            xdr::xvector<LedgerKey> const& readWrite, bool expectSuccess,
            uint32_t refundableFee) {
            auto keySymbol = makeSymbolSCVal(key);
            auto numKiloBytesU32 = makeU32(numKiloBytes);
            auto lowLifetimeU32 = makeU32(lowLifetime);
            auto highLifetimeU32 = makeU32(highLifetime);

            std::string funcStr = "replace_with_bytes_and_extend";

            // TODO: Better bytes to write value
            auto [tx, ltx, txm] = createTx(
                readOnly, readWrite, 2000, contractID, makeSymbol(funcStr),
                {keySymbol, numKiloBytesU32, lowLifetimeU32, highLifetimeU32},
                refundableFee);

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

    auto runStateArchivalOp = [&](TestAccount& root, TransactionFrameBasePtr tx,
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
        auto actuallyCharged =
            baseCharged + /* surge pricing additional fee */ 200;
        REQUIRE(initBalance - balanceAfterFeeCharged == actuallyCharged);
        auto nonRefundableResourceFee = sorobanResourceFee(
            *app, tx->sorobanResources(), xdr::xdr_size(tx->getEnvelope()), 0);
        auto expectedChargedAfterRefund =
            nonRefundableResourceFee + expectedRefundableFeeCharged + 300;
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
                    actuallyCharged - expectedChargedAfterRefund);
        }

        // The account should receive a refund for unspent refundable fee.
        REQUIRE(root.getBalance() - balanceAfterFeeCharged ==
                actuallyCharged - expectedChargedAfterRefund);
    };

    auto extendOp = [&](uint32_t extendAmount,
                        xdr::xvector<LedgerKey> const& readOnly) {
        int64_t expectedRefundableFeeCharged = 0;
        for (auto const& key : readOnly)
        {
            expectedRefundableFeeCharged +=
                getRentFeeForExtend(*app, key, extendAmount, sorobanConfig);
        }

        Operation extendOp;
        extendOp.body.type(EXTEND_FOOTPRINT_TTL);
        extendOp.body.extendFootprintTTLOp().extendTo = extendAmount;

        SorobanResources extendResources;
        extendResources.footprint.readOnly = readOnly;
        extendResources.instructions = 0;
        extendResources.readBytes = 10'000;
        extendResources.writeBytes = 0;

        auto tx = sorobanTransactionFrameFromOps(
            app->getNetworkID(), root, {extendOp}, {}, extendResources, 1000,
            DEFAULT_TEST_RESOURCE_FEE);

        runStateArchivalOp(root, tx, expectedRefundableFeeCharged);
    };

    auto restoreOp = [&](xdr::xvector<LedgerKey> const& readWrite,
                         int64_t expectedRefundableFeeCharged) {
        Operation restoreOp;
        restoreOp.body.type(RESTORE_FOOTPRINT);

        SorobanResources resources;
        resources.footprint.readWrite = readWrite;
        resources.instructions = 0;
        resources.readBytes = 10'000;
        resources.writeBytes = 10'000;

        // submit operation
        auto tx = sorobanTransactionFrameFromOps(
            app->getNetworkID(), root, {restoreOp}, {}, resources, 1000,
            300'000 + 40'000 * readWrite.size());
        runStateArchivalOp(root, tx, expectedRefundableFeeCharged);
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
        delWithFootprint(
            key, contractKeys,
            {contractDataKey(contractID, makeSymbolSCVal(key), type)}, true,
            type);
    };

    auto checkContractDataArchivalState =
        [&](std::string const& key, ContractDataDurability type,
            uint32_t ledgerSeq, bool expectedIsLive) {
            auto le = loadStorageEntry(*app, contractID, key, type);
            auto ttlKey = getTTLKey(le);

            {
                LedgerTxn ltx(app->getLedgerTxnRoot());
                auto ttlLtxe = ltx.loadWithoutRecord(ttlKey);
                REQUIRE(ttlLtxe);
                REQUIRE(isLive(ttlLtxe.current(), ledgerSeq) == expectedIsLive);
            }

            // Make sure entry is accessible/inaccessible
            get(key, type, expectedIsLive);
        };

    // Check entries existence and ttl
    auto checkEntry = [&](LedgerKey const& key,
                          uint32_t expectedLiveUntilLedgerLedger) {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        auto ltxe = ltx.loadWithoutRecord(key);
        REQUIRE(ltxe);
        checkTTL(ltx, key, expectedLiveUntilLedgerLedger);
    };

    auto checkContractDataLiveUntilLedger =
        [&](std::string const& key, ContractDataDurability type,
            uint32_t expectedLiveUntilLedgerLedger) {
            auto le = loadStorageEntry(*app, contractID, key, type);
            checkEntry(LedgerEntryKey(le), expectedLiveUntilLedgerLedger);
        };

    auto ledgerSeq = getLedgerSeq(*app);

    SECTION("default limits")
    {
        put("key1", 0, ContractDataDurability::PERSISTENT);
        put("key2", 21, ContractDataDurability::PERSISTENT);

        // Failure: insufficient write bytes
        putWithFootprint("key2", 88888, contractKeys,
                         {contractDataKey(contractID, makeSymbolSCVal("key2"),
                                          ContractDataDurability::PERSISTENT)},
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
        putWithFootprint("key2", 2, contractKeys,
                         {contractDataKey(contractID, makeSymbolSCVal("key2"),
                                          ContractDataDurability::PERSISTENT)},
                         1000, false, ContractDataDurability::PERSISTENT);
    }

    SECTION("Same ScVal key, different types")
    {
        // Check that each type is in their own keyspace
        uint64_t uniqueVal = 0;
        uint64_t temporaryVal = 2;

        put("key", uniqueVal, ContractDataDurability::PERSISTENT);
        put("key", temporaryVal, TEMPORARY);

        auto uniqueScVal = makeU64SCVal(uniqueVal);
        auto temporaryScVal = makeU64SCVal(temporaryVal);
        auto keySymbol = makeSymbolSCVal("key");

        checkContractData(keySymbol, ContractDataDurability::PERSISTENT,
                          &uniqueScVal);
        checkContractData(keySymbol, TEMPORARY, &temporaryScVal);

        put("key2", 3, ContractDataDurability::PERSISTENT);
        auto key2Symbol = makeSymbolSCVal("key2");
        auto uniqueScVal2 = makeU64SCVal(3);

        checkContractData(key2Symbol, ContractDataDurability::PERSISTENT,
                          &uniqueScVal2);
        checkContractData(key2Symbol, TEMPORARY, nullptr);
    }

    SECTION("contract instance and wasm ttl")
    {
        uint32_t originalexpectedLiveUntilLedger =
            stateArchivalSettings.minPersistentTTL + ledgerSeq - 1;

        for (uint32_t i = app->getLedgerManager().getLastClosedLedgerNum();
             i <= originalexpectedLiveUntilLedger + 1; ++i)
        {
            closeLedgerOn(*app, i, 2, 1, 2016);
        }

        // Contract instance and code are expired, an TX should fail
        putWithFootprint(
            "temp", 0, contractKeys,
            {contractDataKey(contractID, makeSymbolSCVal("temp"), TEMPORARY)},
            1000, /*expectSuccess*/ false, ContractDataDurability::TEMPORARY);

        ledgerSeq = getLedgerSeq(*app);
        auto newexpectedLiveUntilLedger =
            stateArchivalSettings.minPersistentTTL + ledgerSeq - 1;

        SECTION("restore contract instance and wasm")
        {
            // Restore Instance and WASM
            restoreOp(contractKeys,
                      96 /* rent bump */ + 40000 /* two LE-writes */);

            // Instance should now be useable
            putWithFootprint(
                "temp", 0, contractKeys,
                {contractDataKey(contractID, makeSymbolSCVal("temp"),
                                 TEMPORARY)},
                1000, /*expectSuccess*/ true,
                ContractDataDurability::TEMPORARY);

            checkEntry(contractKeys[0], newexpectedLiveUntilLedger);
            checkEntry(contractKeys[1], newexpectedLiveUntilLedger);
        }

        SECTION("restore contract instance, not wasm")
        {
            // Only restore contract instance
            restoreOp({contractKeys[0]},
                      48 /* rent bump */ + 20000 /* one LE write */);

            // invocation should fail
            putWithFootprint(
                "temp", 0, contractKeys,
                {contractDataKey(contractID, makeSymbolSCVal("temp"),
                                 TEMPORARY)},
                1000, /*expectSuccess*/ false,
                ContractDataDurability::TEMPORARY);

            checkEntry(contractKeys[0], newexpectedLiveUntilLedger);
            checkEntry(contractKeys[1], originalexpectedLiveUntilLedger);
        }

        SECTION("restore contract wasm, not instance")
        {
            // Only restore WASM
            restoreOp({contractKeys[1]},
                      48 /* rent bump */ + 20000 /* one LE write */);

            // invocation should fail
            putWithFootprint(
                "temp", 0, contractKeys,
                {contractDataKey(contractID, makeSymbolSCVal("temp"),
                                 TEMPORARY)},
                1000, /*expectSuccess*/ false,
                ContractDataDurability::TEMPORARY);

            checkEntry(contractKeys[0], originalexpectedLiveUntilLedger);
            checkEntry(contractKeys[1], newexpectedLiveUntilLedger);
        }

        SECTION("lifetime extensions")
        {
            // Restore Instance and WASM
            restoreOp(contractKeys,
                      96 /* rent bump */ + 40000 /* two LE writes */);

            auto instanceExtendAmount = 10'000;
            auto wasmExtendAmount = 15'000;

            // extend instance
            extendOp(instanceExtendAmount, {contractKeys[0]});

            // extend WASM
            extendOp(wasmExtendAmount, {contractKeys[1]});

            checkEntry(contractKeys[0], ledgerSeq + instanceExtendAmount);
            checkEntry(contractKeys[1], ledgerSeq + wasmExtendAmount);
        }
    }

    SECTION("contract storage ttl")
    {
        put("unique", 0, ContractDataDurability::PERSISTENT);
        put("temp", 0, TEMPORARY);

        auto expectedTempLiveUntilLedger =
            stateArchivalSettings.minTemporaryTTL + ledgerSeq - 1;
        auto expectedPersistentLiveUntilLedger =
            stateArchivalSettings.minPersistentTTL + ledgerSeq - 1;

        // Check for expected minimum lifetime values
        checkContractDataLiveUntilLedger("unique",
                                         ContractDataDurability::PERSISTENT,
                                         expectedPersistentLiveUntilLedger);
        checkContractDataLiveUntilLedger("temp", TEMPORARY,
                                         expectedTempLiveUntilLedger);

        // Close ledgers until temp entry expires
        uint32 currLedger = app->getLedgerManager().getLastClosedLedgerNum();
        for (; currLedger <= expectedTempLiveUntilLedger + 1; ++currLedger)
        {
            closeLedgerOn(*app, currLedger, 2, 1, 2016);
        }
        REQUIRE(app->getLedgerManager().getLastClosedLedgerNum() ==
                expectedTempLiveUntilLedger + 1);

        // Check that temp entry has expired
        checkContractDataArchivalState(
            "temp", TEMPORARY, app->getLedgerManager().getLastClosedLedgerNum(),
            false);

        checkContractDataArchivalState(
            "unique", ContractDataDurability::PERSISTENT,
            app->getLedgerManager().getLastClosedLedgerNum(), true);

        // Check that we can recreate an expired TEMPORARY entry
        putWithFootprint(
            "temp", 0, contractKeys,
            {contractDataKey(contractID, makeSymbolSCVal("temp"), TEMPORARY)},
            1000, /*expectSuccess*/ true, ContractDataDurability::TEMPORARY);

        // Recreated entry should be live
        ledgerSeq = getLedgerSeq(*app);
        checkContractDataArchivalState(
            "temp", TEMPORARY, app->getLedgerManager().getLastClosedLedgerNum(),
            true);
        checkContractDataLiveUntilLedger("temp", TEMPORARY,
                                         stateArchivalSettings.minTemporaryTTL +
                                             ledgerSeq - 1);

        // Close ledgers until PERSISTENT entry expires
        for (; currLedger <= expectedPersistentLiveUntilLedger + 1;
             ++currLedger)
        {
            closeLedgerOn(*app, currLedger, 2, 1, 2016);
        }

        REQUIRE(app->getLedgerManager().getLastClosedLedgerNum() ==
                expectedPersistentLiveUntilLedger + 1);
        checkContractDataArchivalState(
            "unique", ContractDataDurability::PERSISTENT,
            app->getLedgerManager().getLastClosedLedgerNum(), false);

        // Check that we can't recreate expired PERSISTENT
        putWithFootprint("unique", 0, contractKeys,
                         {contractDataKey(contractID, makeSymbolSCVal("unique"),
                                          ContractDataDurability::PERSISTENT)},
                         1000, /*expectSuccess*/ false,
                         ContractDataDurability::PERSISTENT);
    }

    SECTION("manual extend")
    {
        // Large extend, followed by smaller extend
        put("key", 0, ContractDataDurability::PERSISTENT);
        extendExact("key", ContractDataDurability::PERSISTENT, 10'000);
        checkContractDataLiveUntilLedger(
            "key", ContractDataDurability::PERSISTENT, ledgerSeq + 10'000);

        // TTL already above 5'000, should be a no-op
        extendExact("key", ContractDataDurability::PERSISTENT, 5'000);
        checkContractDataLiveUntilLedger(
            "key", ContractDataDurability::PERSISTENT, ledgerSeq + 10'000);

        // Small extend followed by larger extend
        put("key2", 0, ContractDataDurability::PERSISTENT);
        extendExact("key2", ContractDataDurability::PERSISTENT, 5'000);
        checkContractDataLiveUntilLedger(
            "key2", ContractDataDurability::PERSISTENT, ledgerSeq + 5'000);

        put("key3", 0, ContractDataDurability::PERSISTENT);
        extendExact("key3", ContractDataDurability::PERSISTENT, 50'000);
        checkContractDataLiveUntilLedger(
            "key3", ContractDataDurability::PERSISTENT, ledgerSeq + 50'000);

        // Extend multiple keys to live 10100 ledger from now
        extendOp(
            10100,
            {contractDataKey(contractID, makeSymbolSCVal("key"),
                             ContractDataDurability::PERSISTENT),
             contractDataKey(contractID, makeSymbolSCVal("key2"),
                             ContractDataDurability::PERSISTENT),
             contractDataKey(
                 contractID, makeSymbolSCVal("key3"),
                 ContractDataDurability::PERSISTENT)}); // only 2 ledger writes
                                                        // because key3 won't be
                                                        // extended

        checkContractDataLiveUntilLedger(
            "key", ContractDataDurability::PERSISTENT, ledgerSeq + 10'100);
        checkContractDataLiveUntilLedger(
            "key2", ContractDataDurability::PERSISTENT, ledgerSeq + 10'100);

        // No change for key3 since ttl is already past 10100 ledgers
        checkContractDataLiveUntilLedger(
            "key3", ContractDataDurability::PERSISTENT, ledgerSeq + 50'000);

        SECTION("low/high watermark")
        {
            uint32_t initialLiveUntilLedger =
                stateArchivalSettings.minPersistentTTL + ledgerSeq - 1;

            put("key4", 0, ContractDataDurability::PERSISTENT);

            // After the put op, key4's lifetime will be minimumLifetime - 1.
            // Set low lifetime to minimumLifetime - 2 so extend does not occur
            extendLowHigh("key4", ContractDataDurability::PERSISTENT,
                          stateArchivalSettings.minPersistentTTL - 2, 50'000);
            checkContractDataLiveUntilLedger("key4",
                                             ContractDataDurability::PERSISTENT,
                                             initialLiveUntilLedger);

            // Close one ledger
            ledgerSeq = app->getLedgerManager().getLastClosedLedgerNum();
            closeLedgerOn(*app, ledgerSeq + 1, 2, 1, 2016);
            ++ledgerSeq;

            // Lifetime is now at low threshold, should be extended
            extendLowHigh("key4", ContractDataDurability::PERSISTENT,
                          stateArchivalSettings.minPersistentTTL - 2, 50'000);
            checkContractDataLiveUntilLedger(
                "key4", ContractDataDurability::PERSISTENT, 50'000 + ledgerSeq);

            // Check that low watermark > high watermark fails
            extendWithFootprint(
                "key4", 60'000, 50'000, contractKeys,
                {contractDataKey(contractID, makeSymbolSCVal("key4"),
                                 ContractDataDurability::PERSISTENT)},
                false, ContractDataDurability::PERSISTENT);
        }
    }

    SECTION("restore expired entry")
    {
        uint32_t initLiveUntilLedger =
            stateArchivalSettings.minPersistentTTL + ledgerSeq - 1;

        // Extend instance and WASM so that they don't expire during the test
        extendOp(10'000, contractKeys);

        put("key", 0, ContractDataDurability::PERSISTENT);
        checkContractDataLiveUntilLedger(
            "key", ContractDataDurability::PERSISTENT, initLiveUntilLedger);

        // Crank until entry expires
        for (uint32 i = app->getLedgerManager().getLastClosedLedgerNum();
             i <= initLiveUntilLedger + 1; ++i)
        {
            closeLedgerOn(*app, i, 2, 1, 2016);
        }

        REQUIRE(app->getLedgerManager().getLastClosedLedgerNum() ==
                initLiveUntilLedger + 1);

        checkContractDataArchivalState(
            "key", ContractDataDurability::PERSISTENT,
            app->getLedgerManager().getLastClosedLedgerNum(), false);

        auto lk = contractDataKey(contractID, makeSymbolSCVal("key"),
                                  ContractDataDurability::PERSISTENT);
        auto roKeys = contractKeys;
        roKeys.emplace_back(lk);

        // Trying to read this expired entry should fail.
        getWithFootprint("key", roKeys, {}, 1000, /*expectSuccess*/ false,
                         ContractDataDurability::PERSISTENT);

        // Recreation of persistent entries should fail.
        putWithFootprint("key", 0, contractKeys, {lk}, 1000,
                         /*expectSuccess*/ false,
                         ContractDataDurability::PERSISTENT);

        // Extend operation should skip expired entries
        extendOp(1'000, {lk});

        // Make sure liveUntilLedger is unchanged by extendOp
        checkContractDataLiveUntilLedger(
            "key", ContractDataDurability::PERSISTENT, initLiveUntilLedger);

        // Restore the entry
        restoreOp({lk}, 20048);

        ledgerSeq = getLedgerSeq(*app);
        checkContractDataArchivalState(
            "key", ContractDataDurability::PERSISTENT, ledgerSeq, true);

        // Entry is considered to be restored on lclNum (as we didn't close an
        // additional ledger).
        checkContractDataLiveUntilLedger(
            "key", ContractDataDurability::PERSISTENT,
            ledgerSeq + stateArchivalSettings.minPersistentTTL - 1);

        // Write to entry to check that is is live
        put("key", 1, ContractDataDurability::PERSISTENT);
    }

    SECTION("re-create expired temporary entry")
    {
        auto minExtend =
            InitialSorobanNetworkConfig::MINIMUM_TEMP_ENTRY_LIFETIME;
        put("key", 0, ContractDataDurability::TEMPORARY);
        REQUIRE(has("key", ContractDataDurability::TEMPORARY));

        uint32_t initLiveUntilLedger = ledgerSeq + minExtend - 1;
        checkContractDataLiveUntilLedger(
            "key", ContractDataDurability::TEMPORARY, initLiveUntilLedger);

        for (size_t i = app->getLedgerManager().getLastClosedLedgerNum();
             i <= initLiveUntilLedger + 1; ++i)
        {
            closeLedgerOn(*app, i, 2, 1, 2016);
        }
        REQUIRE(app->getLedgerManager().getLastClosedLedgerNum() ==
                initLiveUntilLedger + 1);
        checkContractDataArchivalState(
            "key", ContractDataDurability::TEMPORARY,
            app->getLedgerManager().getLastClosedLedgerNum(), false);

        // Entry has expired
        REQUIRE(!has("key", ContractDataDurability::TEMPORARY));

        // We can recreate an expired temp entry
        put("key", 0, ContractDataDurability::TEMPORARY);
        REQUIRE(has("key", ContractDataDurability::TEMPORARY));

        uint32_t newLiveUntilLedger =
            app->getLedgerManager().getLastClosedLedgerNum() + minExtend - 1;
        checkContractDataLiveUntilLedger(
            "key", ContractDataDurability::TEMPORARY, newLiveUntilLedger);
    }

    SECTION("charge rent fees for storage resize")
    {
        auto resizeKilobytes = 1;
        put("key1", 0, ContractDataDurability::PERSISTENT);
        auto key1lk =
            contractDataKey(contractID, makeSymbolSCVal("key1"), PERSISTENT);

        auto startingSizeBytes = 0;
        {
            LedgerTxn ltx(app->getLedgerTxnRoot());
            auto txle = ltx.loadWithoutRecord(key1lk);
            REQUIRE(txle);
            startingSizeBytes = xdr::xdr_size(txle.current());
        }

        // First, resize a key with large fee to guarantee success
        increaseEntrySizeAndExtend("key1", resizeKilobytes, 0, 0, contractKeys,
                                   {key1lk}, true, 40'000);

        auto sizeDeltaBytes = 0;
        {
            LedgerTxn ltx(app->getLedgerTxnRoot());
            auto txle = ltx.loadWithoutRecord(key1lk);
            REQUIRE(txle);
            sizeDeltaBytes = xdr::xdr_size(txle.current()) - startingSizeBytes;
        }

        // Now that we know the size delta, we can calculate the expected rent
        // and check that another entry resize charges fee correctly
        put("key2", 0, ContractDataDurability::PERSISTENT);

        auto initialLifetime = stateArchivalSettings.minPersistentTTL;

        SECTION("resize with no extend")
        {
            auto expectedRentFee = getRentFeeForBytes(
                sizeDeltaBytes, initialLifetime, sorobanConfig,
                /*isPersistent=*/true);

            // resourceFee = rent fee + (event size + return val) fee. So
            // in order to succeed resourceFee needs to be expectedRentFee
            // + 1 to account for the return val size. We are not changing the
            // liveUntilLedgerSeq, so there is no TTLEntry write charge
            auto refundableFee = expectedRentFee + 1;
            increaseEntrySizeAndExtend(
                "key2", resizeKilobytes, 0, 0, contractKeys,
                {contractDataKey(contractID, makeSymbolSCVal("key2"),
                                 PERSISTENT)},
                false, refundableFee - 1);

            // Size change should succeed with enough refundable fee
            increaseEntrySizeAndExtend(
                "key2", resizeKilobytes, 0, 0, contractKeys,
                {contractDataKey(contractID, makeSymbolSCVal("key2"),
                                 PERSISTENT)},
                true, refundableFee);
        }

        SECTION("resize and extend")
        {
            auto newLifetime = initialLifetime + 1000;

            // New bytes are charged rent for previous lifetime and new lifetime
            auto resizeRentFee =
                getRentFeeForBytes(sizeDeltaBytes, newLifetime, sorobanConfig,
                                   /*isPersistent=*/true);

            // Initial bytes just charged for new lifetime
            auto extendFee = getRentFeeForBytes(
                startingSizeBytes, newLifetime - initialLifetime, sorobanConfig,
                /*isPersistent=*/true);

            // resourceFee = rent fee + (event size + return val) fee. So in
            // order to success resourceFee needs to be expectedRentFee
            // + 1 to account for the return val size.
            auto refundableFee = resizeRentFee + extendFee +
                                 getTTLEntryWriteFee(sorobanConfig) + 1;
            increaseEntrySizeAndExtend(
                "key2", resizeKilobytes, newLifetime, newLifetime, contractKeys,
                {contractDataKey(contractID, makeSymbolSCVal("key2"),
                                 PERSISTENT)},
                false, refundableFee - 1);

            // Size change should succeed with enough refundable fee
            increaseEntrySizeAndExtend(
                "key2", resizeKilobytes, newLifetime, newLifetime, contractKeys,
                {contractDataKey(contractID, makeSymbolSCVal("key2"),
                                 PERSISTENT)},
                true, refundableFee);
        }
    }

    SECTION("max ttl")
    {
        // Check that attempting to extend past max ledger results in error
        put("key", 0, ContractDataDurability::PERSISTENT);
        extendWithFootprint(
            "key", UINT32_MAX, UINT32_MAX, contractKeys,
            {contractDataKey(contractID, makeSymbolSCVal("key"),
                             ContractDataDurability::PERSISTENT)},
            false, ContractDataDurability::PERSISTENT);

        // Make sure maximum extend succeeds
        extendExact("key", ContractDataDurability::PERSISTENT,
                    stateArchivalSettings.maxEntryTTL - 1);

        auto maxLiveUntilLedger =
            ledgerSeq + stateArchivalSettings.maxEntryTTL - 1;
        checkContractDataLiveUntilLedger(
            "key", ContractDataDurability::PERSISTENT, maxLiveUntilLedger);
    }
    SECTION("footprint tests")
    {
        put("key1", 0, ContractDataDurability::PERSISTENT);
        put("key2", 21, ContractDataDurability::PERSISTENT);
        SECTION("unused readWrite key")
        {
            putWithFootprint(
                "key1", 0, contractKeys,
                {contractDataKey(contractID, makeSymbolSCVal("key1"),
                                 ContractDataDurability::PERSISTENT),
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
            readOnlyFootprint.push_back(
                contractDataKey(contractID, makeSymbolSCVal("key2"),
                                ContractDataDurability::PERSISTENT));
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
    overrideSorobanNetworkConfigForTest(*app);

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
        resources.readBytes = 10'000;
        resources.writeBytes = writeBytes;

        auto tx = sorobanTransactionFrameFromOps(
            app->getNetworkID(), root, {op}, {}, resources, 1000, 340'000);
        LedgerTxn ltx(app->getLedgerTxnRoot());
        REQUIRE(tx->checkValid(*app, ltx, 0, 0, 0));
        return tx;
    };

    auto putWithFootprint = [&](std::string const& key, uint64_t val,
                                xdr::xvector<LedgerKey> const& readOnly,
                                xdr::xvector<LedgerKey> const& readWrite,
                                uint32_t writeBytes, bool expectSuccess,
                                ContractDataDurability type) {
        auto keySymbol = makeSymbolSCVal(key);
        auto valU64 = makeU64SCVal(val);

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

    auto checkContractDataArchivalState =
        [&](std::string const& key, ContractDataDurability type,
            uint32_t ledgerSeq, bool expectedIsLive) {
            auto le = loadStorageEntry(*app, contractID, key, type);
            auto ttlKey = getTTLKey(le);
            LedgerTxn ltx(app->getLedgerTxnRoot());
            auto ttlLtxe = ltx.loadWithoutRecord(ttlKey);
            REQUIRE(ttlLtxe);
            REQUIRE(isLive(ttlLtxe.current(), ledgerSeq) == expectedIsLive);
        };

    auto lk = contractDataKey(contractID, makeSymbolSCVal("temp"), TEMPORARY);
    putWithFootprint("temp", 0, contractKeys, {lk}, 1000, true, TEMPORARY);
    // Check that entry exists and is live.
    checkContractDataArchivalState(
        "temp", TEMPORARY, app->getLedgerManager().getLastClosedLedgerNum(),
        true);
    // Close ledgers until temp entry is evicted
    uint32 currLedger = app->getLedgerManager().getLastClosedLedgerNum();
    for (; currLedger <= 4096; ++currLedger)
    {
        closeLedgerOn(*app, currLedger, 2, 1, 2016);
    }

    // Check that temp entry has expired
    checkContractDataArchivalState(
        "temp", TEMPORARY, app->getLedgerManager().getLastClosedLedgerNum(),
        false);

    // close one more ledger to trigger the eviction
    closeLedgerOn(*app, 4097, 2, 1, 2016);

    XDRInputFileStream in;
    in.open(metaPath);
    LedgerCloseMeta lcm;
    bool evicted = false;
    while (in.readOne(lcm))
    {
        REQUIRE(lcm.v() == 1);
        if (lcm.v1().ledgerHeader.header.ledgerSeq == 4097)
        {
            REQUIRE(lcm.v1().evictedTemporaryLedgerKeys.size() == 2);
            auto sortedKeys = lcm.v1().evictedTemporaryLedgerKeys;
            std::sort(sortedKeys.begin(), sortedKeys.end());
            REQUIRE(sortedKeys[0] == lk);
            REQUIRE(sortedKeys[1] == getTTLKey(lk));
            evicted = true;
        }
        else
        {
            REQUIRE(lcm.v1().evictedTemporaryLedgerKeys.empty());
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
                                             {}, resources, 100,
                                             DEFAULT_TEST_RESOURCE_FEE);
    LedgerTxn ltx(app->getLedgerTxnRoot());
    TransactionMetaFrame txm(ltx.loadHeader().current().ledgerVersion);
    REQUIRE(tx->checkValid(*app, ltx, 0, 0, 0));
    REQUIRE(!tx->apply(*app, ltx, txm));
    ltx.commit();

    auto const& opEvents = txm.getXDR().v3().sorobanMeta->diagnosticEvents;
    REQUIRE(opEvents.size() == 23);

    auto const& callEv = opEvents.at(0);
    REQUIRE(!callEv.inSuccessfulContractCall);
    REQUIRE(callEv.event.type == ContractEventType::DIAGNOSTIC);
    REQUIRE(callEv.event.body.v0().data.type() == SCV_VEC);

    auto const& contract_ev = opEvents.at(1);
    REQUIRE(!contract_ev.inSuccessfulContractCall);
    REQUIRE(contract_ev.event.type == ContractEventType::CONTRACT);
    REQUIRE(contract_ev.event.body.v0().data.type() == SCV_VEC);

    auto const& hostFnErrorEv = opEvents.at(3);
    REQUIRE(!hostFnErrorEv.inSuccessfulContractCall);
    REQUIRE(hostFnErrorEv.event.type == ContractEventType::DIAGNOSTIC);
    auto const& hostFnErrorBody = hostFnErrorEv.event.body.v0();
    SCError expectedError(SCE_WASM_VM);
    expectedError.code() = SCEC_INVALID_ACTION;
    REQUIRE(hostFnErrorBody.topics.size() == 2);
    REQUIRE(hostFnErrorBody.topics.at(0).sym() == "host_fn_failed");
    REQUIRE(hostFnErrorBody.topics.at(1).error() == expectedError);

    auto const& readEntryEv = opEvents.at(4);
    REQUIRE(!readEntryEv.inSuccessfulContractCall);
    REQUIRE(readEntryEv.event.type == ContractEventType::DIAGNOSTIC);
    auto const& readEntryEvBody = readEntryEv.event.body.v0();
    REQUIRE(readEntryEvBody.topics.size() == 2);
    REQUIRE(readEntryEvBody.topics.at(0).sym() == "core_metrics");
    REQUIRE(readEntryEvBody.topics.at(1).sym() == "read_entry");
    REQUIRE(readEntryEvBody.data.type() == SCV_U64);
    REQUIRE(readEntryEvBody.data.u64() == 2);

    auto const& cpuInsnEv = opEvents.at(16);
    REQUIRE(!cpuInsnEv.inSuccessfulContractCall);
    REQUIRE(cpuInsnEv.event.type == ContractEventType::DIAGNOSTIC);
    auto const& cpuInsnEvBody = cpuInsnEv.event.body.v0();
    REQUIRE(cpuInsnEvBody.topics.size() == 2);
    REQUIRE(cpuInsnEvBody.topics.at(0).sym() == "core_metrics");
    REQUIRE(cpuInsnEvBody.topics.at(1).sym() == "cpu_insn");
    REQUIRE(cpuInsnEvBody.data.type() == SCV_U64);
    REQUIRE(cpuInsnEvBody.data.u64() >= 1000);
}

TEST_CASE("invalid tx with diagnostics", "[tx][soroban]")
{
    // This test produces a diagnostic event _during validation_, i.e. before
    // it ever makes it to the soroban host.

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
    resources.instructions = 2'000'000'000;
    resources.readBytes = 2000;
    resources.writeBytes = 1000;

    auto tx = sorobanTransactionFrameFromOps(app->getNetworkID(), root, {op},
                                             {}, resources, 100,
                                             DEFAULT_TEST_RESOURCE_FEE);
    LedgerTxn ltx(app->getLedgerTxnRoot());
    TransactionMetaFrame txm(ltx.loadHeader().current().ledgerVersion);
    REQUIRE(!tx->checkValid(*app, ltx, 0, 0, 0));

    auto const& diagEvents = tx->getDiagnosticEvents();
    REQUIRE(diagEvents.size() == 1);

    DiagnosticEvent const& diag_ev = diagEvents.at(0);
    LOG_INFO(DEFAULT_LOG, "event 0: {}", xdr::xdr_to_string(diag_ev));
    REQUIRE(!diag_ev.inSuccessfulContractCall);
    REQUIRE(diag_ev.event.type == ContractEventType::DIAGNOSTIC);
    REQUIRE(diag_ev.event.body.v0().topics.at(0).sym() == "error");
    REQUIRE(diag_ev.event.body.v0().data.vec()->at(0).str().find(
                "instructions") != std::string::npos);
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
            auto tx = sorobanTransactionFrameFromOps(app->getNetworkID(), root,
                                                     {op}, {}, resources, 100,
                                                     DEFAULT_TEST_RESOURCE_FEE);
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

TEST_CASE("Stellar asset contract transfer",
          "[tx][soroban][invariant][conservationoflumens]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    // Override the initial limits for the trustline to trustline transfer
    cfg.TESTING_SOROBAN_HIGH_LIMIT_OVERRIDE = true;
    auto app = createTestApplication(clock, cfg);
    overrideSorobanNetworkConfigForTest(*app);
    auto root = TestAccount::createRoot(*app);

    auto createAssetContract =
        [&](Asset const& asset) -> std::pair<SCAddress, LedgerKey> {
        HashIDPreimage preImage;
        preImage.type(ENVELOPE_TYPE_CONTRACT_ID);
        preImage.contractID().contractIDPreimage.type(
            CONTRACT_ID_PREIMAGE_FROM_ASSET);
        preImage.contractID().contractIDPreimage.fromAsset() = asset;
        preImage.contractID().networkID = app->getNetworkID();
        auto contractID = makeContractAddress(xdrSha256(preImage));

        Operation createOp;
        createOp.body.type(INVOKE_HOST_FUNCTION);
        auto& createHF = createOp.body.invokeHostFunctionOp();
        createHF.hostFunction.type(HOST_FUNCTION_TYPE_CREATE_CONTRACT);
        auto& createContractArgs = createHF.hostFunction.createContract();

        ContractExecutable exec;
        exec.type(CONTRACT_EXECUTABLE_STELLAR_ASSET);
        createContractArgs.contractIDPreimage.type(
            CONTRACT_ID_PREIMAGE_FROM_ASSET);
        createContractArgs.contractIDPreimage.fromAsset() = asset;
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

        createResources.footprint.readWrite = {contractExecutableKey};

        {
            // submit operation
            auto tx = sorobanTransactionFrameFromOps(
                app->getNetworkID(), root, {createOp}, {}, createResources, 100,
                DEFAULT_TEST_RESOURCE_FEE);

            LedgerTxn ltx(app->getLedgerTxnRoot());
            TransactionMetaFrame txm(ltx.loadHeader().current().ledgerVersion);
            REQUIRE(tx->checkValid(*app, ltx, 0, 0, 0));
            REQUIRE(tx->apply(*app, ltx, txm));
            ltx.commit();
        }

        return std::make_pair(contractID, contractExecutableKey);
    };

    SECTION("XLM")
    {
        auto idAndExec = createAssetContract(txtest::makeNativeAsset());

        auto contractID = idAndExec.first;
        auto contractExecutableKey = idAndExec.second;

        auto key = SecretKey::pseudoRandomForTesting();
        TestAccount a1(*app, key);
        auto tx = transactionFrameFromOps(
            app->getNetworkID(), root,
            {root.op(beginSponsoringFutureReserves(a1)),
             root.op(createAccount(
                 a1, app->getLedgerManager().getLastMinBalance(10))),
             a1.op(endSponsoringFutureReserves())},
            {key});

        {
            LedgerTxn ltx(app->getLedgerTxnRoot());
            TransactionMetaFrame txm(ltx.loadHeader().current().ledgerVersion);
            REQUIRE(tx->checkValid(*app, ltx, 0, 0, 0));
            REQUIRE(tx->apply(*app, ltx, txm));
            ltx.commit();
        }

        {
            LedgerTxn ltx(app->getLedgerTxnRoot());
            checkSponsorship(ltx, a1.getPublicKey(), 1, &root.getPublicKey(), 0,
                             2, 0, 2);
            checkSponsorship(ltx, root.getPublicKey(), 0, nullptr, 0, 2, 2, 0);
        }

        // transfer 10 XLM from a1 to contractID
        SCAddress fromAccount(SC_ADDRESS_TYPE_ACCOUNT);
        fromAccount.accountId() = a1.getPublicKey();
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
        accountLedgerKey.account().accountID = a1.getPublicKey();

        LedgerKey balanceLedgerKey(CONTRACT_DATA);
        balanceLedgerKey.contractData().contract = contractID;
        SCVec balance = {makeSymbolSCVal("Balance"), to};
        SCVal balanceKey(SCValType::SCV_VEC);
        balanceKey.vec().activate() = balance;
        balanceLedgerKey.contractData().key = balanceKey;
        balanceLedgerKey.contractData().durability =
            ContractDataDurability::PERSISTENT;

        resources.footprint.readOnly = {contractExecutableKey};
        resources.footprint.readWrite = {accountLedgerKey, balanceLedgerKey};

        auto preTransferA1Balance = a1.getBalance();

        {
            // submit operation
            auto tx = sorobanTransactionFrameFromOps(
                app->getNetworkID(), a1, {transfer}, {}, resources, 100,
                DEFAULT_TEST_RESOURCE_FEE);

            LedgerTxn ltx(app->getLedgerTxnRoot());
            TransactionMetaFrame txm(ltx.loadHeader().current().ledgerVersion);
            REQUIRE(tx->checkValid(*app, ltx, 0, 0, 0));
            REQUIRE(tx->apply(*app, ltx, txm));
            ltx.commit();
        }
        REQUIRE(preTransferA1Balance - 10 == a1.getBalance());

        // Make sure sponsorship info hasn't changed
        {
            LedgerTxn ltx(app->getLedgerTxnRoot());
            checkSponsorship(ltx, a1.getPublicKey(), 1, &root.getPublicKey(), 0,
                             2, 0, 2);
            checkSponsorship(ltx, root.getPublicKey(), 0, nullptr, 0, 2, 2, 0);
        }
    }

    SECTION("trustline")
    {
        auto const minBalance = app->getLedgerManager().getLastMinBalance(2);
        auto issuer = root.create("issuer", minBalance);
        auto acc = root.create("acc", minBalance);
        auto sponsor = root.create("sponsor", minBalance * 2);

        Asset idr = makeAsset(issuer, "IDR");
        root.changeTrust(idr, 100);
        {
            auto tx = transactionFrameFromOps(
                app->getNetworkID(), sponsor,
                {sponsor.op(beginSponsoringFutureReserves(acc)),
                 acc.op(changeTrust(idr, 100)),
                 acc.op(endSponsoringFutureReserves())},
                {acc});

            LedgerTxn ltx(app->getLedgerTxnRoot());
            TransactionMetaFrame txm(ltx.loadHeader().current().ledgerVersion);
            REQUIRE(tx->checkValid(*app, ltx, 0, 0, 0));
            REQUIRE(tx->apply(*app, ltx, txm));
            REQUIRE(tx->getResultCode() == txSUCCESS);
            ltx.commit();
        }

        {
            LedgerTxn ltx(app->getLedgerTxnRoot());
            auto tlAsset = assetToTrustLineAsset(idr);
            checkSponsorship(ltx, trustlineKey(acc, tlAsset), 1,
                             &sponsor.getPublicKey());
            checkSponsorship(ltx, acc, 0, nullptr, 1, 2, 0, 1);
            checkSponsorship(ltx, sponsor, 0, nullptr, 0, 2, 1, 0);
        }

        issuer.pay(root, idr, 100);

        auto idAndExec = createAssetContract(idr);

        auto contractID = idAndExec.first;
        auto contractExecutableKey = idAndExec.second;

        SCAddress fromAccount(SC_ADDRESS_TYPE_ACCOUNT);
        fromAccount.accountId() = root.getPublicKey();
        SCVal from(SCV_ADDRESS);
        from.address() = fromAccount;

        SCAddress toAccount(SC_ADDRESS_TYPE_ACCOUNT);
        toAccount.accountId() = acc.getPublicKey();
        SCVal to(SCV_ADDRESS);
        to.address() = toAccount;

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

        LedgerKey issuerLedgerKey(ACCOUNT);
        issuerLedgerKey.account().accountID = issuer.getPublicKey();

        LedgerKey rootTrustlineLedgerKey(TRUSTLINE);
        rootTrustlineLedgerKey.trustLine().accountID = root.getPublicKey();
        rootTrustlineLedgerKey.trustLine().asset = assetToTrustLineAsset(idr);

        LedgerKey accTrustlineLedgerKey(TRUSTLINE);
        accTrustlineLedgerKey.trustLine().accountID = acc.getPublicKey();
        accTrustlineLedgerKey.trustLine().asset = assetToTrustLineAsset(idr);

        resources.footprint.readOnly = {contractExecutableKey, issuerLedgerKey};

        resources.footprint.readWrite = {rootTrustlineLedgerKey,
                                         accTrustlineLedgerKey};

        {
            // submit operation
            auto tx = sorobanTransactionFrameFromOps(
                app->getNetworkID(), root, {transfer}, {}, resources, 100,
                DEFAULT_TEST_RESOURCE_FEE);

            LedgerTxn ltx(app->getLedgerTxnRoot());
            TransactionMetaFrame txm(ltx.loadHeader().current().ledgerVersion);
            REQUIRE(tx->checkValid(*app, ltx, 0, 0, 0));
            REQUIRE(tx->apply(*app, ltx, txm));
            ltx.commit();
        }

        REQUIRE(root.getTrustlineBalance(idr) == 90);
        REQUIRE(acc.getTrustlineBalance(idr) == 10);

        // Make sure sponsorship info hasn't changed
        {
            LedgerTxn ltx(app->getLedgerTxnRoot());
            auto tlAsset = assetToTrustLineAsset(idr);
            checkSponsorship(ltx, trustlineKey(acc, tlAsset), 1,
                             &sponsor.getPublicKey());
            checkSponsorship(ltx, acc, 0, nullptr, 1, 2, 0, 1);
            checkSponsorship(ltx, sponsor, 0, nullptr, 0, 2, 1, 0);
        }
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

        auto tx = sorobanTransactionFrameFromOps(app->getNetworkID(), root,
                                                 {op}, {}, resources, 100,
                                                 DEFAULT_TEST_RESOURCE_FEE);
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
                             ConfigSettingID::CONFIG_SETTING_STATE_ARCHIVAL))
                    .current()
                    .data.configSetting()
                    .stateArchivalSettings();
    exp.maxEntryTTL = MinimumSorobanNetworkConfig::MAXIMUM_ENTRY_LIFETIME;
    exp.minPersistentTTL =
        MinimumSorobanNetworkConfig::MINIMUM_PERSISTENT_ENTRY_LIFETIME;
    exp.minTemporaryTTL =
        MinimumSorobanNetworkConfig::MINIMUM_TEMP_ENTRY_LIFETIME;
    exp.persistentRentRateDenominator =
        MinimumSorobanNetworkConfig::RENT_RATE_DENOMINATOR;
    exp.tempRentRateDenominator =
        MinimumSorobanNetworkConfig::RENT_RATE_DENOMINATOR;
    exp.maxEntriesToArchive =
        MinimumSorobanNetworkConfig::MAX_ENTRIES_TO_ARCHIVE;
    exp.bucketListSizeWindowSampleSize =
        MinimumSorobanNetworkConfig::BUCKETLIST_SIZE_WINDOW_SAMPLE_SIZE;
    exp.evictionScanSize = MinimumSorobanNetworkConfig::EVICTION_SCAN_SIZE;
    exp.startingEvictionScanLevel =
        MinimumSorobanNetworkConfig::STARTING_EVICTION_LEVEL;

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
        uploadResources.instructions =
            MinimumSorobanNetworkConfig::TX_MAX_INSTRUCTIONS;
        uploadResources.readBytes =
            MinimumSorobanNetworkConfig::TX_MAX_READ_BYTES;
        uploadResources.writeBytes =
            MinimumSorobanNetworkConfig::TX_MAX_WRITE_BYTES;

        SorobanResources createResources{};
        createResources.instructions =
            MinimumSorobanNetworkConfig::TX_MAX_INSTRUCTIONS;
        createResources.readBytes =
            MinimumSorobanNetworkConfig::TX_MAX_READ_BYTES;
        createResources.writeBytes =
            MinimumSorobanNetworkConfig::TX_MAX_WRITE_BYTES;
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
        upgrade.contractData().key =
            makeBytes(xdr::xdr_to_opaque(upgrade_hash));

        SorobanResources resources{};
        resources.footprint.readOnly = contractKeys;
        resources.footprint.readWrite = {upgrade};
        resources.instructions = 2'000'000;
        resources.readBytes = 3000;
        resources.writeBytes = 2000;

        auto tx = sorobanTransactionFrameFromOps(
            app->getNetworkID(), root, {op}, {}, resources, 1000, 20'000'000);

        {
            LedgerTxn ltx(app->getLedgerTxnRoot());
            REQUIRE(tx->checkValid(*app, ltx, 0, 0, 0));
            TransactionMetaFrame txm(ltx.loadHeader().current().ledgerVersion);
            REQUIRE(tx->apply(*app, ltx, txm));
            ltx.commit();
        }

        {
            // verify that the contract code, contract instance, and upgrade
            // entry were all extended by
            // 1036800 ledgers (60 days) -
            // https://github.com/stellar/rs-soroban-env/blob/main/soroban-test-wasms/wasm-workspace/write_upgrade_bytes/src/lib.rs#L3-L5
            LedgerTxn ltx(app->getLedgerTxnRoot());
            auto ledgerSeq = ltx.loadHeader().current().ledgerSeq;
            auto extendedKeys = contractKeys;
            extendedKeys.emplace_back(upgrade);

            REQUIRE(extendedKeys.size() == 3);
            for (auto const& key : extendedKeys)
            {
                auto ltxe = ltx.load(key);
                REQUIRE(ltxe);
                checkTTL(ltx, key, ledgerSeq + 1036800);
            }
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
