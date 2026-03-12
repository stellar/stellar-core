// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTypeUtils.h"
#include "main/SettingsUpgradeUtils.h"
#include "rust/RustBridge.h"
#include "test/Catch2.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/test/SorobanTxTestUtils.h"
#include "xdr/Stellar-ledger-entries.h"

using namespace stellar;
using namespace stellar::txtest;

namespace
{

TEST_CASE("Module cache", "[tx][soroban]")
{
    VirtualClock clock;
    auto cfg = getTestConfig(0);
    cfg.USE_CONFIG_FOR_GENESIS = false;

    auto app = createTestApplication(clock, cfg);

    auto upgrade20 = LedgerUpgrade{LEDGER_UPGRADE_VERSION};
    upgrade20.newLedgerVersion() = static_cast<int>(SOROBAN_PROTOCOL_VERSION);
    executeUpgrade(*app, upgrade20);

    // Test that repeated calls to a contract in a single transaction are
    // cheaper due to caching introduced in v21.
    auto sum_wasm = rust_bridge::get_test_wasm_sum_i32();
    auto add_wasm = rust_bridge::get_test_wasm_add_i32();
    SorobanTest test(app);

    auto const& sumContract = test.deployWasmContract(sum_wasm);
    auto const& addContract = test.deployWasmContract(add_wasm);

    auto invocation = [&](int64_t instructions) -> bool {
        auto fnName = "sum";
        auto scVec = makeVecSCVal({makeI32(1), makeI32(2), makeI32(3),
                                   makeI32(4), makeI32(5), makeI32(6)});

        auto invocationSpec = SorobanInvocationSpec()
                                  .setInstructions(instructions)
                                  .setReadBytes(2'000)
                                  .setInclusionFee(12345);

        uint32_t const expectedRefund = 100'000;
        auto spec = invocationSpec.setNonRefundableResourceFee(33'000)
                        .setRefundableResourceFee(expectedRefund);

        spec = spec.extendReadOnlyFootprint(addContract.getKeys());

        auto invocation = sumContract.prepareInvocation(
            fnName, {makeAddressSCVal(addContract.getAddress()), scVec}, spec,
            expectedRefund);
        auto tx = invocation.createTx();
        return isSuccessResult(test.invokeTx(tx));
    };

    REQUIRE(invocation(7'000'000));
    REQUIRE(!invocation(6'000'000));

    auto upgrade21 = LedgerUpgrade{LEDGER_UPGRADE_VERSION};
    upgrade21.newLedgerVersion() = static_cast<int>(ProtocolVersion::V_21);
    executeUpgrade(*app, upgrade21);

    // V21 Caching reduces the instructions required
    REQUIRE(invocation(4'000'000));
}

static TransactionFrameBasePtr
makeAddTx(TestContract const& contract, int64_t instructions,
          TestAccount& source)
{
    auto fnName = "add";
    auto sc7 = makeI32(7);
    auto sc16 = makeI32(16);
    auto spec = SorobanInvocationSpec()
                    .setInstructions(instructions)
                    .setReadBytes(2'000)
                    .setInclusionFee(12345)
                    .setNonRefundableResourceFee(33'000)
                    .setRefundableResourceFee(100'000);
    auto invocation = contract.prepareInvocation(fnName, {sc7, sc16}, spec);
    return invocation.createTx(&source);
}

static bool
wasmsAreCached(Application& app, std::vector<Hash> const& wasms)
{
    auto moduleCache = app.getLedgerManager().getModuleCacheForTesting();
    for (auto const& wasm : wasms)
    {
        if (!moduleCache->contains_module(
                app.getLedgerManager()
                    .getLastClosedLedgerHeader()
                    .header.ledgerVersion,
                ::rust::Slice{wasm.data(), wasm.size()}))
        {
            return false;
        }
    }
    return true;
}

static int64_t const INVOKE_ADD_UNCACHED_COST_PASS = 500'000;
static int64_t const INVOKE_ADD_UNCACHED_COST_FAIL = 400'000;

static int64_t const INVOKE_ADD_CACHED_COST_PASS = 300'000;
static int64_t const INVOKE_ADD_CACHED_COST_FAIL = 200'000;

TEST_CASE("reusable module cache", "[soroban][modulecache]")
{
    VirtualClock clock;
    Config cfg = getTestConfig(0, Config::TESTDB_BUCKET_DB_PERSISTENT);

    cfg.OVERRIDE_EVICTION_PARAMS_FOR_TESTING = true;
    cfg.TESTING_STARTING_EVICTION_SCAN_LEVEL = 1;

    // This test uses/tests/requires the reusable module cache.
    if (!protocolVersionStartsFrom(
            cfg.LEDGER_PROTOCOL_VERSION,
            REUSABLE_SOROBAN_MODULE_CACHE_PROTOCOL_VERSION))
        return;

    // First upload some wasms
    std::vector<RustBuf> testWasms = {rust_bridge::get_test_wasm_add_i32(),
                                      rust_bridge::get_test_wasm_err(),
                                      rust_bridge::get_test_wasm_complex()};

    std::vector<Hash> contractHashes;
    uint32_t ttl{0};
    {
        txtest::SorobanTest stest(cfg);
        ttl = stest.getNetworkCfg().stateArchivalSettings().minPersistentTTL;
        for (auto const& wasm : testWasms)
        {

            stest.deployWasmContract(wasm);
            contractHashes.push_back(sha256(wasm));
        }
        // Check the module cache got populated by the uploads.
        REQUIRE(wasmsAreCached(stest.getApp(), contractHashes));
    }

    // Restart the application and check module cache gets populated in the new
    // app.
    auto app = createTestApplication(clock, cfg, false, true);
    REQUIRE(wasmsAreCached(*app, contractHashes));

    // Crank the app forward a while until the wasms are evicted.
    CLOG_INFO(Ledger, "advancing for {} ledgers to evict wasms", ttl);
    for (int i = 0; i < ttl; ++i)
    {
        txtest::closeLedger(*app);
    }
    // Check the modules got evicted.
    REQUIRE(!wasmsAreCached(*app, contractHashes));
}

TEST_CASE("module cache rebuild on incremental wasm uploads",
          "[soroban][modulecache]")
{
    VirtualClock clock;
    Config cfg = getTestConfig(0, Config::TESTDB_BUCKET_DB_PERSISTENT);

    // This test uses/tests/requires the reusable module cache.
    if (!protocolVersionStartsFrom(
            cfg.LEDGER_PROTOCOL_VERSION,
            REUSABLE_SOROBAN_MODULE_CACHE_PROTOCOL_VERSION))
        return;

    std::vector<RustBuf> initialWasms = {rust_bridge::get_test_wasm_add_i32(),
                                         rust_bridge::get_test_wasm_sum_i32()};
    std::vector<RustBuf> additionalWasms = {
        rust_bridge::get_test_wasm_err(),
        rust_bridge::get_test_wasm_contract_data(),
        rust_bridge::get_test_wasm_complex(),
        rust_bridge::get_test_wasm_loadgen()};

    std::vector<Hash> initialHashes;
    for (auto const& wasm : initialWasms)
    {
        initialHashes.push_back(sha256(wasm));
    }

    // Populate persistent DB with an initial set of wasm entries.
    {
        SorobanTest stest(cfg);
        for (auto const& wasm : initialWasms)
        {
            stest.deployWasmContract(wasm);
        }
        REQUIRE(wasmsAreCached(stest.getApp(), initialHashes));
    }

    // Restart and verify startup compilation rebuilt cache from the DB state.
    auto app = createTestApplication(clock, cfg, false, true);
    REQUIRE(wasmsAreCached(*app, initialHashes));

    auto& metrics = app->getLedgerManager().getSorobanMetrics();
    auto rebuildBytesAtStartup = metrics.mModuleCacheRebuildBytes.count();
    REQUIRE(rebuildBytesAtStartup > 0);

    auto uploader = app->getRoot();
    auto uploadWasm = [&](RustBuf const& wasm) {
        auto uploadResources = defaultUploadWasmResourcesWithoutFootprint(
            wasm, getLclProtocolVersion(*app));
        auto uploadTx = makeSorobanWasmUploadTx(*app, *uploader, wasm,
                                                uploadResources, 1000);
        auto txResults = closeLedger(*app, {uploadTx});
        REQUIRE(txResults.results.size() == 1);
        REQUIRE(isSuccessResult(txResults.results[0].result));
    };

    uint64_t uploadedRawBeforeTrigger = 0;
    uint64_t uploadedRawAtTrigger = 0;
    uint64_t uploadedRawBytes = 0;
    bool rebuilt = false;

    for (auto const& wasm : additionalWasms)
    {
        uploadedRawBeforeTrigger = uploadedRawBytes;
        uploadWasm(wasm);
        uploadedRawBytes += wasm.data.size();

        // If a rebuild was started by this upload, it completes on next
        // ledger close at apply start.
        closeLedger(*app);

        if (metrics.mModuleCacheRebuildBytes.count() != rebuildBytesAtStartup)
        {
            rebuilt = true;
            uploadedRawAtTrigger = uploadedRawBytes;
            break;
        }
    }

    REQUIRE(rebuilt);

    // maybeRebuildModuleCache rebuilds when current cache bytes exceed
    // 2 * bytes from the previous full rebuild. Since this test starts from a
    // freshly rebuilt cache, that means a rebuild should start after crossing
    // roughly one rebuild's worth of additional wasm bytes.
    REQUIRE(uploadedRawBeforeTrigger <=
            static_cast<uint64_t>(rebuildBytesAtStartup));
    REQUIRE(uploadedRawAtTrigger >
            static_cast<uint64_t>(rebuildBytesAtStartup));
}

TEST_CASE("Module cache across protocol versions", "[tx][soroban][modulecache]")
{
    VirtualClock clock;
    auto cfg = getTestConfig(0);
    // Start in p22
    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
        static_cast<int>(REUSABLE_SOROBAN_MODULE_CACHE_PROTOCOL_VERSION) - 1;
    auto app = createTestApplication(clock, cfg);

    // Deploy and invoke contract in protocol 22
    SorobanTest test(app);

    size_t baseContractCount = app->getLedgerManager()
                                   .getSorobanMetrics()
                                   .mModuleCacheNumEntries.count();
    auto const& addContract =
        test.deployWasmContract(rust_bridge::get_test_wasm_add_i32());

    auto invoke = [&](int64_t instructions) -> bool {
        auto tx = makeAddTx(addContract, instructions, test.getRoot());
        auto res = test.invokeTx(tx);
        return isSuccessResult(res);
    };

    REQUIRE(!invoke(INVOKE_ADD_UNCACHED_COST_FAIL));
    REQUIRE(invoke(INVOKE_ADD_UNCACHED_COST_PASS));

    // The upload should have triggered a single compilation for the p23+ module
    // caches, which _exist_ in this version of stellar-core, and need to be
    // populated on each upload, but are just not yet active.
    int moduleCacheProtocolCount =
        Config::CURRENT_LEDGER_PROTOCOL_VERSION -
        static_cast<int>(REUSABLE_SOROBAN_MODULE_CACHE_PROTOCOL_VERSION) + 1;
    REQUIRE(app->getLedgerManager()
                .getSorobanMetrics()
                .mModuleCacheNumEntries.count() ==
            baseContractCount + moduleCacheProtocolCount);

    // Upgrade to protocol 23 (with the reusable module cache)
    auto upgradeTo23 = LedgerUpgrade{LEDGER_UPGRADE_VERSION};
    upgradeTo23.newLedgerVersion() =
        static_cast<int>(REUSABLE_SOROBAN_MODULE_CACHE_PROTOCOL_VERSION);
    executeUpgrade(*app, upgradeTo23);

    // We can now run the same contract with fewer instructions
    REQUIRE(!invoke(INVOKE_ADD_CACHED_COST_FAIL));
    REQUIRE(invoke(INVOKE_ADD_CACHED_COST_PASS));
}

TEST_CASE("Module cache miss on immediate execution",
          "[tx][soroban][modulecache]")
{
    VirtualClock clock;
    auto cfg = getTestConfig(0);

    // This test uses/tests/requires the reusable module cache.
    if (!protocolVersionStartsFrom(
            cfg.LEDGER_PROTOCOL_VERSION,
            REUSABLE_SOROBAN_MODULE_CACHE_PROTOCOL_VERSION))
        return;

    auto app = createTestApplication(clock, cfg);

    SorobanTest test(app);

    size_t baseContractCount = app->getLedgerManager()
                                   .getSorobanMetrics()
                                   .mModuleCacheNumEntries.count();

    auto wasm = rust_bridge::get_test_wasm_add_i32();

    SECTION("separate ledger upload and execution")
    {
        // First upload the contract
        auto const& contract = test.deployWasmContract(wasm);

        // Confirm upload succeeded and triggered compilation
        REQUIRE(app->getLedgerManager()
                    .getSorobanMetrics()
                    .mModuleCacheNumEntries.count() == baseContractCount + 1);

        // Try to execute with low instructions since we can use cached module.
        auto txFail =
            makeAddTx(contract, INVOKE_ADD_CACHED_COST_FAIL, test.getRoot());
        REQUIRE(!isSuccessResult(test.invokeTx(txFail)));

        auto txPass =
            makeAddTx(contract, INVOKE_ADD_CACHED_COST_PASS, test.getRoot());
        REQUIRE(isSuccessResult(test.invokeTx(txPass)));
    }

    SECTION("same ledger upload and execution")
    {

        // Here we're going to create 4 txs in the same ledger (so they have to
        // come from 4 separate accounts). The 1st uploads a contract wasm, the
        // 2nd creates a contract, and the 3rd and 4th run it.
        //
        // Because all 4 happen in the same ledger, there is no opportunity for
        // the module cache to be populated between the upload and the
        // execution. This should result in a cache miss and higher cost: the
        // 3rd (invoking) tx fails and the 4th passes, but at the higher cost.
        //
        // Finally to confirm that the cache is populated, we run the same
        // invocations in the next ledger and it should succeed at a lower cost.

        auto minbal = test.getApp().getLedgerManager().getLastMinBalance(1);
        TestAccount A(test.getRoot().create("A", minbal * 1000));
        TestAccount B(test.getRoot().create("B", minbal * 1000));
        TestAccount C(test.getRoot().create("C", minbal * 1000));
        TestAccount D(test.getRoot().create("D", minbal * 1000));

        // Transaction 1: the upload
        auto uploadResources = defaultUploadWasmResourcesWithoutFootprint(
            wasm, getLclProtocolVersion(test.getApp()));
        auto uploadTx = makeSorobanWasmUploadTx(test.getApp(), A, wasm,
                                                uploadResources, 1000);

        // Transaction 2: create contract
        Hash contractHash = sha256(wasm);
        ContractExecutable executable = makeWasmExecutable(contractHash);
        Hash salt = sha256("salt");
        ContractIDPreimage contractPreimage = makeContractIDPreimage(B, salt);
        HashIDPreimage hashPreimage = makeFullContractIdPreimage(
            test.getApp().getNetworkID(), contractPreimage);
        SCAddress contractId = makeContractAddress(xdrSha256(hashPreimage));
        auto createResources = SorobanResources();
        createResources.instructions = 5'000'000;
        createResources.diskReadBytes =
            static_cast<uint32_t>(wasm.data.size() + 1000);
        createResources.writeBytes = 1000;
        auto createContractTx =
            makeSorobanCreateContractTx(test.getApp(), B, contractPreimage,
                                        executable, createResources, 1000);

        // Transaction 3: invocation (with inadequate instructions to succeed)
        TestContract contract(test, contractId,
                              {contractCodeKey(contractHash),
                               makeContractInstanceKey(contractId)});
        auto invokeFailTx =
            makeAddTx(contract, INVOKE_ADD_UNCACHED_COST_FAIL, C);

        // Transaction 4: invocation (with inadequate instructions to succeed)
        auto invokePassTx =
            makeAddTx(contract, INVOKE_ADD_UNCACHED_COST_PASS, C);

        // Run single ledger with all 4 txs. First 2 should pass, 3rd should
        // fail, 4th should pass.
        auto txResults = closeLedger(
            *app, {uploadTx, createContractTx, invokeFailTx, invokePassTx},
            /*strictOrder=*/true);

        REQUIRE(txResults.results.size() == 4);
        REQUIRE(
            isSuccessResult(txResults.results[0].result)); // Upload succeeds
        REQUIRE(
            isSuccessResult(txResults.results[1].result)); // Create succeeds
        REQUIRE(!isSuccessResult(
            txResults.results[2].result)); // Invoke fails at 400k
        REQUIRE(isSuccessResult(
            txResults.results[3].result)); // Invoke passes at 500k

        // But if we try again in next ledger, the cost threshold should be
        // lower.
        auto invokeTxFail2 =
            makeAddTx(contract, INVOKE_ADD_CACHED_COST_FAIL, C);
        auto invokeTxPass2 =
            makeAddTx(contract, INVOKE_ADD_CACHED_COST_PASS, D);
        txResults = closeLedger(*app, {invokeTxFail2, invokeTxPass2},
                                /*strictOrder=*/true);
        REQUIRE(txResults.results.size() == 2);
        REQUIRE(!isSuccessResult(txResults.results[0].result));
        REQUIRE(isSuccessResult(txResults.results[1].result));
    }
}

TEST_CASE("Module cache cost with restore gaps", "[tx][soroban][modulecache]")
{
    VirtualClock clock;
    auto cfg = getTestConfig(0);

    cfg.OVERRIDE_EVICTION_PARAMS_FOR_TESTING = true;
    cfg.TESTING_STARTING_EVICTION_SCAN_LEVEL = 1;

    auto app = createTestApplication(clock, cfg);
    auto& lm = app->getLedgerManager();
    SorobanTest test(app);
    auto wasm = rust_bridge::get_test_wasm_add_i32();

    auto minbal = lm.getLastMinBalance(1);
    TestAccount A(test.getRoot().create("A", minbal * 1000));
    TestAccount B(test.getRoot().create("B", minbal * 1000));

    auto contract = test.deployWasmContract(wasm);
    auto contractKeys = contract.getKeys();

    // Let contract expire
    auto ttl = test.getNetworkCfg().stateArchivalSettings().minPersistentTTL;
    auto proto = lm.getLastClosedLedgerHeader().header.ledgerVersion;
    for (auto i = 0; i < ttl; ++i)
    {
        closeLedger(test.getApp());
    }
    auto moduleCache = lm.getModuleCacheForTesting();
    auto const wasmHash = sha256(wasm);
    REQUIRE(!moduleCache->contains_module(
        proto, ::rust::Slice{wasmHash.data(), wasmHash.size()}));

    SECTION("scenario A: restore in one ledger, invoke in next")
    {
        // Restore contract in ledger N+1
        test.invokeRestoreOp(contractKeys, 40'493);

        // Invoke in ledger N+2
        // Because we have a gap between restore and invoke, the module cache
        // will be populated and we need fewer instructions
        auto tx1 = makeAddTx(contract, INVOKE_ADD_CACHED_COST_FAIL, A);
        auto tx2 = makeAddTx(contract, INVOKE_ADD_CACHED_COST_PASS, B);
        auto txResults = closeLedger(*app, {tx1, tx2}, /*strictOrder=*/true);
        REQUIRE(txResults.results.size() == 2);
        REQUIRE(!isSuccessResult(txResults.results[0].result));
        REQUIRE(isSuccessResult(txResults.results[1].result));
    }

    SECTION("scenario B: restore and invoke in same ledger")
    {
        // Combine restore and invoke in ledger N+1
        // First restore
        SorobanResources resources;
        resources.footprint.readWrite = contractKeys;
        resources.instructions = 0;
        resources.diskReadBytes = 10'000;
        resources.writeBytes = 10'000;
        auto resourceFee = 300'000 + 40'000 * contractKeys.size();
        auto tx1 = test.createRestoreTx(resources, 1'000, resourceFee);

        // Then try to invoke immediately
        // Because there is no gap between restore and invoke, the module cache
        // won't be populated and we need more instructions.
        auto tx2 = makeAddTx(contract, INVOKE_ADD_UNCACHED_COST_FAIL, A);
        auto tx3 = makeAddTx(contract, INVOKE_ADD_UNCACHED_COST_PASS, B);
        auto txResults =
            closeLedger(*app, {tx1, tx2, tx3}, /*strictOrder=*/true);
        REQUIRE(txResults.results.size() == 3);
        REQUIRE(isSuccessResult(txResults.results[0].result));
        REQUIRE(!isSuccessResult(txResults.results[1].result));
        REQUIRE(isSuccessResult(txResults.results[2].result));
    }
}

} // namespace
