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
#include "main/SettingsUpgradeUtils.h"
#include "rust/RustBridge.h"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "transactions/InvokeHostFunctionOpFrame.h"
#include "transactions/SignatureUtils.h"
#include "transactions/TransactionUtils.h"
#include "transactions/test/SorobanTxTestUtils.h"
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

TEST_CASE("basic contract invocation", "[tx][soroban]")
{
    ContractInvocationTest test(rust_bridge::get_test_wasm_add_i32());

    int64_t const INCLUSION_FEE = 12'345;

    auto checkRefund = [&test](auto const& txm, int64_t balanceAfterFeeCharged,
                               int64_t expectedRefund) {
        auto changesAfter = txm->getChangesAfter();
        REQUIRE(changesAfter.size() == 2);
        REQUIRE(changesAfter[1].updated().data.account().balance -
                    changesAfter[0].state().data.account().balance ==
                expectedRefund);

        // Make sure account receives expected refund
        REQUIRE(test.getRoot().getBalance() - balanceAfterFeeCharged ==
                expectedRefund);
    };

    auto const expectedRefund = 267'298;
    auto successfulInvoke = [&](TransactionFrameBasePtr tx,
                                uint32_t resourceFee,
                                SorobanResources const& resources) {
        int64_t initBalance = test.getRoot().getBalance();
        test.txCheckValid(tx);

        REQUIRE(tx->getFullFee() == INCLUSION_FEE + resourceFee);
        REQUIRE(tx->getInclusionFee() == INCLUSION_FEE);

        // Initially we store in result the charge for resources plus
        // minimum inclusion  fee bid (currently equivalent to the
        // network `baseFee` of 100).
        int64_t baseCharged = (tx->getFullFee() - tx->getInclusionFee()) + 100;
        REQUIRE(tx->getResult().feeCharged == baseCharged);
        {
            LedgerTxn ltx(test.getApp()->getLedgerTxnRoot());
            // Imitate surge pricing by charging at a higher rate than
            // base fee.
            tx->processFeeSeqNum(ltx, 300);
            ltx.commit();
        }
        // The resource and the base fee are charged, with additional
        // surge pricing fee.
        int64_t balanceAfterFeeCharged = test.getRoot().getBalance();
        REQUIRE(initBalance - balanceAfterFeeCharged ==
                baseCharged + /* surge pricing additional fee */ 200);

        auto txm = test.invokeTx(tx, /*expectSuccess=*/true);

        auto changesAfter = txm->getChangesAfter();
        checkRefund(txm, balanceAfterFeeCharged, expectedRefund);

        // Sanity-check the expected refund value. The exact computation is
        // tricky because of the rent fee.
        // NB: We don't use the computed value here in order
        // to check possible changes in fee computation algorithm (so that
        // the conditions above would fail, while this still succeeds).
        REQUIRE(sorobanResourceFee(
                    *test.getApp(), resources, xdr::xdr_size(tx->getEnvelope()),
                    100 /* size of event emitted by the test contract */) ==
                resourceFee - expectedRefund);

        REQUIRE(tx->getResult().result.code() == txSUCCESS);
        REQUIRE(!tx->getResult().result.results().empty());

        auto const& ores = tx->getResult().result.results().at(0);
        REQUIRE(ores.tr().type() == INVOKE_HOST_FUNCTION);
        REQUIRE(ores.tr().invokeHostFunctionResult().code() ==
                INVOKE_HOST_FUNCTION_SUCCESS);

        SCVal resultVal = txm->getXDR().v3().sorobanMeta->returnValue;

        InvokeHostFunctionSuccessPreImage success2;
        success2.returnValue = resultVal;
        success2.events = txm->getXDR().v3().sorobanMeta->events;

        REQUIRE(ores.tr().invokeHostFunctionResult().success() ==
                xdrSha256(success2));
    };

    auto failedInvoke = [&](TransactionFrameBasePtr tx, int64_t resourceFee,
                            SorobanResources const& resources) {
        test.txCheckValid(tx);
        auto nonRefundableResourceFee = sorobanResourceFee(
            *test.getApp(), resources, xdr::xdr_size(tx->getEnvelope()), 0);

        // Imitate surge pricing by charging at a higher rate than base fee.
        {
            LedgerTxn ltx(test.getApp()->getLedgerTxnRoot());
            tx->processFeeSeqNum(ltx, 300);
            ltx.commit();
        }

        int64_t balanceAfterFeeCharged = test.getRoot().getBalance();
        auto txm = test.invokeTx(tx, /*expectSuccess=*/false);

        // The account should receive a full refund in case of tx failure.
        if (resourceFee > 0)
        {
            checkRefund(txm, balanceAfterFeeCharged,
                        /*expectedRefund=*/resourceFee -
                            nonRefundableResourceFee);
        }
        REQUIRE(tx->getResult().result.code() != txSUCCESS);
    };

    auto scFunc = makeSymbol("add");
    auto sc7 = makeI32(7);
    auto sc16 = makeI32(16);
    SorobanResources resources;
    resources.footprint.readOnly = test.getContractKeys();
    resources.instructions = 2'000'000;
    resources.readBytes = 2000;
    resources.writeBytes = 0;

    SECTION("correct invocation")
    {
        auto tx = test.createInvokeTx(resources, scFunc, {sc7, sc16},
                                      INCLUSION_FEE, 300'000);
        successfulInvoke(tx, 300'000, resources);

        REQUIRE(test.getApp()
                    ->getMetrics()
                    .NewTimer({"soroban", "host-fn-op", "exec"})
                    .count() != 0);
        REQUIRE(test.getApp()
                    ->getMetrics()
                    .NewMeter({"soroban", "host-fn-op", "success"}, "call")
                    .count() != 0);
    }

    SECTION("incorrect invocation parameters")
    {
        SECTION("non-existent contract id")
        {
            test.getContractID().contractId()[0] = 1;
            auto tx =
                test.createInvokeTx(resources, scFunc, {sc7, sc16},
                                    INCLUSION_FEE, DEFAULT_TEST_RESOURCE_FEE);
            failedInvoke(tx, DEFAULT_TEST_RESOURCE_FEE, resources);
        }
        SECTION("account address")
        {
            SCAddress address(SC_ADDRESS_TYPE_ACCOUNT);
            address.accountId() = test.getRoot().getPublicKey();
            test.getContractID() = address;
            auto tx =
                test.createInvokeTx(resources, scFunc, {sc7, sc16},
                                    INCLUSION_FEE, DEFAULT_TEST_RESOURCE_FEE);
            failedInvoke(tx, DEFAULT_TEST_RESOURCE_FEE, resources);
        }
        SECTION("too few parameters")
        {
            auto tx =
                test.createInvokeTx(resources, scFunc, {sc7}, INCLUSION_FEE,
                                    DEFAULT_TEST_RESOURCE_FEE);
            failedInvoke(tx, DEFAULT_TEST_RESOURCE_FEE, resources);
        }
        SECTION("too many parameters")
        {
            auto tx =
                test.createInvokeTx(resources, scFunc, {sc7, sc16, makeI32(0)},
                                    INCLUSION_FEE, DEFAULT_TEST_RESOURCE_FEE);
            failedInvoke(tx, DEFAULT_TEST_RESOURCE_FEE, resources);
        }
    }

    SECTION("insufficient instructions")
    {
        resources.instructions = 10000;
        auto tx = test.createInvokeTx(resources, scFunc, {sc7, sc16},
                                      INCLUSION_FEE, DEFAULT_TEST_RESOURCE_FEE);
        failedInvoke(tx, DEFAULT_TEST_RESOURCE_FEE, resources);
    }
    SECTION("insufficient read bytes")
    {
        resources.readBytes = 100;
        auto tx = test.createInvokeTx(resources, scFunc, {sc7, sc16},
                                      INCLUSION_FEE, DEFAULT_TEST_RESOURCE_FEE);
        failedInvoke(tx, DEFAULT_TEST_RESOURCE_FEE, resources);
    }
    SECTION("insufficient resource fee")
    {
        auto tx = test.createInvokeTx(resources, scFunc, {sc7, sc16},
                                      INCLUSION_FEE, 32'701);
        failedInvoke(tx, 32'701, resources);
    }
}

TEST_CASE("invalid footprint keys", "[tx][soroban]")
{
    ContractInvocationTest test(rust_bridge::get_test_wasm_add_i32());

    SorobanResources resources;
    resources.footprint.readOnly = test.getContractKeys();
    resources.instructions = 2'000'000;
    resources.readBytes = 2000;
    resources.writeBytes = 0;

    // Tests for each Soroban op
    auto testValidInvoke = [&](bool shouldBeValid) {
        auto scFunc = makeSymbol("add");
        auto sc7 = makeI32(7);
        auto sc16 = makeI32(16);

        auto tx = test.createInvokeTx(resources, scFunc, {sc7, sc16}, 100,
                                      DEFAULT_TEST_RESOURCE_FEE);
        REQUIRE(test.isTxValid(tx) == shouldBeValid);
    };

    auto testValidExtendOp = [&](bool shouldBeValid) {
        auto tx = test.createExtendOpTx(resources, 10, 100,
                                        DEFAULT_TEST_RESOURCE_FEE);
        REQUIRE(test.isTxValid(tx) == shouldBeValid);
    };

    auto testValidRestoreOp = [&](bool shouldBeValid) {
        auto tx =
            test.createRestoreTx(resources, 100, DEFAULT_TEST_RESOURCE_FEE);
        REQUIRE(test.isTxValid(tx) == shouldBeValid);
    };

    // Keys to test
    auto acc = test.getRoot().create(
        "acc", test.getApp()->getLedgerManager().getLastMinBalance(1));
    auto persistentKey =
        contractDataKey(test.getContractID(), makeSymbolSCVal("key1"),
                        ContractDataDurability::PERSISTENT);
    auto ttlKey = getTTLKey(persistentKey);

    // This function adds every invalid type to footprint and then runs f,
    // where f is either testValidInvoke, testValidExtendOp, or
    // testValidRestoreOp
    auto invalidateFootprint = [&](xdr::xvector<stellar::LedgerKey>& footprint,
                                   auto f) {
        SECTION("native asset trustline")
        {
            footprint.emplace_back(
                trustlineKey(test.getRoot().getPublicKey(), makeNativeAsset()));
            f(false);
        }
        SECTION("issuer trustline")
        {
            footprint.emplace_back(
                trustlineKey(test.getRoot().getPublicKey(),
                             makeAsset(test.getRoot(), "USD")));
            f(false);
        }
        auto invalidAssets = testutil::getInvalidAssets(test.getRoot());
        for (size_t i = 0; i < invalidAssets.size(); ++i)
        {
            auto key = trustlineKey(acc.getPublicKey(), invalidAssets[i]);
            SECTION("invalid asset " + std::to_string(i))
            {
                footprint.emplace_back(key);
                f(false);
            }
        }
        SECTION("offer")
        {
            footprint.emplace_back(offerKey(test.getRoot(), 1));
            f(false);
        }
        SECTION("data")
        {
            footprint.emplace_back(dataKey(test.getRoot(), "name"));
            f(false);
        }
        SECTION("claimable balance")
        {
            footprint.emplace_back(claimableBalanceKey(ClaimableBalanceID{}));
            f(false);
        }
        SECTION("liquidity pool")
        {
            footprint.emplace_back(liquidityPoolKey(PoolID{}));
            f(false);
        }
        SECTION("config setting")
        {
            footprint.emplace_back(configSettingKey(ConfigSettingID{}));
            f(false);
        }
        SECTION("TTL entry")
        {
            footprint.emplace_back(ttlKey);
            f(false);
        }
    };

    SECTION("invokeHostFunction")
    {
        auto validateFootprint =
            [&](xdr::xvector<stellar::LedgerKey>& footprint) {
                SECTION("valid")
                {
                    // add a valid trustline to the footprint to make sure the
                    // initial tx is valid.
                    footprint.emplace_back(trustlineKey(
                        test.getRoot().getPublicKey(), makeAsset(acc, "USD")));
                    testValidInvoke(true);
                }
            };

        SECTION("readOnly")
        {
            validateFootprint(resources.footprint.readOnly);
            invalidateFootprint(resources.footprint.readOnly, testValidInvoke);
        }
        SECTION("readWrite")
        {
            validateFootprint(resources.footprint.readWrite);
            invalidateFootprint(resources.footprint.readWrite, testValidInvoke);
        }
    }

    SECTION("extendOp")
    {
        SECTION("valid")
        {
            testValidExtendOp(true);
        }
        SECTION("readWrite set with Soroban key")
        {
            resources.footprint.readWrite.emplace_back(persistentKey);
            testValidExtendOp(false);
        }
        SECTION("invalid readOnly keys")
        {
            invalidateFootprint(resources.footprint.readOnly,
                                testValidExtendOp);
        }
        SECTION("invalid readWrite keys")
        {
            invalidateFootprint(resources.footprint.readWrite,
                                testValidExtendOp);
        }
    }

    SECTION("restoreOp")
    {
        resources.footprint.readWrite = resources.footprint.readOnly;
        resources.footprint.readOnly.clear();
        SECTION("valid")
        {
            testValidRestoreOp(true);
        }
        SECTION("readOnly set with Soroban key")
        {
            resources.footprint.readOnly.emplace_back(persistentKey);
            testValidRestoreOp(false);
        }
        SECTION("invalid readOnly keys")
        {
            invalidateFootprint(resources.footprint.readOnly,
                                testValidRestoreOp);
        }
        SECTION("invalid readWrite keys")
        {
            invalidateFootprint(resources.footprint.readWrite,
                                testValidRestoreOp);
        }
    }
}

TEST_CASE("non-refundable resource metering", "[tx][soroban]")
{
    uniform_int_distribution<uint64_t> feeDist(1, 100'000);
    auto cfgModifyFn = [&feeDist](SorobanNetworkConfig& cfg) {
        cfg.mFeeRatePerInstructionsIncrement = feeDist(Catch::rng());
        cfg.mFeeReadLedgerEntry = feeDist(Catch::rng());
        cfg.mFeeWriteLedgerEntry = feeDist(Catch::rng());
        cfg.mFeeRead1KB = feeDist(Catch::rng());
        cfg.mFeeWrite1KB = feeDist(Catch::rng());
        cfg.mFeeHistorical1KB = feeDist(Catch::rng());
        cfg.mFeeTransactionSize1KB = feeDist(Catch::rng());
    };

    ContractInvocationTest test(rust_bridge::get_test_wasm_add_i32(),
                                /*deployContract=*/false, getTestConfig(),
                                /*useTestLimits=*/true, cfgModifyFn);

    auto scFunc = makeSymbol("add");
    auto scArgs = {makeI32(7), makeI32(16)};

    SorobanResources resources;
    resources.instructions = 0;
    resources.readBytes = 0;
    resources.writeBytes = 0;

    auto checkFees = [&](int64_t expectedNonRefundableFee,
                         std::shared_ptr<TransactionFrameBase> rootTX) {
        test.txCheckValid(rootTX);
        {
            auto& app = *test.getApp();
            LedgerTxn ltx(app.getLedgerTxnRoot());
            auto actualFeePair =
                std::dynamic_pointer_cast<TransactionFrame>(rootTX)
                    ->computePreApplySorobanResourceFee(
                        ltx.loadHeader().current().ledgerVersion,
                        app.getLedgerManager().getSorobanNetworkConfig(ltx),
                        app.getConfig());
            REQUIRE(expectedNonRefundableFee ==
                    actualFeePair.non_refundable_fee);
        }

        auto inclusionFee = [&] {
            LedgerTxn ltx(test.getApp()->getLedgerTxnRoot());
            return getMinInclusionFee(*rootTX, ltx.getHeader());
        }();

        // Check that minimum fee succeeds
        auto minimalTX = test.createInvokeTx(
            resources, scFunc, scArgs, inclusionFee, expectedNonRefundableFee);
        REQUIRE(test.isTxValid(minimalTX));

        // Check that just below minimum resource fee fails
        auto badTX =
            test.createInvokeTx(resources, scFunc, scArgs, inclusionFee,
                                expectedNonRefundableFee - 1);
        REQUIRE(!test.isTxValid(badTX));

        // check that just below minimum inclusion fee fails
        auto badTX2 = test.createInvokeTx(
            resources, scFunc, scArgs, inclusionFee - 1,
            // It doesn't matter how high the resource fee is.
            expectedNonRefundableFee + 1'000'000);
        REQUIRE(!test.isTxValid(badTX2));
    };

    // In the following tests, we isolate a single fee to test by zeroing out
    // every other resource, as much as is possible
    SECTION("tx size fees")
    {
        auto tx =
            test.createInvokeTx(resources, scFunc, scArgs, 100,
                                std::numeric_limits<uint32_t>::max() - 100);

        // Resources are all null, only include TX size based fees
        auto expectedFee = test.getTxSizeFees(tx);
        checkFees(expectedFee, tx);
    }

    SECTION("compute fee")
    {
        resources.instructions = feeDist(Catch::rng());
        auto tx =
            test.createInvokeTx(resources, scFunc, scArgs, 100,
                                std::numeric_limits<uint32_t>::max() - 100);

        // Should only have instruction and TX size fees
        auto expectedFee =
            test.getTxSizeFees(tx) + test.getComputeFee(resources);
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

            auto tx =
                test.createInvokeTx(resources, scFunc, scArgs, 100,
                                    std::numeric_limits<uint32_t>::max() - 100);

            // Only read entry fees and TX size fees
            auto expectedFee =
                test.getTxSizeFees(tx) + test.getEntryReadFee(resources);
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

            auto tx =
                test.createInvokeTx(resources, scFunc, scArgs, 100,
                                    std::numeric_limits<uint32_t>::max() - 100);

            // Only read entry, write entry, and TX size fees
            auto expectedFee = test.getTxSizeFees(tx) +
                               test.getEntryReadFee(resources) +
                               test.getEntryWriteFee(resources);
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

            auto tx =
                test.createInvokeTx(resources, scFunc, scArgs, 100,
                                    std::numeric_limits<uint32_t>::max() - 100);

            // Only read entry, write entry, and TX size fees
            auto expectedFee = test.getTxSizeFees(tx) +
                               test.getEntryReadFee(resources) +
                               test.getEntryWriteFee(resources);
            checkFees(expectedFee, tx);
        }
    }

    uniform_int_distribution<uint32_t> bytesDist(1, 100'000);
    SECTION("readBytes fee")
    {
        resources.readBytes = bytesDist(Catch::rng());

        auto tx =
            test.createInvokeTx(resources, scFunc, scArgs, 100,
                                std::numeric_limits<uint32_t>::max() - 100);

        // Only readBytes and TX size fees
        auto expectedFee =
            test.getTxSizeFees(tx) + test.getReadBytesFee(resources);
        checkFees(expectedFee, tx);
    }

    SECTION("writeBytes fee")
    {
        resources.writeBytes = bytesDist(Catch::rng());
        auto tx =
            test.createInvokeTx(resources, scFunc, scArgs, 100,
                                std::numeric_limits<uint32_t>::max() - 100);

        // Only writeBytes and TX size fees
        auto expectedFee =
            test.getTxSizeFees(tx) + test.getWriteBytesFee(resources);
        checkFees(expectedFee, tx);
    }
}

TEST_CASE("refund account merged", "[tx][soroban][merge]")
{
    ContractInvocationTest test(rust_bridge::get_test_wasm_add_i32(),
                                /*deployContact=*/false);

    const int64_t startingBalance =
        test.getApp()->getLedgerManager().getLastMinBalance(50);

    auto a1 = test.getRoot().create("A", startingBalance);
    auto b1 = test.getRoot().create("B", startingBalance);
    auto c1 = test.getRoot().create("C", startingBalance);

    auto tx = test.createUploadWasmTx(a1, DEFAULT_TEST_RESOURCE_FEE);

    auto mergeOp = accountMerge(b1);
    mergeOp.sourceAccount.activate() = toMuxedAccount(a1);

    auto classicMergeTx = c1.tx({mergeOp});
    classicMergeTx->addSignature(a1.getSecretKey());

    auto r = closeLedger(*test.getApp(), {classicMergeTx, tx});
    checkTx(0, r, txSUCCESS);

    // The source account of the soroban tx was merged during the classic phase
    checkTx(1, r, txNO_ACCOUNT);
}

TEST_CASE("buying liabilities plus refund is greater than INT64_MAX",
          "[tx][soroban][offer]")
{
    ContractInvocationTest test(rust_bridge::get_test_wasm_add_i32(),
                                /*deployContact=*/false);

    const int64_t startingBalance =
        test.getApp()->getLedgerManager().getLastMinBalance(50);

    auto a1 = test.getRoot().create("A", startingBalance);
    auto b1 = test.getRoot().create("B", startingBalance);

    auto native = txtest::makeNativeAsset();
    auto cur1 = txtest::makeAsset(test.getRoot(), "CUR1");
    a1.changeTrust(cur1, INT64_MAX);
    test.getRoot().pay(a1, cur1, INT64_MAX);

    auto a1PreBalance = a1.getBalance();

    auto tx = test.createUploadWasmTx(a1, 300'000);
    // There is no surge pricing, so only 100 stroops are charged from the
    // inclusion fee.
    auto feeChargedBeforeRefund =
        tx->getFullFee() - tx->getInclusionFee() + 100;
    // Create a maximal possible offer that wouldn't overflow int64. The offer
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

    auto r = closeLedger(*test.getApp(), {offerTx, tx});
    checkTx(0, r, txSUCCESS);
    checkTx(1, r, txSUCCESS);

    // After an offer is created, the sum of `a1` balance and buying liability
    // is `INT64_MAX`, thus any fee refund would cause an overflow, so no
    // refunds happen.
    REQUIRE(a1PreBalance - feeChargedBeforeRefund == a1.getBalance());
}

TEST_CASE("failure diagnostics", "[tx][soroban]")
{
    auto cfg = getTestConfig();
    cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS = true;
    ContractInvocationTest test(rust_bridge::get_test_wasm_add_i32(),
                                /*deployContact=*/true, cfg);

    auto scFunc = makeSymbol("add");
    auto sc1 = makeI32(7);
    auto scMax = makeI32(INT32_MAX);
    SorobanResources resources;
    resources.footprint.readOnly = test.getContractKeys();
    resources.instructions = 2'000'000;
    resources.readBytes = 2000;
    resources.writeBytes = 1000;

    // This test calls the add_i32 contract with two numbers that cause an
    // overflow. Because we have diagnostics on, we will see two events - The
    // diagnostic "fn_call" event, and the event that the add_i32 contract
    // emits.
    SECTION("failed invocation")
    {
        auto tx = test.createInvokeTx(resources, scFunc, {sc1, scMax}, 1000,
                                      DEFAULT_TEST_RESOURCE_FEE);
        test.txCheckValid(tx);
        auto txm = test.invokeTx(tx, /*expectSuccess=*/false);

        auto const& opEvents = txm->getXDR().v3().sorobanMeta->diagnosticEvents;
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

    // This test produces a diagnostic event _during validation_, i.e. before
    // it ever makes it to the soroban host.
    SECTION("invalid tx")
    {
        // Instructions beyond max
        resources.instructions = 2'000'000'000;

        auto tx = test.createInvokeTx(resources, scFunc, {sc1, scMax}, 1000,
                                      DEFAULT_TEST_RESOURCE_FEE);
        REQUIRE(!test.isTxValid(tx));

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
}

TEST_CASE("errors roll back", "[tx][soroban]")
{
    // This tests that various sorts of error created inside a contract
    // cause the invocation to fail and that in turn aborts the tx.
    auto call_fn_check_failure = [](std::string const& name) {
        ContractInvocationTest test(rust_bridge::get_test_wasm_err());

        SorobanResources resources;
        resources.footprint.readOnly = test.getContractKeys();
        resources.instructions = 3'000'000;
        resources.readBytes = 3000;

        auto tx = test.createInvokeTx(resources, makeSymbol(name), {}, 100,
                                      DEFAULT_TEST_RESOURCE_FEE);
        test.txCheckValid(tx);
        test.invokeTx(tx, /*expectSuccess=*/false);

        REQUIRE(tx->getResult()
                    .result.results()
                    .at(0)
                    .tr()
                    .invokeHostFunctionResult()
                    .code() == INVOKE_HOST_FUNCTION_TRAPPED);
        REQUIRE(tx->getResultCode() == txFAILED);
    };

    for (auto const& name :
         {"err_eek", "err_err", "ok_err", "ok_val_err", "err", "val"})
    {
        call_fn_check_failure(name);
    }
}

TEST_CASE("settings upgrade", "[tx][soroban][upgrades]")
{
    auto cfg = getTestConfig();
    cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS = true;
    ContractInvocationTest test(rust_bridge::get_write_bytes(),
                                /*deployContract=*/false, cfg,
                                /*useTestLimits=*/false);

    auto runTest = [&]() {
        {
            // make sure LedgerManager picked up cached values by looking at
            // a couple settings
            auto const& networkConfig = test.getNetworkCfg();
            REQUIRE(networkConfig.txMaxReadBytes() ==
                    MinimumSorobanNetworkConfig::TX_MAX_READ_BYTES);
            REQUIRE(networkConfig.ledgerMaxReadBytes() ==
                    networkConfig.txMaxReadBytes());
        }

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

        test.deployWithResources(uploadResources, createResources);

        // build upgrade

        // This test assumes that all settings including and after
        // CONFIG_SETTING_BUCKETLIST_SIZE_WINDOW are not upgradeable, so they
        // won't be included in the upgrade.
        xdr::xvector<ConfigSettingEntry> updatedEntries;
        for (uint32_t i = 0;
             i < static_cast<uint32_t>(CONFIG_SETTING_BUCKETLIST_SIZE_WINDOW);
             ++i)
        {
            LedgerTxn ltx(test.getApp()->getLedgerTxnRoot());
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

        LedgerKey upgrade(CONTRACT_DATA);
        upgrade.contractData().durability = TEMPORARY;
        upgrade.contractData().contract = test.getContractID();
        upgrade.contractData().key =
            makeBytes(xdr::xdr_to_opaque(upgrade_hash));

        SorobanResources resources{};
        resources.footprint.readOnly = test.getContractKeys();
        resources.footprint.readWrite = {upgrade};
        resources.instructions = 2'000'000;
        resources.readBytes = 3000;
        resources.writeBytes = 2000;

        auto tx = test.createInvokeTx(resources, makeSymbol("write"),
                                      {makeBytes(xdr)}, 1000, 20'000'000);

        test.txCheckValid(tx);
        test.invokeTx(tx, /*expectSuccess=*/true, /*processPostApply=*/false);

        {
            // verify that the contract code, contract instance, and upgrade
            // entry were all extended by
            // 1036800 ledgers (60 days) -
            // https://github.com/stellar/rs-soroban-env/blob/main/soroban-test-wasms/wasm-workspace/write_upgrade_bytes/src/lib.rs#L3-L5
            auto ledgerSeq = test.getLedgerSeq();
            auto extendedKeys = test.getContractKeys();
            extendedKeys.emplace_back(upgrade);

            REQUIRE(extendedKeys.size() == 3);
            for (auto const& key : extendedKeys)
            {
                test.checkTTL(key, ledgerSeq + 1036800);
            }
        }

        // arm the upgrade through commandHandler. This isn't required
        // because we'll trigger the upgrade through externalizeValue, but
        // this will test the submission and deserialization code.
        ConfigUpgradeSetKey key;
        key.contentHash = upgrade_hash;
        key.contractID = test.getContractID().contractId();

        auto& commandHandler = test.getApp()->getCommandHandler();

        std::string command = "mode=set&configupgradesetkey=";
        command += decoder::encode_b64(xdr::xdr_to_opaque(key));
        command += "&upgradetime=2000-07-21T22:04:00Z";

        std::string ret;
        commandHandler.upgrades(command, ret);
        REQUIRE(ret == "");

        // trigger upgrade
        auto ledgerUpgrade = LedgerUpgrade{LEDGER_UPGRADE_CONFIG};
        ledgerUpgrade.newConfig() = key;

        auto const& lcl =
            test.getApp()->getLedgerManager().getLastClosedLedgerHeader();
        auto txSet = TxSetFrame::makeEmpty(lcl);
        auto lastCloseTime = lcl.header.scpValue.closeTime;

        test.getApp()->getHerder().externalizeValue(
            txSet, lcl.header.ledgerSeq + 1, lastCloseTime,
            {LedgerTestUtils::toUpgradeType(ledgerUpgrade)});

        // validate upgrade succeeded
        {
            auto costKey = configSettingKey(
                ConfigSettingID::CONFIG_SETTING_CONTRACT_LEDGER_COST_V0);
            LedgerTxn ltx(test.getApp()->getLedgerTxnRoot());
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
        overrideNetworkSettingsToMin(*test.getApp());
        runTest();
    }
}

TEST_CASE("complex contract", "[tx][soroban]")
{
    auto complexTest = [&](bool enableDiagnostics) {
        auto cfg = getTestConfig();
        cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS = enableDiagnostics;
        ContractInvocationTest test(rust_bridge::get_test_wasm_complex(),
                                    /*deployContact=*/true, cfg);

        // Contract writes a single `data` CONTRACT_DATA entry.
        LedgerKey dataKey(LedgerEntryType::CONTRACT_DATA);
        dataKey.contractData().contract = test.getContractID();
        dataKey.contractData().key = makeSymbolSCVal("data");

        SorobanResources resources;
        resources.footprint.readOnly = test.getContractKeys();
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

        auto tx = test.createInvokeTx(resources, makeSymbol("go"), {}, 100,
                                      DEFAULT_TEST_RESOURCE_FEE);
        test.txCheckValid(tx);
        auto txm = test.invokeTx(tx, /*expectSuccess=*/true);

        // Contract should have emitted a single event carrying a `Bytes`
        // value.
        REQUIRE(txm->getXDR().v3().sorobanMeta->events.size() == 1);
        REQUIRE(txm->getXDR().v3().sorobanMeta->events.at(0).type ==
                ContractEventType::CONTRACT);
        REQUIRE(txm->getXDR()
                    .v3()
                    .sorobanMeta->events.at(0)
                    .body.v0()
                    .data.type() == SCV_BYTES);

        if (enableDiagnostics)
        {
            verifyDiagnosticEvents(
                txm->getXDR().v3().sorobanMeta->diagnosticEvents);
        }
        else
        {
            REQUIRE(txm->getXDR().v3().sorobanMeta->diagnosticEvents.size() ==
                    0);
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

TEST_CASE("contract storage", "[tx][soroban]")
{
    ContractStorageInvocationTest test{};
    auto const& stateArchivalSettings =
        test.getNetworkCfg().stateArchivalSettings();
    auto ledgerSeq = test.getLedgerSeq();

    SECTION("default limits")
    {
        // Test writing an entry
        test.put("key1", ContractDataDurability::PERSISTENT, 0);
        REQUIRE(test.get("key1", ContractDataDurability::PERSISTENT) == 0);
        test.put("key2", ContractDataDurability::PERSISTENT, 21);
        REQUIRE(test.get("key2", ContractDataDurability::PERSISTENT) == 21);

        // Test overwriting an entry
        test.put("key1", ContractDataDurability::PERSISTENT, 9);
        REQUIRE(test.get("key1", ContractDataDurability::PERSISTENT) == 9);
        test.put("key2", ContractDataDurability::PERSISTENT, UINT64_MAX);
        REQUIRE(test.get("key2", ContractDataDurability::PERSISTENT) ==
                UINT64_MAX);

        // Test deleting an entry
        test.del("key1", ContractDataDurability::PERSISTENT);
        REQUIRE(!test.has("key1", ContractDataDurability::PERSISTENT));
        test.del("key2", ContractDataDurability::PERSISTENT);
        REQUIRE(!test.has("key2", ContractDataDurability::PERSISTENT));
    }

    SECTION("failure: entry size exceeds write bytes")
    {
        // 5kb write should fail because it exceeds the write bytes
        test.resizeStorageAndExtend("key", 5, 1, 1, 5'000, 40'000,
                                    /*expectSuccess=*/false);

        // 5kb write should succeed with high write bytes value
        test.resizeStorageAndExtend("key", 5, 1, 1, 10'000, 40'000,
                                    /*expectSuccess=*/true);
    }

    SECTION("Same ScVal key, different types")
    {
        // Check that each type is in their own keyspace
        test.put("key", ContractDataDurability::PERSISTENT, 1);
        test.put("key", ContractDataDurability::TEMPORARY, 2);
        REQUIRE(test.get("key", ContractDataDurability::PERSISTENT) == 1);
        REQUIRE(test.get("key", ContractDataDurability::TEMPORARY) == 2);
    }

    SECTION("contract instance and wasm archival")
    {
        uint32_t originalExpectedLiveUntilLedger =
            stateArchivalSettings.minPersistentTTL + ledgerSeq - 1;

        for (uint32_t i =
                 test.getApp()->getLedgerManager().getLastClosedLedgerNum();
             i <= originalExpectedLiveUntilLedger + 1; ++i)
        {
            closeLedgerOn(*test.getApp(), i, 2, 1, 2016);
        }

        // Contract instance and code should be expired
        ledgerSeq = test.getLedgerSeq();
        auto const& contractKeys = test.getContractKeys();
        REQUIRE(!test.isEntryLive(contractKeys[0], ledgerSeq));
        REQUIRE(!test.isEntryLive(contractKeys[1], ledgerSeq));

        // Contract instance and code are expired, any TX should fail
        test.put("temp", ContractDataDurability::TEMPORARY, 0,
                 /*expectSuccess=*/false);

        auto newExpectedLiveUntilLedger =
            stateArchivalSettings.minPersistentTTL + ledgerSeq - 1;

        SECTION("restore contract instance and wasm")
        {
            // Restore Instance and Wasm
            test.restoreOp(contractKeys,
                           96 /* rent bump */ + 40000 /* two LE-writes */);

            // Instance should now be useable
            test.put("temp", ContractDataDurability::TEMPORARY, 0);
            test.checkTTL(contractKeys[0], newExpectedLiveUntilLedger);
            test.checkTTL(contractKeys[1], newExpectedLiveUntilLedger);
        }

        SECTION("restore contract instance, not wasm")
        {
            // Only restore contract instance
            test.restoreOp({contractKeys[0]},
                           48 /* rent bump */ + 20000 /* one LE write */);

            // invocation should fail
            test.put("temp", ContractDataDurability::TEMPORARY, 0,
                     /*expectSuccess=*/false);
            test.checkTTL(contractKeys[0], newExpectedLiveUntilLedger);
            test.checkTTL(contractKeys[1], originalExpectedLiveUntilLedger);
        }

        SECTION("restore contract wasm, not instance")
        {
            // Only restore Wasm
            test.restoreOp({contractKeys[1]},
                           48 /* rent bump */ + 20000 /* one LE write */);

            // invocation should fail
            test.put("temp", ContractDataDurability::TEMPORARY, 0,
                     /*expectSuccess=*/false);
            test.checkTTL(contractKeys[0], originalExpectedLiveUntilLedger);
            test.checkTTL(contractKeys[1], newExpectedLiveUntilLedger);
        }

        SECTION("lifetime extensions")
        {
            // Restore Instance and Wasm
            test.restoreOp(contractKeys,
                           96 /* rent bump */ + 40000 /* two LE writes */);

            auto instanceExtendTo = 10'000;
            auto wasmExtendTo = 15'000;

            // extend instance
            test.extendOp({contractKeys[0]}, instanceExtendTo);

            // extend Wasm
            test.extendOp({contractKeys[1]}, wasmExtendTo);

            test.checkTTL(contractKeys[0], ledgerSeq + instanceExtendTo);
            test.checkTTL(contractKeys[1], ledgerSeq + wasmExtendTo);
        }
    }

    SECTION("contract storage archival")
    {
        // Wasm and instance should not expire during test
        test.extendOp(test.getContractKeys(), 10'000);

        test.put("persistent", ContractDataDurability::PERSISTENT, 10);
        test.put("temp", ContractDataDurability::TEMPORARY, 0);

        auto expectedTempLiveUntilLedger =
            stateArchivalSettings.minTemporaryTTL + ledgerSeq - 1;
        auto expectedPersistentLiveUntilLedger =
            stateArchivalSettings.minPersistentTTL + ledgerSeq - 1;

        // Check for expected minimum lifetime values
        test.checkTTL("persistent", ContractDataDurability::PERSISTENT,
                      expectedPersistentLiveUntilLedger);

        test.checkTTL("temp", ContractDataDurability::TEMPORARY,
                      expectedTempLiveUntilLedger);

        // Close ledgers until temp entry expires
        uint32 nextLedgerSeq =
            test.getApp()->getLedgerManager().getLastClosedLedgerNum();
        for (; nextLedgerSeq <= expectedTempLiveUntilLedger; ++nextLedgerSeq)
        {
            closeLedgerOn(*test.getApp(), nextLedgerSeq, 2, 1, 2016);
        }

        ledgerSeq = test.getLedgerSeq();
        REQUIRE(ledgerSeq == expectedTempLiveUntilLedger);

        SECTION("entry accessible when currentLedger == liveUntilLedger")
        {
            // Entry should still be accessible when currentLedger ==
            // liveUntilLedgerSeq
            REQUIRE(test.has("temp", ContractDataDurability::TEMPORARY));
        }

        SECTION("write does not increase TTL")
        {
            test.put("temp", ContractDataDurability::TEMPORARY, 42);
            // TTL should not be network minimum since entry already exists
            test.checkTTL("temp", ContractDataDurability::TEMPORARY,
                          expectedTempLiveUntilLedger);
        }

        SECTION("extendOp when currentLedger == liveUntilLedger")
        {
            auto tempKey =
                contractDataKey(test.getContractID(), makeSymbolSCVal("temp"),
                                ContractDataDurability::TEMPORARY);
            test.extendOp({tempKey}, 10'000);
            test.checkTTL("temp", ContractDataDurability::TEMPORARY,
                          ledgerSeq + 10'000);
        }

        SECTION("TTL enforcement")
        {
            // Close one more ledger so entry is expired
            closeLedgerOn(*test.getApp(), nextLedgerSeq++, 2, 1, 2016);
            ledgerSeq = test.getLedgerSeq();
            REQUIRE(ledgerSeq == expectedTempLiveUntilLedger + 1);

            // Check that temp entry has expired
            REQUIRE(!test.isEntryLive("temp", ContractDataDurability::TEMPORARY,
                                      ledgerSeq));

            // Get should fail since entry no longer exists
            test.get("temp", ContractDataDurability::TEMPORARY,
                     /*expectSuccess=*/false);

            // Has should succeed since the entry is TEMPORARY, but should
            // return false
            REQUIRE(!test.has("temp", ContractDataDurability::TEMPORARY));

            // PERSISTENT entry is still live, has higher minimum TTL
            REQUIRE(test.isEntryLive(
                "persistent", ContractDataDurability::PERSISTENT, ledgerSeq));
            REQUIRE(test.has("persistent", ContractDataDurability::PERSISTENT));

            // Check that we can recreate an expired TEMPORARY entry
            test.put("temp", ContractDataDurability::TEMPORARY, 42);

            // Recreated entry should be live
            REQUIRE(test.get("temp", ContractDataDurability::TEMPORARY) == 42);
            test.checkTTL("temp", ContractDataDurability::TEMPORARY,
                          stateArchivalSettings.minTemporaryTTL + ledgerSeq -
                              1);

            // Close ledgers until PERSISTENT entry liveUntilLedger
            for (; nextLedgerSeq <= expectedPersistentLiveUntilLedger;
                 ++nextLedgerSeq)
            {
                closeLedgerOn(*test.getApp(), nextLedgerSeq, 2, 1, 2016);
            }

            // Entry should still be accessible when currentLedger ==
            // liveUntilLedgerSeq
            REQUIRE(test.has("persistent", ContractDataDurability::PERSISTENT));

            auto lk = contractDataKey(test.getContractID(),
                                      makeSymbolSCVal("persistent"),
                                      ContractDataDurability::PERSISTENT);
            SECTION("restoreOp skips when currentLedger == liveUntilLedger")
            {
                // Restore should skip entry, refund all of refundableFee
                test.restoreOp({lk}, 0);

                // TTL should be unchanged
                test.checkTTL("persistent", ContractDataDurability::PERSISTENT,
                              expectedPersistentLiveUntilLedger);
            }

            // Close one more ledger so entry is expired
            closeLedgerOn(*test.getApp(), nextLedgerSeq++, 2, 1, 2016);
            REQUIRE(
                test.getApp()->getLedgerManager().getLastClosedLedgerNum() ==
                expectedPersistentLiveUntilLedger + 1);
            ledgerSeq = test.getLedgerSeq();

            // Check that persistent entry has expired
            REQUIRE(!test.isEntryLive(
                "persistent", ContractDataDurability::PERSISTENT, ledgerSeq));

            // Check that we can't recreate expired PERSISTENT
            test.put("persistent", ContractDataDurability::PERSISTENT, 42,
                     /*expectSuccess=*/false);

            // Since entry is PERSISTENT, has should fail
            test.has("persistent", ContractDataDurability::PERSISTENT,
                     /*expectSuccess=*/false);

            test.put("persistent2", ContractDataDurability::PERSISTENT, 0);
            auto lk2 = contractDataKey(test.getContractID(),
                                       makeSymbolSCVal("persistent2"),
                                       ContractDataDurability::PERSISTENT);
            test.checkTTL("persistent2", ContractDataDurability::PERSISTENT,
                          stateArchivalSettings.minPersistentTTL + ledgerSeq -
                              1);

            // Restore ARCHIVED key and LIVE key, should only be charged for one
            test.restoreOp({lk, lk2}, /*charge for one entry*/ 20048);

            // Live entry TTL should be unchanged
            test.checkTTL("persistent2", ContractDataDurability::PERSISTENT,
                          stateArchivalSettings.minPersistentTTL + ledgerSeq -
                              1);

            // Check value and TTL of restored entry
            test.checkTTL("persistent", ContractDataDurability::PERSISTENT,
                          stateArchivalSettings.minPersistentTTL + ledgerSeq -
                              1);
            REQUIRE(test.get("persistent",
                             ContractDataDurability::PERSISTENT) == 10);
        }
    }

    SECTION("conditional TTL extension")
    {
        // Large bump, followed by smaller bump
        test.put("key", ContractDataDurability::PERSISTENT, 0);
        test.extendHostFunction("key", ContractDataDurability::PERSISTENT,
                                10'000, 10'000);
        test.checkTTL("key", ContractDataDurability::PERSISTENT,
                      ledgerSeq + 10'000);

        // Expiration already above 5'000, should be a no-op
        test.extendHostFunction("key", ContractDataDurability::PERSISTENT,
                                5'000, 5'000);
        test.checkTTL("key", ContractDataDurability::PERSISTENT,
                      ledgerSeq + 10'000);

        // Small bump followed by larger bump
        test.put("key2", ContractDataDurability::PERSISTENT, 0);
        test.extendHostFunction("key2", ContractDataDurability::PERSISTENT,
                                10'000, 10'000);
        test.checkTTL("key2", ContractDataDurability::PERSISTENT,
                      ledgerSeq + 10'000);

        test.put("key3", ContractDataDurability::PERSISTENT, 0);
        test.extendHostFunction("key3", ContractDataDurability::PERSISTENT,
                                50'000, 50'000);
        test.checkTTL("key3", ContractDataDurability::PERSISTENT,
                      ledgerSeq + 50'000);

        // Bump multiple keys to live 10100 ledger from now
        xdr::xvector<LedgerKey> keysToExtend = {
            contractDataKey(test.getContractID(), makeSymbolSCVal("key"),
                            ContractDataDurability::PERSISTENT),
            contractDataKey(test.getContractID(), makeSymbolSCVal("key2"),
                            ContractDataDurability::PERSISTENT),
            contractDataKey(test.getContractID(), makeSymbolSCVal("key3"),
                            ContractDataDurability::PERSISTENT)};

        SECTION("calculate refund")
        {
            test.extendOp(keysToExtend, 10'100);
            test.checkTTL("key", ContractDataDurability::PERSISTENT,
                          ledgerSeq + 10'100);
            test.checkTTL("key2", ContractDataDurability::PERSISTENT,
                          ledgerSeq + 10'100);

            // No change for key3 since expiration is already past 10100 ledgers
            // from now
            test.checkTTL("key3", ContractDataDurability::PERSISTENT,
                          ledgerSeq + 50'000);
        }

        // Check same extendOp with hardcoded expected refund to detect if
        // refund logic changes unexpectedly
        SECTION("absolute refund")
        {
            test.extendOp(keysToExtend, 10'100, /*expectSuccess=*/true, 40096);
            test.checkTTL("key", ContractDataDurability::PERSISTENT,
                          ledgerSeq + 10'100);
            test.checkTTL("key2", ContractDataDurability::PERSISTENT,
                          ledgerSeq + 10'100);

            // No change for key3 since expiration is already past 10100 ledgers
            // from now
            test.checkTTL("key3", ContractDataDurability::PERSISTENT,
                          ledgerSeq + 50'000);
        }
    }

    uint32_t initialLiveUntilLedger =
        stateArchivalSettings.minPersistentTTL + ledgerSeq - 1;

    SECTION("TTL threshold")
    {
        test.put("key", ContractDataDurability::PERSISTENT, 0);
        test.checkTTL("key", ContractDataDurability::PERSISTENT,
                      initialLiveUntilLedger);

        // After the put op, key4's lifetime will be minimumLifetime - 1.
        // Set low lifetime to minimumLifetime - 2 so bump does not occur
        test.extendHostFunction("key", ContractDataDurability::PERSISTENT,
                                stateArchivalSettings.minPersistentTTL - 2,
                                50'000);
        test.checkTTL("key", ContractDataDurability::PERSISTENT,
                      initialLiveUntilLedger);

        closeLedgerOn(*test.getApp(), ++ledgerSeq, 2, 1, 2016);

        // Lifetime is now at low threshold, should be bumped
        test.extendHostFunction("key", ContractDataDurability::PERSISTENT,
                                stateArchivalSettings.minPersistentTTL - 2,
                                50'000);
        test.checkTTL("key", ContractDataDurability::PERSISTENT,
                      50'000 + ledgerSeq);

        // Check that threshold > extendTo fails
        test.extendHostFunction("key", ContractDataDurability::PERSISTENT,
                                60'001, 60'000,
                                /*expectSuccess=*/false);
    }

    SECTION("max TTL extension")
    {
        test.put("key", ContractDataDurability::PERSISTENT, 0);
        auto lk = contractDataKey(test.getContractID(), makeSymbolSCVal("key"),
                                  ContractDataDurability::PERSISTENT);

        SECTION("extension op")
        {
            test.extendOp({lk}, stateArchivalSettings.maxEntryTTL,
                          /*expectSuccess=*/false);
            test.checkTTL(lk, initialLiveUntilLedger);

            // Max TTL includes current ledger, so subtract 1
            test.extendOp({lk}, stateArchivalSettings.maxEntryTTL - 1);
            test.checkTTL(lk,
                          stateArchivalSettings.maxEntryTTL - 1 + ledgerSeq);
        }

        SECTION("extend host function")
        {
            test.extendHostFunction("key", ContractDataDurability::PERSISTENT,
                                    stateArchivalSettings.maxEntryTTL,
                                    stateArchivalSettings.maxEntryTTL,
                                    /*expectSuccess=*/false);
            test.checkTTL(lk, initialLiveUntilLedger);

            // Max TTL includes current ledger, so subtract 1
            test.extendHostFunction("key", ContractDataDurability::PERSISTENT,
                                    stateArchivalSettings.maxEntryTTL - 1,
                                    stateArchivalSettings.maxEntryTTL - 1,
                                    /*expectSuccess=*/true);
            test.checkTTL(lk,
                          stateArchivalSettings.maxEntryTTL - 1 + ledgerSeq);
        }
    }

    SECTION("charge rent fees for storage resize")
    {
        auto resizeKilobytes = 1;
        test.put("key1", ContractDataDurability::PERSISTENT, 0);
        auto key1lk = contractDataKey(test.getContractID(),
                                      makeSymbolSCVal("key1"), PERSISTENT);

        auto startingSizeBytes = 0;
        {
            LedgerTxn ltx(test.getApp()->getLedgerTxnRoot());
            auto txle = ltx.loadWithoutRecord(key1lk);
            REQUIRE(txle);
            startingSizeBytes = xdr::xdr_size(txle.current());
        }

        // First, resize a key with large fee to guarantee success
        test.resizeStorageAndExtend("key1", resizeKilobytes, 0, 0, 10'000,
                                    40'000,
                                    /*expectSuccess=*/true);

        auto sizeDeltaBytes = 0;
        {
            LedgerTxn ltx(test.getApp()->getLedgerTxnRoot());
            auto txle = ltx.loadWithoutRecord(key1lk);
            REQUIRE(txle);
            sizeDeltaBytes = xdr::xdr_size(txle.current()) - startingSizeBytes;
        }

        // Now that we know the size delta, we can calculate the expected rent
        // and check that another entry resize charges fee correctly
        test.put("key2", ContractDataDurability::PERSISTENT, 0);

        auto initialLifetime = stateArchivalSettings.minPersistentTTL;

        SECTION("resize with no extend")
        {
            auto expectedRentFee =
                test.getRentFeeForBytes(sizeDeltaBytes, initialLifetime,
                                        /*isPersistent=*/true);

            // resourceFee = rent fee + (event size + return val) fee. So in
            // order to success resourceFee needs to be expectedRentFee
            // + 1 to account for the return val size. We are not changing
            // the liveUntilLedgerSeq, so there is no TTL Entry write
            // charge
            auto resourceFee = expectedRentFee + 1;
            test.resizeStorageAndExtend("key2", resizeKilobytes, 0, 0, 3'000,
                                        resourceFee - 1,
                                        /*expectSuccess=*/false);

            // Size change should succeed with enough refundable fee
            test.resizeStorageAndExtend("key2", resizeKilobytes, 0, 0, 3'000,
                                        resourceFee, /*expectSuccess=*/true);
        }

        SECTION("resize and extend")
        {
            auto newLifetime = initialLifetime + 1000;

            // New bytes are charged rent for previous lifetime and new lifetime
            auto resizeRentFee =
                test.getRentFeeForBytes(sizeDeltaBytes, newLifetime,
                                        /*isPersistent=*/true);

            // Initial bytes just charged for new lifetime
            auto rentFee = test.getRentFeeForBytes(
                startingSizeBytes, newLifetime - initialLifetime,
                /*isPersistent=*/true);

            // refundableFee = rent fee + (event size + return val) fee. So in
            // order to success refundableFee needs to be expectedRentFee +
            // 1 to  account for the return val size.
            auto refundableFee =
                resizeRentFee + rentFee + test.getTTLEntryWriteFee() + 1;

            test.resizeStorageAndExtend("key2", resizeKilobytes, newLifetime,
                                        newLifetime, 3'000, refundableFee - 1,
                                        /*expectSuccess=*/false);

            // Size change should succeed with enough refundable fee
            test.resizeStorageAndExtend("key2", resizeKilobytes, newLifetime,
                                        newLifetime, 3'000, refundableFee,
                                        /*expectSuccess=*/true);
        }
    }

    SECTION("footprint tests")
    {
        test.put("key", ContractDataDurability::PERSISTENT, 0);
        auto lk = contractDataKey(test.getContractID(), makeSymbolSCVal("key"),
                                  ContractDataDurability::PERSISTENT);

        SECTION("unused readWrite key")
        {
            auto acc = test.getRoot().create(
                "acc", test.getApp()->getLedgerManager().getLastMinBalance(1));

            REQUIRE(doesAccountExist(*test.getApp(), acc));
            test.putWithFootprint("key", ContractDataDurability::PERSISTENT, 0,
                                  test.getContractKeys(), {lk, accountKey(acc)},
                                  /*expectSuccess*/ true);

            // make sure account still exists and hasn't change
            REQUIRE(doesAccountExist(*test.getApp(), acc));
        }

        SECTION("incorrect footprint")
        {
            // Failure: contract data isn't in footprint
            test.putWithFootprint("key", ContractDataDurability::PERSISTENT, 88,
                                  test.getContractKeys(), {}, false);
            test.getWithFootprint("key", ContractDataDurability::PERSISTENT,
                                  test.getContractKeys(), {}, false);
            test.hasWithFootprint("key", ContractDataDurability::PERSISTENT,
                                  test.getContractKeys(), {}, false);
            test.delWithFootprint("key", ContractDataDurability::PERSISTENT,
                                  test.getContractKeys(), {}, false);

            // Failure: contract data is read only
            auto readOnlyFootprint = test.getContractKeys();
            readOnlyFootprint.push_back(lk);
            test.putWithFootprint("key", ContractDataDurability::PERSISTENT, 88,
                                  readOnlyFootprint, {}, false);
            test.delWithFootprint("key", ContractDataDurability::PERSISTENT,
                                  readOnlyFootprint, {}, false);

            // Failure: Contract Wasm/instance not included
            test.putWithFootprint("key", ContractDataDurability::PERSISTENT, 88,
                                  {}, {lk}, false);
            test.putWithFootprint("key", ContractDataDurability::PERSISTENT, 88,
                                  {test.getContractKeys()[0]}, {lk}, false);
            test.putWithFootprint("key", ContractDataDurability::PERSISTENT, 88,
                                  {test.getContractKeys()[1]}, {lk}, false);
        }

        SECTION("resource limits")
        {
            // Too few write bytes, should fail
            test.putWithFootprint(
                "key2", ContractDataDurability::PERSISTENT, 42,
                {test.getContractKeys()},
                {contractDataKey(test.getContractID(), makeSymbolSCVal("key2"),
                                 ContractDataDurability::PERSISTENT)},
                /*expectSuccess=*/false, /*writeBytes=*/1);
            REQUIRE(!test.has("key2", ContractDataDurability::PERSISTENT));

            // Resize with too few writeBytes
            test.resizeStorageAndExtend("key", /*numKiloBytes=*/5, 0, 0, 4'000,
                                        300'000,
                                        /*expectSuccess=*/false);

            // Resize with enough writeBytes
            test.resizeStorageAndExtend("key", /*numKiloBytes=*/5, 0, 0, 6'000,
                                        300'000,
                                        /*expectSuccess=*/true);

            // Reading the entry should fail with insufficient readBytes
            test.getWithFootprint("key", ContractDataDurability::PERSISTENT,
                                  {test.getContractKeys()}, {lk},
                                  /*expectSuccess=*/false,
                                  /*readBytes=*/5'000);
        }
    }
}

TEST_CASE("temp entry eviction", "[tx][soroban]")
{
    Config cfg = getTestConfig();
    TmpDirManager tdm(std::string("soroban-storage-meta-") +
                      binToHex(randomBytes(8)));
    TmpDir td = tdm.tmpDir("soroban-meta-ok");
    std::string metaPath = td.getName() + "/stream.xdr";

    cfg.METADATA_OUTPUT_STREAM = metaPath;

    ContractStorageInvocationTest test{cfg};
    auto const& contractKeys = test.getContractKeys();

    // extend Wasm and instance
    test.extendOp({contractKeys[0]}, 10'000);
    test.extendOp({contractKeys[1]}, 10'000);

    auto keySym = makeSymbolSCVal("temp");
    auto funcSym = makeSymbol("put_temporary");
    auto valU64 = makeU64SCVal(0);

    auto lk = contractDataKey(test.getContractID(), keySym, TEMPORARY);

    SorobanResources resources;
    resources.footprint.readOnly = test.getContractKeys();
    resources.footprint.readWrite = {lk};
    resources.instructions = 4'000'000;
    resources.readBytes = 10'000;
    resources.writeBytes = 1000;

    auto tx = test.createInvokeTx(resources, funcSym, {keySym, valU64}, 1000,
                                  340'000);
    closeLedger(*test.getApp(), {tx});

    auto expectedLiveUntilLedger =
        test.getApp()->getLedgerManager().getLastClosedLedgerNum() +
        InitialSorobanNetworkConfig::MINIMUM_TEMP_ENTRY_LIFETIME - 1;

    auto const evictionLedger = 4097;

    // Close ledgers until temp entry is evicted
    for (uint32_t i =
             test.getApp()->getLedgerManager().getLastClosedLedgerNum();
         i < evictionLedger; ++i)
    {
        closeLedgerOn(*test.getApp(), i, 2, 1, 2016);
    }

    test.checkTTL(lk, expectedLiveUntilLedger);

    // This should be a noop
    test.extendOp({lk}, 10'000);
    test.checkTTL(lk, expectedLiveUntilLedger);

    // This should fail because the temp entry is expired
    test.extendHostFunction("temp", ContractDataDurability::TEMPORARY, 10'000,
                            10'000, false);
    test.checkTTL(lk, expectedLiveUntilLedger);

    // Check that temp entry has expired
    auto ledgerSeq = test.getLedgerSeq();
    REQUIRE(!test.isEntryLive("temp", ContractDataDurability::TEMPORARY,
                              ledgerSeq));

    SECTION("eviction")
    {
        // close one more ledger to trigger the eviction
        closeLedgerOn(*test.getApp(), evictionLedger, 2, 1, 2016);

        {
            LedgerTxn ltx(test.getApp()->getLedgerTxnRoot());
            REQUIRE(!ltx.load(lk));
        }

        XDRInputFileStream in;
        in.open(metaPath);
        LedgerCloseMeta lcm;
        bool evicted = false;
        while (in.readOne(lcm))
        {
            REQUIRE(lcm.v() == 1);
            if (lcm.v1().ledgerHeader.header.ledgerSeq == evictionLedger)
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

    SECTION("Create temp entry with same key as an expired entry on eviction "
            "ledger")
    {
        auto tx2 = test.createInvokeTx(resources, funcSym, {keySym, valU64},
                                       1000, 340'000);
        closeLedger(*test.getApp(), {tx2});

        {
            LedgerTxn ltx(test.getApp()->getLedgerTxnRoot());
            REQUIRE(ltx.load(lk));
        }

        auto ledgerSeq = test.getLedgerSeq();

        // Verify that we're on the ledger where the entry would get evicted if
        // it wasn't recreated.
        REQUIRE(ledgerSeq == evictionLedger);

        // Entry is live again
        REQUIRE(test.isEntryLive("temp", ContractDataDurability::TEMPORARY,
                                 ledgerSeq));

        // Verify that we didn't emit an eviction
        XDRInputFileStream in;
        in.open(metaPath);
        LedgerCloseMeta lcm;
        while (in.readOne(lcm))
        {
            REQUIRE(lcm.v1().evictedTemporaryLedgerKeys.empty());
        }
    }
}

/*
This test uses the same utils (SettingsUpgradeUtils.h) as the
get-settings-upgrade-txs command to make sure the transactions have the proper
resources set.
*/
TEST_CASE("settings upgrade command line utils", "[tx][soroban][upgrades]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS = true;
    cfg.EXPERIMENTAL_BUCKETLIST_DB = false;
    auto app = createTestApplication(clock, cfg);
    auto root = TestAccount::createRoot(*app);
    auto& lm = app->getLedgerManager();

    const int64_t startingBalance =
        app->getLedgerManager().getLastMinBalance(50);

    auto a1 = root.create("A", startingBalance);

    std::vector<TransactionEnvelope> txsToSign;

    auto uploadRes =
        getUploadTx(a1.getPublicKey(), a1.getLastSequenceNumber() + 1);
    txsToSign.emplace_back(uploadRes.first);
    auto const& contractCodeLedgerKey = uploadRes.second;

    auto createRes =
        getCreateTx(a1.getPublicKey(), contractCodeLedgerKey,
                    cfg.NETWORK_PASSPHRASE, a1.getLastSequenceNumber() + 2);
    txsToSign.emplace_back(std::get<0>(createRes));
    auto const& contractSourceRefLedgerKey = std::get<1>(createRes);
    auto const& contractID = std::get<2>(createRes);

    xdr::xvector<ConfigSettingEntry> updatedEntries;
    for (uint32_t i = 0;
         i < static_cast<uint32_t>(CONFIG_SETTING_BUCKETLIST_SIZE_WINDOW); ++i)
    {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        auto entry =
            ltx.load(configSettingKey(static_cast<ConfigSettingID>(i)));
        if (entry.current().data.configSetting().configSettingID() ==
            CONFIG_SETTING_CONTRACT_LEDGER_COST_V0)
        {
            entry.current()
                .data.configSetting()
                .contractLedgerCost()
                .feeRead1KB = 1234;
        }
        updatedEntries.emplace_back(entry.current().data.configSetting());
    }

    ConfigUpgradeSet upgradeSet;
    upgradeSet.updatedEntry = updatedEntries;

    auto invokeRes = getInvokeTx(a1.getPublicKey(), contractCodeLedgerKey,
                                 contractSourceRefLedgerKey, contractID,
                                 upgradeSet, a1.getLastSequenceNumber() + 3);
    txsToSign.emplace_back(invokeRes.first);
    auto const& upgradeSetKey = invokeRes.second;

    for (auto& txEnv : txsToSign)
    {
        txEnv.v1().signatures.emplace_back(SignatureUtils::sign(
            a1.getSecretKey(),
            sha256(xdr::xdr_to_opaque(app->getNetworkID(), ENVELOPE_TYPE_TX,
                                      txEnv.v1().tx))));

        auto const& tx = TransactionFrameBase::makeTransactionFromWire(
            app->getNetworkID(), txEnv);
        LedgerTxn ltx(app->getLedgerTxnRoot());
        TransactionMetaFrame txm(ltx.loadHeader().current().ledgerVersion);
        REQUIRE(tx->checkValid(*app, ltx, 0, 0, 0));
        REQUIRE(tx->apply(*app, ltx, txm));
        ltx.commit();
    }

    auto& commandHandler = app->getCommandHandler();

    std::string command = "mode=set&configupgradesetkey=";
    command += decoder::encode_b64(xdr::xdr_to_opaque(upgradeSetKey));
    command += "&upgradetime=2000-07-21T22:04:00Z";

    std::string ret;
    commandHandler.upgrades(command, ret);
    REQUIRE(ret == "");

    auto checkCost = [&](int64 feeRead) {
        auto costKey = configSettingKey(
            ConfigSettingID::CONFIG_SETTING_CONTRACT_LEDGER_COST_V0);
        LedgerTxn ltx(app->getLedgerTxnRoot());
        auto costEntry = ltx.load(costKey);
        REQUIRE(costEntry.current()
                    .data.configSetting()
                    .contractLedgerCost()
                    .feeRead1KB == feeRead);
    };

    SECTION("success")
    {
        // trigger upgrade
        auto ledgerUpgrade = LedgerUpgrade{LEDGER_UPGRADE_CONFIG};
        ledgerUpgrade.newConfig() = upgradeSetKey;

        auto const& lcl = lm.getLastClosedLedgerHeader();
        auto txSet = TxSetFrame::makeEmpty(lcl);
        auto lastCloseTime = lcl.header.scpValue.closeTime;

        app->getHerder().externalizeValue(
            txSet, lcl.header.ledgerSeq + 1, lastCloseTime,
            {LedgerTestUtils::toUpgradeType(ledgerUpgrade)});

        checkCost(1234);
    }

    auto const& lcl = lm.getLastClosedLedgerHeader();

    // The only readWrite key in the invoke op is the one that writes the
    // ConfigUpgradeSet xdr
    auto proposalKey = invokeRes.first.v1()
                           .tx.ext.sorobanData()
                           .resources.footprint.readWrite.at(0);

    SECTION("entry expired")
    {
        {
            LedgerTxn ltx(app->getLedgerTxnRoot());
            auto ttl = ltx.load(getTTLKey(proposalKey));
            // Expire the entry on the next ledger
            ttl.current().data.ttl().liveUntilLedgerSeq = lcl.header.ledgerSeq;
            ltx.commit();
        }

        // trigger upgrade
        auto ledgerUpgrade = LedgerUpgrade{LEDGER_UPGRADE_CONFIG};
        ledgerUpgrade.newConfig() = upgradeSetKey;

        auto txSet = TxSetFrame::makeEmpty(lcl);
        auto lastCloseTime = lcl.header.scpValue.closeTime;

        app->getHerder().externalizeValue(
            txSet, lcl.header.ledgerSeq + 1, lastCloseTime,
            {LedgerTestUtils::toUpgradeType(ledgerUpgrade)});

        // No upgrade due to expired entry
        checkCost(1000);
    }

    auto updateBytes = [&](SCVal const& bytes) {
        {
            LedgerTxn ltx(app->getLedgerTxnRoot());
            auto entry = ltx.load(proposalKey);
            entry.current().data.contractData().val = bytes;
            ltx.commit();
        }

        // trigger upgrade
        auto ledgerUpgrade = LedgerUpgrade{LEDGER_UPGRADE_CONFIG};
        ledgerUpgrade.newConfig() = upgradeSetKey;

        auto txSet = TxSetFrame::makeEmpty(lcl);
        auto lastCloseTime = lcl.header.scpValue.closeTime;

        app->getHerder().externalizeValue(
            txSet, lcl.header.ledgerSeq + 1, lastCloseTime,
            {LedgerTestUtils::toUpgradeType(ledgerUpgrade)});

        // No upgrade due to tampered entry
        checkCost(1000);
    };

    SECTION("Invalid XDR")
    {
        SCVal b(SCV_BYTES);
        updateBytes(b);
    }

    SECTION("Valid XDR but hash mismatch")
    {
        ConfigSettingEntry costSetting(CONFIG_SETTING_CONTRACT_LEDGER_COST_V0);
        costSetting.contractLedgerCost().feeRead1KB = 1234;

        ConfigUpgradeSet upgradeSet;
        upgradeSet.updatedEntry.emplace_back(costSetting);

        auto upgradeSetBytes(xdr::xdr_to_opaque(upgradeSet));
        SCVal b(SCV_BYTES);
        b.bytes() = upgradeSetBytes;
        updateBytes(b);
    }
}
