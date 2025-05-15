// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "test/test.h"
#include "transactions/TransactionFrameBase.h"
#include "util/Logging.h"
#include "util/ProtocolVersion.h"
#include "util/UnorderedSet.h"
#include "xdr/Stellar-transaction.h"
#include <iterator>
#include <stdexcept>
#include <xdrpp/printer.h>

#include "crypto/Random.h"
#include "crypto/SecretKey.h"
#include "herder/Herder.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTypeUtils.h"
#include "ledger/test/LedgerTestUtils.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/CommandHandler.h"
#include "main/SettingsUpgradeUtils.h"
#include "rust/RustBridge.h"
#include "test/TestAccount.h"
#include "test/TestPrinter.h"
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

namespace
{
void
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
    costEntry.txMaxDiskReadEntries =
        MinimumSorobanNetworkConfig::TX_MAX_READ_LEDGER_ENTRIES;
    costEntry.txMaxDiskReadBytes =
        MinimumSorobanNetworkConfig::TX_MAX_READ_BYTES;

    costEntry.txMaxWriteLedgerEntries =
        MinimumSorobanNetworkConfig::TX_MAX_WRITE_LEDGER_ENTRIES;
    costEntry.txMaxWriteBytes = MinimumSorobanNetworkConfig::TX_MAX_WRITE_BYTES;

    costEntry.ledgerMaxDiskReadEntries = costEntry.txMaxDiskReadEntries;
    costEntry.ledgerMaxDiskReadBytes = costEntry.txMaxDiskReadBytes;
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
    exp.liveSorobanStateSizeWindowSampleSize =
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
} // namespace

TEST_CASE_VERSIONS("Trustline stellar asset contract",
                   "[tx][soroban][invariant][conservationoflumens]")
{
    auto issuerKey = getAccount("issuer");
    Asset idr = makeAsset(issuerKey, "IDR");

    auto cfg = getTestConfig();
    cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS = true;

    // Enable all invariants (including EventsAreConsistentWithEntryDiffs)
    cfg.INVARIANT_CHECKS = {".*"};
    cfg.EMIT_CLASSIC_EVENTS = true;
    cfg.BACKFILL_STELLAR_ASSET_EVENTS = true;

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);

    for_versions_from(20, *app, [&] {
        SorobanTest test(app);
        auto& app = test.getApp();
        auto const minBalance = app.getLedgerManager().getLastMinBalance(2);
        auto& root = test.getRoot();
        auto issuer = root.create(issuerKey, minBalance);
        // Enable clawback
        issuer.setOptions(
            setFlags(AUTH_CLAWBACK_ENABLED_FLAG | AUTH_REVOCABLE_FLAG));

        auto acc = root.create("acc", minBalance);
        auto acc2 = root.create("acc2", minBalance);
        auto sponsor = root.create("sponsor", minBalance * 2);
        root.changeTrust(idr, 100);

        {
            auto tx = transactionFrameFromOps(
                app.getNetworkID(), sponsor,
                {sponsor.op(beginSponsoringFutureReserves(acc)),
                 acc.op(changeTrust(idr, 100)),
                 acc.op(endSponsoringFutureReserves())},
                {acc});
            REQUIRE(isSuccessResult(test.invokeTx(tx)));
        }

        {
            LedgerTxn ltx(app.getLedgerTxnRoot());
            auto tlAsset = assetToTrustLineAsset(idr);
            checkSponsorship(ltx, trustlineKey(acc, tlAsset), 1,
                             &sponsor.getPublicKey());
            checkSponsorship(ltx, acc, 0, nullptr, 1, 2, 0, 1);
            checkSponsorship(ltx, sponsor, 0, nullptr, 0, 2, 1, 0);
        }

        issuer.pay(root, idr, 100);

        auto accAddr = makeAccountAddress(acc.getPublicKey());

        AssetContractTestClient client(test, idr);
        // Transfer and mint to account
        REQUIRE(client.transfer(root, accAddr, 10));
        REQUIRE(client.mint(issuer, accAddr, 10));

        // Now mint by transfering from issuer
        REQUIRE(client.transfer(issuer, accAddr, 10));

        // Now burn by transfering to the issuer and by using burn function
        REQUIRE(client.transfer(acc, makeAccountAddress(issuer.getPublicKey()),
                                10));
        REQUIRE(client.burn(acc, 10));

        // Can't burn an issuers balance because it doesn't exist
        REQUIRE(!client.burn(issuer, 10));

        // Now transfer and mint to contractAddress
        auto contractAddr = makeContractAddress(sha256("contract"));
        REQUIRE(client.transfer(root, contractAddr, 10));
        REQUIRE(client.mint(issuer, contractAddr, 10));

        // Now mint by transfering from issuer
        REQUIRE(client.transfer(issuer, contractAddr, 10));

        // Now clawback
        REQUIRE(client.clawback(issuer, accAddr, 2));
        REQUIRE(client.clawback(issuer, contractAddr, 2));

        // Try to clawback more than available
        REQUIRE(!client.clawback(issuer, accAddr, 100));
        REQUIRE(!client.clawback(issuer, contractAddr, 100));

        // Clear clawback, create new balances, and try to clawback
        issuer.setOptions(clearFlags(AUTH_CLAWBACK_ENABLED_FLAG));

        acc2.changeTrust(idr, 100);

        auto acc2Addr = makeAccountAddress(acc2.getPublicKey());
        auto contract2Addr = makeContractAddress(sha256("contract2"));
        REQUIRE(client.mint(issuer, acc2Addr, 10));
        REQUIRE(client.mint(issuer, contract2Addr, 10));

        // Clawback not allowed because trustline and client balance
        // was created when issuer did not have AUTH_CLAWBACK_ENABLED_FLAG set.
        REQUIRE(!client.clawback(issuer, acc2Addr, 1));
        REQUIRE(!client.clawback(issuer, contract2Addr, 1));

        // Now transfer more than balance
        REQUIRE(!client.transfer(acc, acc2Addr, 10));

        // Now transfer from a contract
        TestContract& transferContract = test.deployWasmContract(
            rust_bridge::get_test_contract_sac_transfer());

        REQUIRE(client.mint(issuer, transferContract.getAddress(), 10));

        auto invocationSpec = client.defaultSpec();

        invocationSpec =
            invocationSpec.setReadOnlyFootprint(client.getContract().getKeys());

        LedgerKey issuerLedgerKey(ACCOUNT);
        issuerLedgerKey.account().accountID = getIssuer(idr);

        LedgerKey fromBalanceKey =
            client.makeBalanceKey(transferContract.getAddress());
        LedgerKey toBalanceKey = client.makeBalanceKey(acc2Addr);

        invocationSpec =
            invocationSpec.extendReadOnlyFootprint({issuerLedgerKey})
                .extendReadWriteFootprint({fromBalanceKey, toBalanceKey});

        auto invocation = transferContract.prepareInvocation(
            "transfer_1",
            {makeAddressSCVal(client.getContract().getAddress()),
             makeAddressSCVal(acc2Addr)},
            invocationSpec);
        REQUIRE(invocation.invoke());

        REQUIRE(client.getBalance(transferContract.getAddress()) == 9);
        REQUIRE(client.getBalance(acc2Addr) == 11);

        // Make sure sponsorship info hasn't changed
        {
            LedgerTxn ltx(app.getLedgerTxnRoot());
            auto tlAsset = assetToTrustLineAsset(idr);
            checkSponsorship(ltx, trustlineKey(acc, tlAsset), 1,
                             &sponsor.getPublicKey());
            checkSponsorship(ltx, acc, 0, nullptr, 1, 2, 0, 1);
            checkSponsorship(ltx, sponsor, 0, nullptr, 0, 2, 1, 0);
        }
    });
}

TEST_CASE("Native stellar asset contract",
          "[tx][soroban][invariant][conservationoflumens]")
{
    auto cfg = getTestConfig();
    cfg.TESTING_SOROBAN_HIGH_LIMIT_OVERRIDE = true;

    SorobanTest test(cfg);
    auto& app = test.getApp();
    auto& root = test.getRoot();

    auto const minBalance = app.getLedgerManager().getLastMinBalance(2);
    auto a2 = root.create("a2", minBalance);

    auto key = SecretKey::pseudoRandomForTesting();
    TestAccount a1(app, key);
    auto tx = transactionFrameFromOps(
        app.getNetworkID(), root,
        {root.op(beginSponsoringFutureReserves(a1)),
         root.op(
             createAccount(a1, app.getLedgerManager().getLastMinBalance(10))),
         a1.op(endSponsoringFutureReserves())},
        {key});
    REQUIRE(isSuccessResult(test.invokeTx(tx)));

    {
        LedgerTxn ltx(app.getLedgerTxnRoot());
        checkSponsorship(ltx, a1.getPublicKey(), 1, &root.getPublicKey(), 0, 2,
                         0, 2);
        checkSponsorship(ltx, root.getPublicKey(), 0, nullptr, 0, 2, 2, 0);
    }

    AssetContractTestClient client(test, txtest::makeNativeAsset());
    // transfer 10 XLM from a1 to contractID
    auto contractAddr = makeContractAddress(sha256("contract"));
    REQUIRE(client.transfer(a1, contractAddr, 10));

    auto a2Addr = makeAccountAddress(a2);
    // Now do an account to account transfer
    REQUIRE(client.transfer(a1, a2Addr, 100));

    // Now try to mint native
    REQUIRE(!client.mint(root, contractAddr, 10));

    // Now transfer more than balance
    REQUIRE(!client.transfer(a1, contractAddr, INT64_MAX));

    // Now test xlm transfer from a contract to another contract and then to an
    // account.
    TestContract& transferContract =
        test.deployWasmContract(rust_bridge::get_test_contract_sac_transfer());

    REQUIRE(client.transfer(a1, transferContract.getAddress(), 10));

    auto invocationSpec = client.defaultSpec();

    invocationSpec =
        invocationSpec.setReadOnlyFootprint(client.getContract().getKeys());

    LedgerKey fromBalanceKey =
        client.makeBalanceKey(transferContract.getAddress());

    // Contract -> Contract
    auto contractToContractSpec = invocationSpec.extendReadWriteFootprint(
        {fromBalanceKey, client.makeBalanceKey(contractAddr)});
    REQUIRE(transferContract
                .prepareInvocation(
                    "transfer_1",
                    {makeAddressSCVal(client.getContract().getAddress()),
                     makeAddressSCVal(contractAddr)},
                    contractToContractSpec)
                .invoke());

    REQUIRE(client.getBalance(transferContract.getAddress()) == 9);
    REQUIRE(client.getBalance(contractAddr) == 11);

    // Contract -> Account
    auto a2BalanceSnapshot = a2.getBalance();
    auto contractToAccountSpec = invocationSpec.extendReadWriteFootprint(
        {fromBalanceKey, client.makeBalanceKey(a2Addr)});
    REQUIRE(transferContract
                .prepareInvocation(
                    "transfer_1",
                    {makeAddressSCVal(client.getContract().getAddress()),
                     makeAddressSCVal(a2Addr)},
                    contractToAccountSpec)
                .invoke());

    REQUIRE(client.getBalance(transferContract.getAddress()) == 8);
    REQUIRE(a2BalanceSnapshot == a2.getBalance() - 1);

    // Make sure sponsorship info hasn't changed
    {
        LedgerTxn ltx(app.getLedgerTxnRoot());
        checkSponsorship(ltx, a1.getPublicKey(), 1, &root.getPublicKey(), 0, 2,
                         0, 2);
        checkSponsorship(ltx, root.getPublicKey(), 0, nullptr, 0, 2, 2, 0);
    }
}

TEST_CASE("Stellar asset contract transfer with CAP-67 address types",
          "[tx][soroban]")
{
    auto cfg = getTestConfig();
    cfg.TESTING_SOROBAN_HIGH_LIMIT_OVERRIDE = true;

    SorobanTest test(cfg);
    auto& root = test.getRoot();

    auto a1 = root.create("a1", 1'000'000'000);
    auto a2 = root.create("a2", 1'000'000'000);
    Asset asset = makeAsset(root.getSecretKey(), "USDC");
    a1.changeTrust(asset, 2'000'000'000);
    a2.changeTrust(asset, 2'000'000'000);
    root.pay(a1.getPublicKey(), asset, 1'000'000'000);
    root.pay(a2.getPublicKey(), asset, 1'000'000'000);

    auto a1Address = makeAccountAddress(a1.getPublicKey());
    auto a2Address = makeAccountAddress(a2.getPublicKey());
    TestContract& transferContract =
        test.deployWasmContract(rust_bridge::get_test_contract_sac_transfer());

    auto runTest = [&](bool useNativeAsset) {
        Asset tokenAsset = useNativeAsset ? txtest::makeNativeAsset() : asset;
        AssetContractTestClient client(test, tokenAsset);
        {
            INFO("transfer to muxed account");
            REQUIRE(client.transfer(
                a1, makeMuxedAccountAddress(a2.getPublicKey(), 123'456'789),
                100'000'000));
            REQUIRE(*client.lastEvent() ==
                    client.makeTransferEvent(a1Address, a2Address, 100'000'000,
                                             123'456'789));
        }
        {
            INFO("transfer from account to contract");
            REQUIRE(client.transfer(a2, transferContract.getAddress(),
                                    200'000'000));
            REQUIRE(*client.lastEvent() ==
                    client.makeTransferEvent(
                        a2Address, transferContract.getAddress(), 200'000'000));
        }
        {
            INFO("transfer from contract not supporting muxed accounts")
            auto contractToAccountSpec =
                client.defaultSpec()
                    .setReadOnlyFootprint(client.getContract().getKeys())
                    .extendReadWriteFootprint(
                        {client.makeBalanceKey(transferContract.getAddress()),
                         client.makeBalanceKey(a2Address)});
            // Muxed destination won't work.
            REQUIRE(
                !transferContract
                     .prepareInvocation(
                         "transfer_1",
                         {makeAddressSCVal(client.getContract().getAddress()),
                          makeAddressSCVal(
                              makeMuxedAccountAddress(a2.getPublicKey(), 111))},
                         contractToAccountSpec)
                     .invoke());
            // Non-muxed destination will work.
            REQUIRE(
                transferContract
                    .prepareInvocation(
                        "transfer_1",
                        {makeAddressSCVal(client.getContract().getAddress()),
                         makeAddressSCVal(a2Address)},
                        contractToAccountSpec)
                    .invoke());
        }
        {
            INFO("transfer from account to muxed account");
            REQUIRE(client.transfer(
                a1,
                makeMuxedAccountAddress(a2.getPublicKey(),
                                        123'456'789'123'456'789ULL),
                300'000'000));
            REQUIRE(*client.lastEvent() ==
                    client.makeTransferEvent(a1Address, a2Address, 300'000'000,
                                             123'456'789'123'456'789ULL));
        }
        {
            INFO("transfer to liquidity pool fails");
            REQUIRE(
                !client.transfer(a1, makeLiquidityPoolAddress(PoolID()), 1));
            REQUIRE(client.lastEvent() == std::nullopt);
        }
        {
            INFO("transfer to claimable balance fails");
            REQUIRE(!client.transfer(
                a1, makeClaimableBalanceAddress(ClaimableBalanceID()), 1));
            REQUIRE(client.lastEvent() == std::nullopt);
        }
    };

    SECTION("native asset")
    {
        runTest(true);
    }
    SECTION("custom asset")
    {
        runTest(false);
    }
}

TEST_CASE_VERSIONS("basic contract invocation", "[tx][soroban]")
{
    auto cfg = getTestConfig();
    if (protocolVersionIsBefore(cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION,
                                SOROBAN_PROTOCOL_VERSION))
    {
        return;
    }

    SorobanTest test(cfg);
    TestContract& addContract =
        test.deployWasmContract(rust_bridge::get_test_wasm_add_i32());
    auto& hostFnExecTimer =
        test.getApp().getMetrics().NewTimer({"soroban", "host-fn-op", "exec"});
    auto& hostFnSuccessMeter = test.getApp().getMetrics().NewMeter(
        {"soroban", "host-fn-op", "success"}, "call");
    auto& hostFnFailureMeter = test.getApp().getMetrics().NewMeter(
        {"soroban", "host-fn-op", "failure"}, "call");

    auto invoke = [&](TestContract& contract, std::string const& functionName,
                      std::vector<SCVal> const& args,
                      SorobanInvocationSpec const& spec,
                      std::optional<uint32_t> expectedRefund = std::nullopt,
                      bool addContractKeys = true) {
        auto rootAccount = test.getRoot();
        // NB: we're using a single root ltx that will collect all the changes
        // in memory in order to test lower-level Soroban logic. Thus the
        // changes will be rolled back after `invoke` has finished.
        LedgerTxn rootLtx(test.getApp().getLedgerTxnRoot());
        auto getRootBalance = [&]() {
            LedgerTxn ltx(rootLtx);
            auto entry = stellar::loadAccount(ltx, rootAccount.getPublicKey());
            return entry.current().data.account().balance;
        };

        int64_t initBalance = getRootBalance();
        auto invocation = contract.prepareInvocation(functionName, args, spec,
                                                     addContractKeys);
        auto tx = invocation.createTx(&rootAccount);
        auto diagnostics = DiagnosticEventManager::createDisabled();
        auto result = tx->checkValid(test.getApp().getAppConnector(), rootLtx,
                                     0, 0, 0, diagnostics);
        REQUIRE(result->isSuccess());

        REQUIRE(tx->getFullFee() ==
                spec.getInclusionFee() + spec.getResourceFee());
        REQUIRE(tx->getInclusionFee() == spec.getInclusionFee());

        // Initially we store in result the charge for resources plus
        // minimum inclusion  fee bid (currently equivalent to the
        // network `baseFee` of 100).
        int64_t baseCharged = tx->declaredSorobanResourceFee() + 100;
        // Imitate surge pricing by charging at a higher rate than
        // base fee.
        uint32_t const surgePricedFee = 300;
        REQUIRE(result->getFeeCharged() == baseCharged);
        {
            LedgerTxn ltx(rootLtx);
            result = tx->processFeeSeqNum(ltx, surgePricedFee);
            ltx.commit();
        }
        // The resource and the base fee are charged, with additional
        // surge pricing fee.
        int64_t balanceAfterFeeCharged = getRootBalance();
        REQUIRE(initBalance - balanceAfterFeeCharged ==
                tx->declaredSorobanResourceFee() + surgePricedFee);

        TransactionMetaBuilder txmBuilder(true, *tx, test.getLedgerVersion(),
                                          test.getApp().getAppConnector());
        auto timerBefore = hostFnExecTimer.count();
        bool success = tx->apply(test.getApp().getAppConnector(), rootLtx,
                                 txmBuilder, *result);
        REQUIRE(hostFnExecTimer.count() - timerBefore > 0);

        {
            LedgerTxn ltx(rootLtx);
            tx->processPostApply(test.getApp().getAppConnector(), ltx,
                                 txmBuilder, *result);
            ltx.commit();
        }

        TransactionMetaFrame txm(txmBuilder.finalize(success));
        auto changesAfter = txm.getChangesAfter();

        // In case of failure we simply refund the whole refundable fee portion.
        if (!expectedRefund)
        {
            REQUIRE(!success);
            // Compute the exact refundable fee (so we don't need spec
            // to have the exact refundable fee set).
            auto nonRefundableFee =
                sorobanResourceFee(test.getApp(), tx->sorobanResources(),
                                   xdr::xdr_size(tx->getEnvelope()), 0);
            expectedRefund = spec.getResourceFee() - nonRefundableFee;
        }

        // Verify refund meta
        REQUIRE(changesAfter.size() == 2);
        REQUIRE(changesAfter[1].updated().data.account().balance -
                    changesAfter[0].state().data.account().balance ==
                *expectedRefund);

        // Make sure account receives expected refund
        REQUIRE(getRootBalance() - balanceAfterFeeCharged == *expectedRefund);

        return std::make_tuple(tx, txm, std::move(result));
    };

    auto failedInvoke =
        [&](TestContract& contract, std::string const& functionName,
            std::vector<SCVal> const& args, SorobanInvocationSpec const& spec,
            bool addContractKeys = true) {
            auto successesBefore = hostFnSuccessMeter.count();
            auto failuresBefore = hostFnFailureMeter.count();
            auto [tx, txm, result] = invoke(contract, functionName, args, spec,
                                            std::nullopt, addContractKeys);
            REQUIRE(hostFnSuccessMeter.count() - successesBefore == 0);
            REQUIRE(hostFnFailureMeter.count() - failuresBefore == 1);
            REQUIRE(result->getResultCode() == txFAILED);
            return result->getXDR()
                .result.results()[0]
                .tr()
                .invokeHostFunctionResult()
                .code();
        };

    auto fnName = "add";
    auto sc7 = makeI32(7);
    auto sc16 = makeI32(16);

    auto invocationSpec = SorobanInvocationSpec()
                              .setInstructions(2'000'000)
                              .setReadBytes(2000)
                              .setInclusionFee(12345);

    uint32_t const expectedResourceFee = 32'702;
    SECTION("correct invocation")
    {
        // NB: We use a hard-coded fees to smoke-test the fee
        // computation logic stability.
        uint32_t const expectedRefund = 100'000;
        auto spec = invocationSpec
                        // Since in tx we don't really distinguish between
                        // refundable and non-refundable fee, set the fee that
                        // we expect to be charged as 'non-refundable'.
                        .setNonRefundableResourceFee(expectedResourceFee)
                        .setRefundableResourceFee(expectedRefund);
        auto successesBefore = hostFnSuccessMeter.count();
        auto failuresBefore = hostFnFailureMeter.count();

        auto [tx, txm, result] =
            invoke(addContract, fnName, {sc7, sc16}, spec, expectedRefund);

        REQUIRE(hostFnSuccessMeter.count() - successesBefore == 1);
        REQUIRE(hostFnFailureMeter.count() - failuresBefore == 0);
        REQUIRE(result->isSuccess());

        auto const& ores = result->getXDR().result.results().at(0);
        REQUIRE(ores.tr().type() == INVOKE_HOST_FUNCTION);
        REQUIRE(ores.tr().invokeHostFunctionResult().code() ==
                INVOKE_HOST_FUNCTION_SUCCESS);

        SCVal resultVal = txm.getReturnValue();
        REQUIRE(resultVal.i32() == 7 + 16);

        InvokeHostFunctionSuccessPreImage successPreImage;
        successPreImage.returnValue = resultVal;
        successPreImage.events = txm.getSorobanContractEvents();

        REQUIRE(ores.tr().invokeHostFunctionResult().success() ==
                xdrSha256(successPreImage));
    }

    auto badAddressTest = [&](SCAddress const& address) {
        auto keys = addContract.getKeys();
        keys[1].contractData().contract = address;
        TestContract badContract(test, address, keys);
        REQUIRE(failedInvoke(badContract, fnName, {sc7, sc16},
                             invocationSpec) == INVOKE_HOST_FUNCTION_TRAPPED);
    };
    SECTION("non-existent contract id")
    {
        auto badContractAddress = addContract.getAddress();
        badContractAddress.contractId()[0] = 0;
        badAddressTest(badContractAddress);
    }
    SECTION("account address")
    {
        auto accountAddress = makeAccountAddress(test.getRoot().getPublicKey());
        badAddressTest(accountAddress);
    }
    SECTION("malformed function names")
    {
        REQUIRE(failedInvoke(addContract, "", {sc7, sc16}, invocationSpec) ==
                INVOKE_HOST_FUNCTION_TRAPPED);
        REQUIRE(failedInvoke(addContract, "add2", {sc7, sc16},
                             invocationSpec) == INVOKE_HOST_FUNCTION_TRAPPED);
        REQUIRE(failedInvoke(addContract, "\0add", {sc7, sc16},
                             invocationSpec) == INVOKE_HOST_FUNCTION_TRAPPED);
        REQUIRE(failedInvoke(addContract, "add$", {sc7, sc16},
                             invocationSpec) == INVOKE_HOST_FUNCTION_TRAPPED);
        REQUIRE(failedInvoke(addContract,
                             "\xFF"
                             "aaaaaa",
                             {sc7, sc16},
                             invocationSpec) == INVOKE_HOST_FUNCTION_TRAPPED);
        REQUIRE(failedInvoke(addContract,
                             "aaaaaa"
                             "\xF4",
                             {sc7, sc16},
                             invocationSpec) == INVOKE_HOST_FUNCTION_TRAPPED);
        REQUIRE(failedInvoke(addContract,
                             "aaaaaaaaaaaaaa"
                             "\xF4",
                             {sc7, sc16},
                             invocationSpec) == INVOKE_HOST_FUNCTION_TRAPPED);
        REQUIRE(failedInvoke(addContract,
                             "aaaaaaa"
                             "\xC5"
                             "aaaaaaa",
                             {sc7, sc16},
                             invocationSpec) == INVOKE_HOST_FUNCTION_TRAPPED);
        REQUIRE(failedInvoke(addContract,
                             "aaa"
                             "\xC5"
                             "a",
                             {sc7, sc16},
                             invocationSpec) == INVOKE_HOST_FUNCTION_TRAPPED);
    }
    SECTION("incorrect invocation parameters")
    {
        SECTION("too few parameters")
        {
            REQUIRE(failedInvoke(addContract, fnName, {sc7}, invocationSpec) ==
                    INVOKE_HOST_FUNCTION_TRAPPED);
        }
        SECTION("too many parameters")
        {
            REQUIRE(failedInvoke(addContract, fnName, {sc7, sc16, makeI32(0)},
                                 invocationSpec) ==
                    INVOKE_HOST_FUNCTION_TRAPPED);
        }
        SECTION("malformed parameters")
        {
            SCVal badSCVal(SCV_LEDGER_KEY_NONCE);
            REQUIRE(failedInvoke(addContract, fnName, {sc7, badSCVal},
                                 invocationSpec) ==
                    INVOKE_HOST_FUNCTION_TRAPPED);
        }
    }

    SECTION("insufficient instructions")
    {
        REQUIRE(failedInvoke(addContract, fnName, {sc7, sc16},
                             invocationSpec.setInstructions(10000)) ==
                INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
    }
    SECTION("insufficient read bytes after p23")
    {
        // Only run this test for protocol version >= 23
        // (AUTO_RESTORE_PROTOCOL_VERSION)
        if (protocolVersionStartsFrom(
                cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION,
                AUTO_RESTORE_PROTOCOL_VERSION))
        {
            // We fail while reading the footprint, before the host
            // function is called. Only classic entries are metered, and
            // they must exist, so use root account
            auto classicEntry = accountKey(test.getRoot().getPublicKey());
            auto readFootprint = addContract.getKeys();
            readFootprint.emplace_back(classicEntry);
            REQUIRE(failedInvoke(
                        addContract, fnName, {sc7, sc16},
                        invocationSpec.setReadBytes(10).setReadOnlyFootprint(
                            readFootprint),
                        /* addContractKeys */ false) ==
                    INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
        }
    }
    SECTION("insufficient read bytes before p23")
    {
        if (protocolVersionIsBefore(cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION,
                                    AUTO_RESTORE_PROTOCOL_VERSION))
        {
            // We fail while reading the footprint, before the host
            // function is called.
            REQUIRE(failedInvoke(addContract, fnName, {sc7, sc16},
                                 invocationSpec.setReadBytes(100)) ==
                    INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
        }
    }
    SECTION("incorrect footprint")
    {
        REQUIRE(
            failedInvoke(
                addContract, fnName, {sc7, sc16},
                invocationSpec.setReadOnlyFootprint({addContract.getKeys()[0]}),
                /* addContractKeys */ false) == INVOKE_HOST_FUNCTION_TRAPPED);
        REQUIRE(
            failedInvoke(
                addContract, fnName, {sc7, sc16},
                invocationSpec.setReadOnlyFootprint({addContract.getKeys()[1]}),
                /* addContractKeys */ false) == INVOKE_HOST_FUNCTION_TRAPPED);
    }
    SECTION("insufficient refundable resource fee")
    {
        REQUIRE(failedInvoke(
                    addContract, fnName, {sc7, sc16},
                    invocationSpec
                        .setNonRefundableResourceFee(expectedResourceFee - 1)
                        .setRefundableResourceFee(0)) ==
                INVOKE_HOST_FUNCTION_INSUFFICIENT_REFUNDABLE_FEE);
    }
}

TEST_CASE("version test", "[tx][soroban]")
{
    // This test is only valid from SOROBAN_PROTOCOL_VERSION + 1, and
    // CURRENT_LEDGER_PROTOCOL_VERSION will never decrease, so an equality check
    // is fine here.
    if (protocolVersionEquals(Config::CURRENT_LEDGER_PROTOCOL_VERSION,
                              SOROBAN_PROTOCOL_VERSION))
    {
        return;
    }
    auto next = Config::CURRENT_LEDGER_PROTOCOL_VERSION;
    auto curr = next - 1;

    auto cfg = getTestConfig(0);
    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION = curr;
    cfg.USE_CONFIG_FOR_GENESIS = false;
    SorobanTest test(cfg, false);

    auto upgrade = LedgerUpgrade{LEDGER_UPGRADE_VERSION};
    upgrade.newLedgerVersion() = curr;

    executeUpgrade(test.getApp(), upgrade);

    overrideSorobanNetworkConfigForTest(test.getApp());

    TestContract& contract =
        test.deployWasmContract(rust_bridge::get_invoke_contract_wasm());

    auto invoke = [&](TestContract& contract, std::string const& functionName,
                      std::vector<SCVal> const& args,
                      SorobanInvocationSpec const& spec) {
        auto invocation =
            contract.prepareInvocation(functionName, args, spec, true);
        auto tx = invocation.createTx();

        REQUIRE(test.isTxValid(tx));

        TransactionMetaFrame txm;
        REQUIRE(isSuccessResult(test.invokeTx(tx, &txm)));
        return txm;
    };

    auto fnName = "get_protocol_version";

    auto invocationSpec = SorobanInvocationSpec()
                              .setInstructions(3'000'000)
                              .setReadBytes(5'000)
                              .setInclusionFee(15'000);

    auto spec = invocationSpec.setNonRefundableResourceFee(50'000)
                    .setRefundableResourceFee(50'000);

    // Check protocol version in curr
    {
        auto txm = invoke(contract, fnName, {}, spec);

        REQUIRE(txm.getReturnValue().u32() == curr);
    }

    // Check protocol version in next
    {
        upgrade.newLedgerVersion() = next;
        executeUpgrade(test.getApp(), upgrade);

        auto txm2 = invoke(contract, fnName, {}, spec);

        REQUIRE(txm2.getReturnValue().u32() == next);
    }
}

TEST_CASE("Soroban footprint validation", "[tx][soroban]")
{
    SorobanTest test;
    auto const& cfg = test.getNetworkCfg();

    auto& addContract =
        test.deployWasmContract(rust_bridge::get_test_wasm_add_i32());

    SorobanResources resources;
    resources.instructions = 2'000'000;
    resources.diskReadBytes = 2000;

    // Tests for each Soroban op
    auto testValidInvoke =
        [&](bool shouldBeValid,
            std::optional<std::vector<uint32_t>> archivedIndexes =
                std::nullopt) {
            SorobanInvocationSpec spec(resources, DEFAULT_TEST_RESOURCE_FEE,
                                       DEFAULT_TEST_RESOURCE_FEE, 100);
            if (archivedIndexes)
            {
                spec = spec.setArchivedIndexes(*archivedIndexes);
            }

            auto tx =
                addContract
                    .prepareInvocation("add", {makeI32(7), makeI32(16)}, spec)
                    .createTx();
            MutableTxResultPtr result;
            {
                auto diagnostics = DiagnosticEventManager::createDisabled();
                LedgerTxn ltx(test.getApp().getLedgerTxnRoot());
                result = tx->checkValid(test.getApp().getAppConnector(), ltx, 0,
                                        0, 0, diagnostics);
            }
            REQUIRE(result->isSuccess() == shouldBeValid);

            if (!shouldBeValid)
            {
                REQUIRE(result->getResultCode() == txSOROBAN_INVALID);
            }
        };

    auto testValidExtendOp = [&](bool shouldBeValid) {
        auto tx = test.createExtendOpTx(resources, 10, 100,
                                        DEFAULT_TEST_RESOURCE_FEE);
        MutableTxResultPtr result;
        {
            auto diagnostics = DiagnosticEventManager::createDisabled();
            LedgerTxn ltx(test.getApp().getLedgerTxnRoot());
            result = tx->checkValid(test.getApp().getAppConnector(), ltx, 0, 0,
                                    0, diagnostics);
        }
        REQUIRE(result->isSuccess() == shouldBeValid);
        if (!shouldBeValid)
        {
            auto txCode = result->getResultCode();
            if (txCode == txFAILED)
            {
                REQUIRE(result->getXDR()
                            .result.results()[0]
                            .tr()
                            .extendFootprintTTLResult()
                            .code() == EXTEND_FOOTPRINT_TTL_MALFORMED);
            }
            else
            {
                REQUIRE(txCode == txSOROBAN_INVALID);
            }
        }
    };

    auto testValidRestoreOp = [&](bool shouldBeValid) {
        auto tx =
            test.createRestoreTx(resources, 100, DEFAULT_TEST_RESOURCE_FEE);

        MutableTxResultPtr result;
        {
            auto diagnostics = DiagnosticEventManager::createDisabled();
            LedgerTxn ltx(test.getApp().getLedgerTxnRoot());
            result = tx->checkValid(test.getApp().getAppConnector(), ltx, 0, 0,
                                    0, diagnostics);
        }
        REQUIRE(result->isSuccess() == shouldBeValid);
        if (!shouldBeValid)
        {
            auto txCode = result->getResultCode();
            if (txCode == txFAILED)
            {
                REQUIRE(result->getXDR()
                            .result.results()[0]
                            .tr()
                            .restoreFootprintResult()
                            .code() == RESTORE_FOOTPRINT_MALFORMED);
            }
            else
            {
                REQUIRE(txCode == txSOROBAN_INVALID);
            }
        }
    };

    // Keys to test
    auto acc = test.getRoot().create(
        "acc", test.getApp().getLedgerManager().getLastMinBalance(1));
    auto persistentKey = addContract.getDataKey(
        makeSymbolSCVal("key1"), ContractDataDurability::PERSISTENT);
    auto tempKey = addContract.getDataKey(makeSymbolSCVal("key1"),
                                          ContractDataDurability::TEMPORARY);
    auto ttlKey = getTTLKey(persistentKey);
    auto persistentKey2 = addContract.getDataKey(
        makeSymbolSCVal("key2"), ContractDataDurability::PERSISTENT);
    auto persistentKey3 = addContract.getDataKey(
        makeSymbolSCVal("key3"), ContractDataDurability::PERSISTENT);

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
        SECTION("contract data key above limit")
        {
            SCVal key;
            key.type(SCV_BYTES);
            key.bytes().resize(cfg.maxContractDataKeySizeBytes());
            footprint.emplace_back(addContract.getDataKey(
                key, ContractDataDurability::PERSISTENT));
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
        SECTION("readOnly footprint not empty")
        {
            resources.footprint.readOnly.emplace_back(persistentKey);
            testValidRestoreOp(false);
        }
        SECTION("temp entries are not allowed")
        {
            resources.footprint.readWrite.emplace_back(tempKey);
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

    SECTION("autorestore footprint")
    {
        SECTION("valid keys")
        {
            resources.footprint.readWrite.emplace_back(persistentKey);
            resources.footprint.readWrite.emplace_back(persistentKey2);
            resources.footprint.readWrite.emplace_back(persistentKey3);
            testValidInvoke(true, std::vector<uint32_t>{0, 2});
        }

        SECTION("entry in readOnly footprint")
        {
            resources.footprint.readOnly.emplace_back(persistentKey);
            testValidInvoke(false, {{0}});
        }

        SECTION("temporary key marked as archived")
        {
            resources.footprint.readWrite.emplace_back(tempKey);
            testValidInvoke(false, {{0}});
        }

        SECTION("classic key marked as archived")
        {
            resources.footprint.readWrite.emplace_back(trustlineKey(
                test.getRoot().getPublicKey(), makeAsset(acc, "USD")));
            testValidInvoke(false, {{0}});
        }

        SECTION("duplicate entries")
        {
            resources.footprint.readWrite.emplace_back(persistentKey);
            resources.footprint.readWrite.emplace_back(persistentKey2);
            resources.footprint.readWrite.emplace_back(persistentKey3);
            testValidInvoke(false, {{0, 1, 0}});
        }

        SECTION("unsorted archived indexes")
        {
            resources.footprint.readWrite.emplace_back(persistentKey);
            resources.footprint.readWrite.emplace_back(persistentKey2);
            resources.footprint.readWrite.emplace_back(persistentKey3);
            testValidInvoke(false, {{0, 2, 1}});
        }

        SECTION("index out of bounds")
        {
            resources.footprint.readWrite.emplace_back(persistentKey);
            resources.footprint.readWrite.emplace_back(persistentKey2);
            resources.footprint.readWrite.emplace_back(persistentKey3);
            testValidInvoke(false, {{0, 2, 3}});
        }
    }
}

TEST_CASE_VERSIONS("Soroban non-refundable resource fees are stable",
                   "[tx][soroban]")
{
    // Historical fee is always paid for 300 byte of transaction result.
    // ceil(6000 * 300 / 1024) == 1758
    int64_t const baseHistoricalFee = 1758;
    // Base valid tx size is 1196 bytes
    // ceil(1196 * 6000 / 1024) + ceil(1196 * 8000 / 1024) == 16'352
    int64_t const baseSizeFee = 16'352;
    int64_t const baseTxFee = baseHistoricalFee + baseSizeFee;
    uint32_t const minInclusionFee = 100;
    uint32_t const writeFee = 5000;

    VirtualClock clock;
    auto cfg = getTestConfig();
    auto app = createTestApplication(clock, cfg);

    for_versions_from(20, *app, [&] {
        auto cfgModifyFn = [&](SorobanNetworkConfig& cfg) {
            cfg.mFeeRatePerInstructionsIncrement = 1000;
            cfg.mfeeDiskReadLedgerEntry = 2000;
            cfg.mFeeWriteLedgerEntry = 3000;
            cfg.mFeeDiskRead1KB = 4000;
            cfg.mFeeHistorical1KB = 6000;
            cfg.mFeeTransactionSize1KB = 8000;

            // In protocol 23 we switched to using the 'flat' write fee. In
            // previous protocol it was dynamic (currently that fee is named
            // 'rent write fee').
            if (protocolVersionStartsFrom(app->getLedgerManager()
                                              .getLastClosedLedgerHeader()
                                              .header.ledgerVersion,
                                          ProtocolVersion::V_23))
            {
                cfg.mFeeFlatRateWrite1KB = writeFee;
            }
        };

        SorobanTest test(app, cfg,
                         /*useTestLimits=*/true, cfgModifyFn);
        auto makeTx = [&test](SorobanResources const& resources,
                              uint32_t inclusionFee, int64_t resourceFee) {
            Operation uploadOp;
            uploadOp.body.type(INVOKE_HOST_FUNCTION);
            auto& uploadHF = uploadOp.body.invokeHostFunctionOp().hostFunction;
            uploadHF.type(HOST_FUNCTION_TYPE_UPLOAD_CONTRACT_WASM);
            uploadHF.wasm().resize(1000);
            return sorobanTransactionFrameFromOps(
                test.getApp().getNetworkID(), test.getRoot(), {uploadOp}, {},
                resources, inclusionFee, resourceFee);
        };

        auto checkFees = [&](SorobanResources const& resources,
                             int64_t expectedNonRefundableFee) {
            auto validTx =
                makeTx(resources, minInclusionFee, expectedNonRefundableFee);
            auto& app = test.getApp();
            // Sanity check the tx fee computation logic.
            auto actualFeePair =
                validTx->getRawTransactionFrame()
                    .computePreApplySorobanResourceFee(
                        app.getLedgerManager()
                            .getLastClosedLedgerHeader()
                            .header.ledgerVersion,
                        app.getLedgerManager()
                            .getLastClosedSorobanNetworkConfig(),
                        app.getConfig());
            REQUIRE(expectedNonRefundableFee ==
                    actualFeePair.non_refundable_fee);

            REQUIRE(test.isTxValid(validTx));

            // Check that just below minimum resource fee fails
            auto notEnoughResourceFeeTx =
                makeTx(resources,
                       // It doesn't matter how high inclusion fee is.
                       1'000'000'000, expectedNonRefundableFee - 1);
            REQUIRE(!test.isTxValid(notEnoughResourceFeeTx));

            // check that just below minimum inclusion fee fails
            auto notEnoughInclusionFeeTx =
                makeTx(resources, minInclusionFee - 1,
                       // It doesn't matter how high the resource fee is.
                       1'000'000'000);
            REQUIRE(!test.isTxValid(notEnoughInclusionFeeTx));
        };

        // In the following tests, we isolate a single fee to test by zeroing
        // out every other resource, as much as is possible
        SECTION("tx size fees")
        {
            SorobanResources resources;
            checkFees(resources, baseTxFee);
        }
        SECTION("compute fee")
        {
            SorobanResources resources;
            resources.instructions = 12'345'678;
            checkFees(resources, 1'234'568 + baseTxFee);
        }

        SECTION("footprint entries")
        {
            // Fee for additonal 6 footprint entries (6 * 36 = 216 bytes)
            // ceil(216 * 6000 / 1024) + ceil(216 * 8000 / 1024) == 2954
            const int64_t additionalTxSizeFee = 2954;
            SECTION("RO only")
            {
                SorobanResources resources;
                LedgerKey lk(LedgerEntryType::CONTRACT_CODE);
                for (uint8_t i = 0; i < 6; ++i)
                {
                    lk.contractCode().hash[0] = i;
                    resources.footprint.readOnly.push_back(lk);
                }

                checkFees(resources,
                          2000 * 6 + baseTxFee + additionalTxSizeFee);
            }
            SECTION("RW only")
            {
                SorobanResources resources;
                LedgerKey lk(LedgerEntryType::CONTRACT_CODE);
                for (uint8_t i = 0; i < 6; ++i)
                {
                    lk.contractCode().hash[0] = i;
                    resources.footprint.readWrite.push_back(lk);
                }

                // RW entries are also counted towards reads.
                checkFees(resources, 2000 * 6 + 3000 * 6 + baseTxFee +
                                         additionalTxSizeFee);
            }
            SECTION("RW and RO")
            {
                SorobanResources resources;
                LedgerKey lk(LedgerEntryType::CONTRACT_CODE);
                for (uint8_t i = 0; i < 3; ++i)
                {
                    lk.contractCode().hash[0] = i;
                    resources.footprint.readOnly.push_back(lk);
                }
                for (uint8_t i = 3; i < 6; ++i)
                {
                    lk.contractCode().hash[0] = i;
                    resources.footprint.readWrite.push_back(lk);
                }
                // RW entries are also counted towards reads.
                checkFees(resources, 2000 * (3 + 3) + 3000 * 3 + baseTxFee +
                                         additionalTxSizeFee);
            }
        }

        // Prior to protocol 23 `mFeeRent1KB` was also used as the regular
        // write fee.
        if (protocolVersionIsBefore(test.getLedgerVersion(),
                                    ProtocolVersion::V_23))
        {
            // Since mFeeWrite1KB is based on the BucketList size sliding
            // window, we
            // must explicitly override the in-memory cached value after
            // initializing the test.
            test.getApp()
                .getLedgerManager()
                .getMutableSorobanNetworkConfigForApply()
                .mFeeRent1KB = 5000;
        }

        SECTION("readBytes fee")
        {
            SorobanResources resources;
            resources.diskReadBytes = 5 * 1024 + 1;
            checkFees(resources, 20'004 + baseTxFee);
        }

        SECTION("writeBytes fee")
        {
            SorobanResources resources;
            resources.writeBytes = 5 * 1024 + 1;
            checkFees(resources, 25'005 + baseTxFee);
        }
    });
}

TEST_CASE_VERSIONS("refund account merged", "[tx][soroban][merge]")
{
    Config cfg = getTestConfig();
    cfg.EMIT_CLASSIC_EVENTS = true;
    cfg.BACKFILL_STELLAR_ASSET_EVENTS = true;

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);

    for_versions_from(20, *app, [&] {
        SorobanTest test(app);
        auto ledgerVersion = getLclProtocolVersion(test.getApp());

        const int64_t startingBalance =
            test.getApp().getLedgerManager().getLastMinBalance(50);

        auto a1 = test.getRoot().create("A", startingBalance);
        auto b1 = test.getRoot().create("B", startingBalance);
        auto c1 = test.getRoot().create("C", startingBalance);
        auto wasm = rust_bridge::get_test_wasm_add_i32();
        auto resources =
            defaultUploadWasmResourcesWithoutFootprint(wasm, ledgerVersion);
        auto tx =
            makeSorobanWasmUploadTx(test.getApp(), a1, wasm, resources, 1000);

        auto mergeOp = accountMerge(b1);
        mergeOp.sourceAccount.activate() = toMuxedAccount(a1);

        auto classicMergeTx = c1.tx({mergeOp});
        classicMergeTx->addSignature(a1.getSecretKey());
        std::vector<TransactionFrameBasePtr> txs = {classicMergeTx, tx};
        auto r = closeLedger(test.getApp(), txs);
        checkTx(0, r, txSUCCESS);

        // The source account of the soroban tx was merged during the classic
        // phase
        checkTx(1, r, txNO_ACCOUNT);

        // The inclusion fee is 1000, but the tx is not surge priced, so remove
        // everything above the min baseFee (100)
        int64_t initialFee = tx->getEnvelope().v1().tx.fee - 900;

        REQUIRE(r.results[1].result.feeCharged == initialFee);

        auto const& txEvents = app->getLedgerManager()
                                   .getLastClosedLedgerTxMeta()[1]
                                   .getTxEvents();

        // The refund event was not emitted because the account was merged.
        REQUIRE(txEvents.size() == 1);
        validateFeeEvent(txEvents[0], a1.getPublicKey(), initialFee,
                         ledgerVersion, false);
    });
}

TEST_CASE_VERSIONS("fee bump refund account merged", "[tx][soroban][merge]")
{
    Config cfg = getTestConfig();
    cfg.EMIT_CLASSIC_EVENTS = true;
    cfg.BACKFILL_STELLAR_ASSET_EVENTS = true;

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);

    for_versions_from(20, *app, [&] {
        SorobanTest test(app);
        auto ledgerVersion = getLclProtocolVersion(test.getApp());

        const int64_t startingBalance =
            test.getApp().getLedgerManager().getLastMinBalance(50);

        auto a1 = test.getRoot().create("A", startingBalance);
        auto b1 = test.getRoot().create("B", startingBalance);
        auto c1 = test.getRoot().create("C", startingBalance);
        auto feeBumper = test.getRoot().create("feeBumper", startingBalance);

        auto wasm = rust_bridge::get_test_wasm_add_i32();
        auto resources =
            defaultUploadWasmResourcesWithoutFootprint(wasm, ledgerVersion);
        auto tx =
            makeSorobanWasmUploadTx(test.getApp(), a1, wasm, resources, 100);

        TransactionEnvelope fb(ENVELOPE_TYPE_TX_FEE_BUMP);
        fb.feeBump().tx.feeSource = toMuxedAccount(feeBumper);
        fb.feeBump().tx.fee = tx->getEnvelope().v1().tx.fee * 5;

        fb.feeBump().tx.innerTx.type(ENVELOPE_TYPE_TX);
        fb.feeBump().tx.innerTx.v1() = tx->getEnvelope().v1();

        fb.feeBump().signatures.emplace_back(SignatureUtils::sign(
            feeBumper, sha256(xdr::xdr_to_opaque(test.getApp().getNetworkID(),
                                                 ENVELOPE_TYPE_TX_FEE_BUMP,
                                                 fb.feeBump().tx))));
        auto feeBumpTxFrame = TransactionFrameBase::makeTransactionFromWire(
            test.getApp().getNetworkID(), fb);

        auto mergeOp = accountMerge(b1);
        mergeOp.sourceAccount.activate() = toMuxedAccount(feeBumper);

        auto classicMergeTx = c1.tx({mergeOp});
        classicMergeTx->addSignature(feeBumper.getSecretKey());
        std::vector<TransactionFrameBasePtr> txs = {classicMergeTx,
                                                    feeBumpTxFrame};
        auto r = closeLedger(test.getApp(), txs);
        checkTx(0, r, txSUCCESS);

        // The fee source account of the soroban tx was merged during the
        // classic phase, but the fee bump will still be applied
        checkTx(1, r, txFEE_BUMP_INNER_SUCCESS);

        REQUIRE(
            r.results[1].result.result.innerResultPair().result.result.code() ==
            txSUCCESS);

        int64_t initialFee = tx->getEnvelope().v1().tx.fee + 100;

        REQUIRE(r.results[1].result.feeCharged == initialFee);

        auto const& txEvents = app->getLedgerManager()
                                   .getLastClosedLedgerTxMeta()[1]
                                   .getTxEvents();

        // The refund event was not emitted because the account was merged.
        REQUIRE(txEvents.size() == 1);
        validateFeeEvent(txEvents[0], feeBumper.getPublicKey(), initialFee,
                         ledgerVersion, false);
    });
}

TEST_CASE_VERSIONS("refund still happens on bad auth", "[tx][soroban]")
{
    Config cfg = getTestConfig();
    cfg.EMIT_CLASSIC_EVENTS = true;
    cfg.BACKFILL_STELLAR_ASSET_EVENTS = true;

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);

    for_versions_from(20, *app, [&] {
        SorobanTest test(app);
        auto ledgerVersion = getLclProtocolVersion(test.getApp());

        const int64_t startingBalance =
            test.getApp().getLedgerManager().getLastMinBalance(50);

        auto a1 = test.getRoot().create("A", startingBalance);
        auto b1 = test.getRoot().create("B", startingBalance);
        auto wasm = rust_bridge::get_test_wasm_add_i32();
        auto resources =
            defaultUploadWasmResourcesWithoutFootprint(wasm, ledgerVersion);
        auto tx =
            makeSorobanWasmUploadTx(test.getApp(), a1, wasm, resources, 100);

        auto a1PreTxBalance = a1.getBalance();
        auto setOptions = txtest::setOptions(setMasterWeight(0));
        setOptions.sourceAccount.activate() = toMuxedAccount(a1);

        auto classicSetOptionsTx = b1.tx({setOptions});
        classicSetOptionsTx->addSignature(a1.getSecretKey());
        std::vector<TransactionFrameBasePtr> txs = {classicSetOptionsTx, tx};
        auto r = closeLedger(test.getApp(), txs);

        checkTx(0, r, txSUCCESS);
        checkTx(1, r, txBAD_AUTH);

        auto a1PostTxBalance = a1.getBalance();

        int64_t expectedRefund = 1'001'583;
        int64_t initialFee = tx->getEnvelope().v1().tx.fee;

        REQUIRE(a1PostTxBalance ==
                a1PreTxBalance - initialFee + expectedRefund);

        auto const& txEvents = app->getLedgerManager()
                                   .getLastClosedLedgerTxMeta()[1]
                                   .getTxEvents();

        REQUIRE(txEvents.size() == 2);
        validateFeeEvent(txEvents[0], a1.getPublicKey(), initialFee,
                         ledgerVersion, false);
        validateFeeEvent(txEvents[1], a1.getPublicKey(), -expectedRefund,
                         ledgerVersion, true);
    });
}

TEST_CASE_VERSIONS("refund test with closeLedger", "[tx][soroban][feebump]")
{
    Config cfg = getTestConfig();
    cfg.EMIT_CLASSIC_EVENTS = true;
    cfg.BACKFILL_STELLAR_ASSET_EVENTS = true;

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);

    for_versions_from(20, *app, [&] {
        SorobanTest test(app);
        auto ledgerVersion = getLclProtocolVersion(test.getApp());

        const int64_t startingBalance =
            test.getApp().getLedgerManager().getLastMinBalance(50);

        auto a1 = test.getRoot().create("A", startingBalance);

        auto a1StartingBalance = a1.getBalance();

        auto wasm = rust_bridge::get_test_wasm_add_i32();
        auto resources =
            defaultUploadWasmResourcesWithoutFootprint(wasm, ledgerVersion);
        auto tx =
            makeSorobanWasmUploadTx(test.getApp(), a1, wasm, resources, 100);

        auto r = closeLedger(test.getApp(), {tx});
        checkTx(0, r, txSUCCESS);

        int64_t expectedRefund =
            protocolVersionStartsFrom(test.getLedgerVersion(),
                                      ProtocolVersion::V_23)
                ? 981'407
                : 981'527;
        int64_t initialFee = tx->getEnvelope().v1().tx.fee;
        REQUIRE(a1.getBalance() ==
                a1StartingBalance - initialFee + expectedRefund);

        // DEFAULT_TEST_RESOURCE_FEE is added onto the calculated soroban
        // resource fee, so the total cost would be greater than
        // DEFAULT_TEST_RESOURCE_FEE without the refund.
        REQUIRE(initialFee - expectedRefund < DEFAULT_TEST_RESOURCE_FEE);

        auto const& txEvents = app->getLedgerManager()
                                   .getLastClosedLedgerTxMeta()[0]
                                   .getTxEvents();

        REQUIRE(txEvents.size() == 2);
        validateFeeEvent(txEvents[0], a1.getPublicKey(), initialFee,
                         ledgerVersion, false);
        validateFeeEvent(txEvents[1], a1.getPublicKey(), -expectedRefund,
                         ledgerVersion, true);
    });
}

TEST_CASE_VERSIONS("refund is sent to fee-bump source",
                   "[tx][soroban][feebump]")
{
    Config cfg = getTestConfig();
    cfg.EMIT_CLASSIC_EVENTS = true;
    cfg.BACKFILL_STELLAR_ASSET_EVENTS = true;

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);

    for_versions_from(20, *app, [&] {
        SorobanTest test(app);
        auto ledgerVersion = getLclProtocolVersion(test.getApp());

        const int64_t startingBalance =
            test.getApp().getLedgerManager().getLastMinBalance(50);

        auto a1 = test.getRoot().create("A", startingBalance);
        auto feeBumper = test.getRoot().create("B", startingBalance);

        auto a1StartingBalance = a1.getBalance();
        auto feeBumperStartingBalance = feeBumper.getBalance();

        auto wasm = rust_bridge::get_test_wasm_add_i32();
        auto resources =
            defaultUploadWasmResourcesWithoutFootprint(wasm, ledgerVersion);
        auto tx =
            makeSorobanWasmUploadTx(test.getApp(), a1, wasm, resources, 100);

        TransactionEnvelope fb(ENVELOPE_TYPE_TX_FEE_BUMP);
        fb.feeBump().tx.feeSource = toMuxedAccount(feeBumper);
        fb.feeBump().tx.fee = tx->getEnvelope().v1().tx.fee * 5;

        fb.feeBump().tx.innerTx.type(ENVELOPE_TYPE_TX);
        fb.feeBump().tx.innerTx.v1() = tx->getEnvelope().v1();

        fb.feeBump().signatures.emplace_back(SignatureUtils::sign(
            feeBumper, sha256(xdr::xdr_to_opaque(test.getApp().getNetworkID(),
                                                 ENVELOPE_TYPE_TX_FEE_BUMP,
                                                 fb.feeBump().tx))));
        auto feeBumpTxFrame = TransactionFrameBase::makeTransactionFromWire(
            test.getApp().getNetworkID(), fb);

        auto r = closeLedger(test.getApp(), {feeBumpTxFrame});
        checkTx(0, r, txFEE_BUMP_INNER_SUCCESS);

        bool afterV20 =
            protocolVersionStartsFrom(ledgerVersion, ProtocolVersion::V_21);

        int64_t expectedRefund =
            protocolVersionStartsFrom(test.getLedgerVersion(),
                                      ProtocolVersion::V_23)
                ? 981'407
                : 981'527;

        // Use the inner transactions fee, which already includes the minimum
        // inclusion fee of 100 stroops, and add the additional 100 because of
        // the fee-bump. This is what the feeBump will be charged.
        int64_t initialFee = tx->getEnvelope().v1().tx.fee + 100;

        // feeCharged did not account for the refund in V20
        auto const feeCharged =
            afterV20 ? initialFee - expectedRefund : initialFee;

        REQUIRE(
            r.results.at(0).result.result.innerResultPair().result.feeCharged ==
            feeCharged - 100);
        REQUIRE(r.results.at(0).result.feeCharged == feeCharged);

        REQUIRE(feeBumper.getBalance() ==
                feeBumperStartingBalance - initialFee + expectedRefund);

        // DEFAULT_TEST_RESOURCE_FEE is added onto the calculated soroban
        // resource fee, so the total cost would be greater than
        // DEFAULT_TEST_RESOURCE_FEE without the refund.
        REQUIRE(initialFee - expectedRefund < DEFAULT_TEST_RESOURCE_FEE);

        // There should be no change to a1's balance
        REQUIRE(a1.getBalance() == a1StartingBalance);

        auto const& txEvents = app->getLedgerManager()
                                   .getLastClosedLedgerTxMeta()[0]
                                   .getTxEvents();

        REQUIRE(txEvents.size() == 2);
        validateFeeEvent(txEvents[0], feeBumper.getPublicKey(), initialFee,
                         ledgerVersion, false);
        validateFeeEvent(txEvents[1], feeBumper.getPublicKey(), -expectedRefund,
                         ledgerVersion, true);
    });
}

TEST_CASE("buying liabilities plus refund is greater than INT64_MAX",
          "[tx][soroban][offer]")
{
    SorobanTest test;

    const int64_t startingBalance =
        test.getApp().getLedgerManager().getLastMinBalance(50);

    auto a1 = test.getRoot().create("A", startingBalance);
    auto b1 = test.getRoot().create("B", startingBalance);

    auto native = txtest::makeNativeAsset();
    auto cur1 = txtest::makeAsset(test.getRoot(), "CUR1");
    a1.changeTrust(cur1, INT64_MAX);
    test.getRoot().pay(a1, cur1, INT64_MAX);

    auto a1PreBalance = a1.getBalance();
    auto wasm = rust_bridge::get_test_wasm_add_i32();
    auto resources = defaultUploadWasmResourcesWithoutFootprint(
        wasm, getLclProtocolVersion(test.getApp()));
    auto tx =
        makeSorobanWasmUploadTx(test.getApp(), a1, wasm, resources, 300'000);

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
    // the offer fee doesn't need to be accounted for in fee
    // computations.
    auto offerTx = b1.tx({offer});
    offerTx->addSignature(a1.getSecretKey());
    std::vector<TransactionFrameBasePtr> txs = {offerTx, tx};
    auto r = closeLedger(test.getApp(), txs);
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
    SorobanTest test(cfg);
    auto& addContract =
        test.deployWasmContract(rust_bridge::get_test_wasm_add_i32());

    auto fnName = "add";
    auto sc7 = makeI32(7);
    auto scMax = makeI32(INT32_MAX);
    auto invocationSpec =
        SorobanInvocationSpec().setInstructions(2'000'000).setReadBytes(2000);

    // This test calls the add_i32 client with two numbers that cause an
    // overflow. Because we have diagnostics on, we will see two events - The
    // diagnostic "fn_call" event, and the event that the add_i32 client
    // emits.

    auto invocation =
        addContract.prepareInvocation(fnName, {sc7, scMax}, invocationSpec);
    REQUIRE(!invocation.invoke());

    auto const& opEvents = invocation.getTxMeta().getDiagnosticEvents();
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

TEST_CASE("transaction validation diagnostics", "[tx][soroban]")
{
    auto cfg = getTestConfig();
    cfg.ENABLE_DIAGNOSTICS_FOR_TX_SUBMISSION = true;
    SorobanTest test(cfg);
    auto& addContract =
        test.deployWasmContract(rust_bridge::get_test_wasm_add_i32());
    auto fnName = "add";
    auto sc7 = makeI32(7);
    auto scMax = makeI32(INT32_MAX);
    auto invocationSpec =
        SorobanInvocationSpec().setInstructions(2'000'000).setReadBytes(2000);
    auto tx =
        addContract
            .prepareInvocation(fnName, {sc7, scMax},
                               invocationSpec.setInstructions(2'000'000'000))
            .createTx();

    auto diagnosticEvents = DiagnosticEventManager::createForValidation(cfg);
    {
        LedgerTxn ltx(test.getApp().getLedgerTxnRoot());
        auto result = tx->checkValid(test.getApp().getAppConnector(), ltx, 0, 0,
                                     0, diagnosticEvents);
    }
    REQUIRE(!test.isTxValid(tx));

    auto const diagEvents = diagnosticEvents.finalize();
    REQUIRE(diagEvents.size() == 1);

    DiagnosticEvent const& diag_ev = diagEvents.at(0);
    LOG_INFO(DEFAULT_LOG, "event 0: {}", xdr::xdr_to_string(diag_ev));
    REQUIRE(!diag_ev.inSuccessfulContractCall);
    REQUIRE(diag_ev.event.type == ContractEventType::DIAGNOSTIC);
    REQUIRE(diag_ev.event.body.v0().topics.at(0).sym() == "error");
    REQUIRE(diag_ev.event.body.v0().data.vec()->at(0).str().find(
                "instructions") != std::string::npos);
}

TEST_CASE("contract errors cause transaction to fail", "[tx][soroban]")
{
    SorobanTest test;
    auto errContract =
        test.deployWasmContract(rust_bridge::get_test_wasm_err());
    auto runTest = [&](std::string const& name) {
        auto invocation = errContract.prepareInvocation(
            name, {},
            SorobanInvocationSpec().setInstructions(3'000'000).setReadBytes(
                3000));
        REQUIRE(!invocation.invoke());
        REQUIRE(invocation.getResultCode() == INVOKE_HOST_FUNCTION_TRAPPED);
    };

    for (auto const& name :
         {"err_eek", "err_err", "ok_err", "ok_val_err", "err", "val"})
    {
        SECTION(name)
        {
            runTest(name);
        }
    }
}

TEST_CASE("settings upgrade", "[tx][soroban][upgrades]")
{
    auto cfg = getTestConfig();
    cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS = true;
    SorobanTest test(cfg, /* useTestLimits*/ false);
    auto runTest = [&]() {
        {
            // make sure LedgerManager picked up cached values by looking at
            // a couple settings
            auto const& networkConfig = test.getNetworkCfg();
            REQUIRE(networkConfig.txMaxDiskReadBytes() ==
                    MinimumSorobanNetworkConfig::TX_MAX_READ_BYTES);
            REQUIRE(networkConfig.ledgerMaxDiskReadBytes() ==
                    networkConfig.txMaxDiskReadBytes());
        }

        SorobanResources maxResources;
        maxResources.instructions =
            MinimumSorobanNetworkConfig::TX_MAX_INSTRUCTIONS;
        maxResources.diskReadBytes =
            MinimumSorobanNetworkConfig::TX_MAX_READ_BYTES;
        maxResources.writeBytes =
            MinimumSorobanNetworkConfig::TX_MAX_WRITE_BYTES;

        // build upgrade

        // This test assumes that all settings including and after
        // CONFIG_SETTING_LIVE_SOROBAN_STATE_SIZE_WINDOW are not upgradeable, so
        // they won't be included in the upgrade.
        xdr::xvector<ConfigSettingEntry> updatedEntries;
        for (auto t : xdr::xdr_traits<ConfigSettingID>::enum_values())
        {
            auto type = static_cast<ConfigSettingID>(t);
            if (SorobanNetworkConfig::isNonUpgradeableConfigSettingEntry(type))
            {
                continue;
            }

            // Because we added more cost types in v21, the initial
            // contractDataEntrySizeBytes setting of 2000 is too low to write
            // all settings at once. This isn't an issue in practice because 1.
            // the setting on pubnet and testnet is much higher, and two, we
            // don't need to upgrade every setting at once. To get around this
            // in the test, we will remove the memory bytes cost types from the
            // upgrade.
            if (type == CONFIG_SETTING_CONTRACT_COST_PARAMS_MEMORY_BYTES)
            {
                continue;
            }

            LedgerTxn ltx(test.getApp().getLedgerTxnRoot());
            auto costEntry = ltx.load(configSettingKey(type));
            updatedEntries.emplace_back(
                costEntry.current().data.configSetting());
        }

        // Update one of the settings. The rest will be the same so will not get
        // upgraded, but this will still test that the limits work when writing
        // all settings to the client.
        auto& cost = updatedEntries.at(static_cast<uint32_t>(
            ConfigSettingID::CONFIG_SETTING_CONTRACT_LEDGER_COST_V0));
        // check a couple settings to make sure they're at the minimum
        REQUIRE(cost.contractLedgerCost().txMaxDiskReadEntries ==
                MinimumSorobanNetworkConfig::TX_MAX_READ_LEDGER_ENTRIES);
        REQUIRE(cost.contractLedgerCost().txMaxDiskReadBytes ==
                MinimumSorobanNetworkConfig::TX_MAX_READ_BYTES);
        REQUIRE(cost.contractLedgerCost().txMaxWriteBytes ==
                MinimumSorobanNetworkConfig::TX_MAX_WRITE_BYTES);
        REQUIRE(cost.contractLedgerCost().ledgerMaxDiskReadEntries ==
                cost.contractLedgerCost().txMaxDiskReadEntries);
        REQUIRE(cost.contractLedgerCost().ledgerMaxDiskReadBytes ==
                cost.contractLedgerCost().txMaxDiskReadBytes);
        REQUIRE(cost.contractLedgerCost().ledgerMaxWriteBytes ==
                cost.contractLedgerCost().txMaxWriteBytes);
        cost.contractLedgerCost().feeDiskRead1KB = 1000;

        // additional testing to make sure a negative value is accepted for the
        // low value.
        cost.contractLedgerCost().rentFee1KBSorobanStateSizeLow = -100;

        ConfigUpgradeSet upgradeSet;
        upgradeSet.updatedEntry = updatedEntries;

        auto xdr = xdr::xdr_to_opaque(upgradeSet);
        auto upgrade_hash = sha256(xdr);

        auto& writeContract = test.deployWasmContract(
            rust_bridge::get_write_bytes(), maxResources, maxResources);

        LedgerKey upgrade(CONTRACT_DATA);
        upgrade.contractData().durability = TEMPORARY;
        upgrade.contractData().contract = writeContract.getAddress();
        upgrade.contractData().key =
            makeBytesSCVal(xdr::xdr_to_opaque(upgrade_hash));

        REQUIRE(writeContract
                    .prepareInvocation("write", {makeBytesSCVal(xdr)},
                                       SorobanInvocationSpec(maxResources,
                                                             20'000'000,
                                                             20'000'000, 1000)
                                           .extendReadWriteFootprint({upgrade}))
                    .invoke());

        {
            // verify that the client code, client instance, and upgrade
            // entry were all extended by
            // 1036800 ledgers (60 days) -
            // https://github.com/stellar/rs-soroban-env/blob/main/soroban-test-wasms/wasm-workspace/write_upgrade_bytes/src/lib.rs#L3-L5
            auto ledgerSeq = test.getLCLSeq();
            auto extendedKeys = writeContract.getKeys();
            extendedKeys.emplace_back(upgrade);

            REQUIRE(extendedKeys.size() == 3);
            for (auto const& key : extendedKeys)
            {
                REQUIRE(test.getTTL(key) == ledgerSeq + 1036800);
            }
        }

        // arm the upgrade through commandHandler. This isn't required
        // because we'll trigger the upgrade through externalizeValue, but
        // this will test the submission and deserialization code.
        ConfigUpgradeSetKey key;
        key.contentHash = upgrade_hash;
        key.contractID = writeContract.getAddress().contractId();

        auto& commandHandler = test.getApp().getCommandHandler();

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
            test.getApp().getLedgerManager().getLastClosedLedgerHeader();
        auto txSet = TxSetXDRFrame::makeEmpty(lcl);
        auto lastCloseTime = lcl.header.scpValue.closeTime;

        test.getApp().getHerder().externalizeValue(
            txSet, lcl.header.ledgerSeq + 1, lastCloseTime,
            {LedgerTestUtils::toUpgradeType(ledgerUpgrade)});

        // validate upgrade succeeded
        {
            auto costKey = configSettingKey(
                ConfigSettingID::CONFIG_SETTING_CONTRACT_LEDGER_COST_V0);
            LedgerTxn ltx(test.getApp().getLedgerTxnRoot());
            auto costEntry = ltx.load(costKey);
            REQUIRE(costEntry.current()
                        .data.configSetting()
                        .contractLedgerCost()
                        .feeDiskRead1KB == 1000);
        }
    };
    SECTION("from init settings")
    {
        runTest();
    }
    SECTION("from min settings")
    {
        overrideNetworkSettingsToMin(test.getApp());
        runTest();
    }
}

TEST_CASE("loadgen Wasm executes properly", "[tx][soroban][loadgen]")
{
    SorobanTest test;
    auto& loadgenContract =
        test.deployWasmContract(rust_bridge::get_test_wasm_loadgen());
    std::string fnName = "do_work";
    uint64_t guestCycles = 10;
    uint64_t hostCycles = 5;
    uint32_t numEntries = 2;
    uint32_t sizeKiloBytes = 3;

    // This function should write numEntries keys, where each key ID is
    // a U32 that starts at startingIndex and is incremented for each key
    xdr::xvector<LedgerKey> keys;
    for (uint32_t i = 0; i < numEntries; ++i)
    {
        auto lk = loadgenContract.getDataKey(
            makeU32(i), ContractDataDurability::PERSISTENT);
        keys.emplace_back(lk);
    }

    auto invocationSpec = SorobanInvocationSpec()
                              .setInstructions(10'000'000)
                              .setReadBytes(20'000)
                              .setWriteBytes(20'000)
                              .extendReadWriteFootprint(keys);

    auto invocation = loadgenContract.prepareInvocation(
        fnName,
        {makeU64(guestCycles), makeU64(hostCycles), makeU32(numEntries),
         makeU32(sizeKiloBytes)},
        invocationSpec);
    REQUIRE(invocation.invoke());

    // Return value is total number of cycles completed
    REQUIRE(invocation.getReturnValue().u256().lo_lo ==
            guestCycles + hostCycles);

    for (auto const& key : keys)
    {
        LedgerTxn ltx(test.getApp().getLedgerTxnRoot());
        auto ltxe = ltx.load(key);
        REQUIRE(ltxe);
        auto size = xdr::xdr_size(ltxe.current());

        // Check that size is correct, plus some overhead
        REQUIRE(
            (size > sizeKiloBytes * 1024 && size < sizeKiloBytes * 1024 + 100));
    }
}

TEST_CASE("complex contract", "[tx][soroban]")
{
    auto complexTest = [&](bool enableDiagnostics) {
        auto cfg = getTestConfig();
        cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS = enableDiagnostics;
        SorobanTest test(cfg);
        auto& contract =
            test.deployWasmContract(rust_bridge::get_test_wasm_complex());

        // Contract writes a single `data` CONTRACT_DATA entry.
        LedgerKey dataKey = contract.getDataKey(
            makeSymbolSCVal("data"), ContractDataDurability::TEMPORARY);

        auto invocationSpec = SorobanInvocationSpec()
                                  .setReadWriteFootprint({dataKey})
                                  .setInstructions(4'000'000)
                                  .setReadBytes(3000)
                                  .setWriteBytes(1000);

        auto invocation = contract.prepareInvocation("go", {}, invocationSpec);
        REQUIRE(invocation.invoke());
        auto const& contractEvents =
            invocation.getTxMeta().getSorobanContractEvents();
        // Contract should have emitted a single event carrying a `Bytes`
        // value.
        REQUIRE(contractEvents.size() == 1);
        REQUIRE(contractEvents.at(0).type == ContractEventType::CONTRACT);
        REQUIRE(contractEvents.at(0).body.v0().data.type() == SCV_BYTES);

        if (enableDiagnostics)
        {
            auto const& diagnosticEvents =
                invocation.getTxMeta().getDiagnosticEvents();
            REQUIRE(diagnosticEvents.size() == 22);

            auto call_ev = diagnosticEvents.at(0);
            REQUIRE(call_ev.event.type == ContractEventType::DIAGNOSTIC);
            REQUIRE(call_ev.event.body.v0().data.type() == SCV_VOID);

            auto contract_ev = diagnosticEvents.at(1);
            REQUIRE(contract_ev.event.type == ContractEventType::CONTRACT);
            REQUIRE(contract_ev.event.body.v0().data.type() == SCV_BYTES);

            auto return_ev = diagnosticEvents.at(2);
            REQUIRE(return_ev.event.type == ContractEventType::DIAGNOSTIC);
            REQUIRE(return_ev.event.body.v0().data.type() == SCV_VOID);

            auto const& metrics_ev = diagnosticEvents.back();
            REQUIRE(metrics_ev.event.type == ContractEventType::DIAGNOSTIC);
            auto const& v0 = metrics_ev.event.body.v0();
            REQUIRE(v0.topics.size() == 2);
            REQUIRE(v0.topics.at(0).sym() == "core_metrics");
            REQUIRE(v0.topics.at(1).sym() == "max_emit_event_byte");
            REQUIRE(v0.data.type() == SCV_U64);
        }
        else
        {
            REQUIRE(invocation.getTxMeta().getDiagnosticEvents().size() == 0);
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

TEST_CASE("ledger entry size limit enforced", "[tx][soroban]")
{
    SorobanTest test;
    auto const& cfg = test.getNetworkCfg();
    ContractStorageTestClient client(test);

    auto failedRestoreOp = [&](LedgerKey const& lk) {
        SorobanResources resources;
        resources.footprint.readWrite = {lk};
        resources.instructions = 0;
        resources.diskReadBytes = cfg.txMaxDiskReadBytes();
        resources.writeBytes = cfg.txMaxWriteBytes();
        auto resourceFee = 1'000'000;

        auto restoreTX = test.createRestoreTx(resources, 1'000, resourceFee);
        auto result = test.invokeTx(restoreTX);
        REQUIRE(!isSuccessResult(result));
        REQUIRE(
            result.result.results()[0].tr().restoreFootprintResult().code() ==
            RESTORE_FOOTPRINT_RESOURCE_LIMIT_EXCEEDED);
    };

    auto failedExtendOp = [&](LedgerKey const& lk) {
        SorobanResources resources;
        resources.footprint.readOnly = {lk};
        resources.instructions = 0;
        resources.diskReadBytes = cfg.txMaxDiskReadBytes();
        resources.writeBytes = cfg.txMaxWriteBytes();
        auto resourceFee = 1'000'000;

        auto extendTx =
            test.createExtendOpTx(resources, 100, 1'000, resourceFee);
        auto result = test.invokeTx(extendTx);
        REQUIRE(!isSuccessResult(result));
        REQUIRE(
            result.result.results()[0].tr().extendFootprintTTLResult().code() ==
            EXTEND_FOOTPRINT_TTL_RESOURCE_LIMIT_EXCEEDED);
    };

    SECTION("contract data limits")
    {
        // Approximately 100 bytes of overhead for the client data entry
        auto maxWriteSizeKiloBytes =
            (cfg.maxContractDataEntrySizeBytes() - 100) / 1024;
        auto maxWriteSpec =
            client.writeKeySpec("key", ContractDataDurability::PERSISTENT)
                .setWriteBytes(cfg.txMaxWriteBytes());
        // Maximum valid write should succeed
        REQUIRE(client.resizeStorageAndExtend("key", maxWriteSizeKiloBytes, 0,
                                              0, maxWriteSpec) ==
                INVOKE_HOST_FUNCTION_SUCCESS);
        uint32_t originalExpectedLiveUntilLedger =
            test.getLCLSeq() + cfg.stateArchivalSettings().minPersistentTTL - 1;
        auto lk = client.getContract().getDataKey(
            makeSymbolSCVal("key"), ContractDataDurability::PERSISTENT);
        REQUIRE(test.isEntryLive({lk}, test.getLCLSeq()));

        // 1KB above max should fail
        REQUIRE(client.resizeStorageAndExtend("key", maxWriteSizeKiloBytes + 1,
                                              0, 0, maxWriteSpec) ==
                INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);

        REQUIRE(client.has("key", ContractDataDurability::PERSISTENT, true) ==
                INVOKE_HOST_FUNCTION_SUCCESS);

        // Reduce max ledger entry size
        modifySorobanNetworkConfig(
            test.getApp(), [](SorobanNetworkConfig& cfg) {
                cfg.mMaxContractDataEntrySizeBytes -= 1024;
            });

        // Check that max write now fails
        REQUIRE(client.resizeStorageAndExtend("key", maxWriteSizeKiloBytes, 0,
                                              0, maxWriteSpec) ==
                INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);

        // Extend should fail
        failedExtendOp(lk);

        // Reads should fail
        REQUIRE(client.has("key", ContractDataDurability::PERSISTENT,
                           std::nullopt) ==
                INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);

        // Archive entry
        for (uint32_t i =
                 test.getApp().getLedgerManager().getLastClosedLedgerNum();
             i <= originalExpectedLiveUntilLedger + 1; ++i)
        {
            closeLedgerOn(test.getApp(), i, 2, 1, 2016);
        }
        REQUIRE(!test.isEntryLive({lk}, test.getLCLSeq()));

        // Restore should fail
        failedRestoreOp(lk);
    }

    SECTION("contract code limits")
    {
        uint32_t originalExpectedLiveUntilLedger =
            test.getLCLSeq() + cfg.stateArchivalSettings().minPersistentTTL - 1;
        // Reduce max ledger entry size
        modifySorobanNetworkConfig(test.getApp(),
                                   [](SorobanNetworkConfig& cfg) {
                                       cfg.mMaxContractSizeBytes = 2000;
                                   });

        // Check that client invocation now fails
        REQUIRE(client.has("key", ContractDataDurability::PERSISTENT,
                           std::nullopt) ==
                INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);

        auto lk = client.getContract().getKeys()[0];
        // Extend should fail on Wasm key.
        failedExtendOp(lk);

        // Archive entry
        for (uint32_t i =
                 test.getApp().getLedgerManager().getLastClosedLedgerNum();
             i <= originalExpectedLiveUntilLedger + 1; ++i)
        {
            closeLedgerOn(test.getApp(), i, 2, 1, 2016);
        }
        REQUIRE(!test.isEntryLive({lk}, test.getLCLSeq()));

        // Restore should fail
        failedRestoreOp(lk);
    }
}

TEST_CASE_VERSIONS("contract storage", "[tx][soroban][archival]")
{
    auto appCfg = getTestConfig();
    if (protocolVersionIsBefore(appCfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION,
                                AUTO_RESTORE_PROTOCOL_VERSION))
    {
        return;
    }

    auto modifyCfg = [](SorobanNetworkConfig& cfg) {
        // Increase write fee so the fee will be greater than 1
        cfg.mRentFee1KBSorobanStateSizeLow = 20'000;
        cfg.mRentFee1KBSorobanStateSizeHigh = 1'000'000;
    };
    appCfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS = true;
    SorobanTest test(appCfg, true, modifyCfg);

    auto isSuccess = [](auto resultCode) {
        return resultCode == INVOKE_HOST_FUNCTION_SUCCESS;
    };
    ContractStorageTestClient client(test);

    SECTION("successful storage operations")
    {
        // Test writing an entry
        REQUIRE(isSuccess(
            client.put("key1", ContractDataDurability::PERSISTENT, 0)));
        REQUIRE(isSuccess(
            client.get("key1", ContractDataDurability::PERSISTENT, 0)));
        REQUIRE(isSuccess(
            client.put("key2", ContractDataDurability::PERSISTENT, 21)));
        REQUIRE(isSuccess(
            client.get("key2", ContractDataDurability::PERSISTENT, 21)));

        // Test overwriting an entry
        REQUIRE(isSuccess(
            client.put("key1", ContractDataDurability::PERSISTENT, 9)));
        REQUIRE(isSuccess(
            client.get("key1", ContractDataDurability::PERSISTENT, 9)));
        REQUIRE(isSuccess(client.put("key2", ContractDataDurability::PERSISTENT,
                                     UINT64_MAX)));
        REQUIRE(isSuccess(client.get("key2", ContractDataDurability::PERSISTENT,
                                     UINT64_MAX)));

        // Test deleting an entry
        REQUIRE(
            isSuccess(client.del("key1", ContractDataDurability::PERSISTENT)));
        REQUIRE(isSuccess(
            client.has("key1", ContractDataDurability::PERSISTENT, false)));
        REQUIRE(
            isSuccess(client.del("key2", ContractDataDurability::PERSISTENT)));
        REQUIRE(isSuccess(
            client.has("key2", ContractDataDurability::PERSISTENT, false)));
    }

    SECTION("read bytes limit enforced")
    {
        // Write 5 KB entry.
        REQUIRE(isSuccess(client.resizeStorageAndExtend("key", 5, 1, 1)));

        if (protocolVersionStartsFrom(
                appCfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION,
                AUTO_RESTORE_PROTOCOL_VERSION))
        {
            auto readSpec =
                client.readKeySpec("key", ContractDataDurability::PERSISTENT)
                    .setReadBytes(0);

            // Entry is above 5 KB, but this should succeed because it is
            // live and in protocol version 23+ live Soroban entries are not
            // metered
            REQUIRE(
                isSuccess(client.has("key", ContractDataDurability::PERSISTENT,
                                     std::nullopt, readSpec)));
        }
        else
        {
            auto readSpec =
                client.readKeySpec("key", ContractDataDurability::PERSISTENT)
                    .setReadBytes(5000);
            REQUIRE(client.has("key", ContractDataDurability::PERSISTENT,
                               std::nullopt, readSpec) ==
                    INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
            REQUIRE(isSuccess(
                client.has("key", ContractDataDurability::PERSISTENT,
                           std::nullopt, readSpec.setReadBytes(10'000))));
        }
    }

    SECTION("write bytes limit enforced")
    {
        auto writeSpec =
            client.writeKeySpec("key", ContractDataDurability::PERSISTENT)
                .setWriteBytes(5000);
        // 5kb write should fail because it exceeds the write bytes
        REQUIRE(client.resizeStorageAndExtend("key", 5, 1, 1, writeSpec) ==
                INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);

        // 5kb write should succeed with high write bytes value
        REQUIRE(isSuccess(client.resizeStorageAndExtend(
            "key", 5, 1, 1, writeSpec.setWriteBytes(10'000))));
    }

    SECTION("Same ScVal key, different types")
    {
        // Check that each type is in their own keyspace
        REQUIRE(isSuccess(
            client.put("key", ContractDataDurability::PERSISTENT, 1)));
        REQUIRE(
            isSuccess(client.put("key", ContractDataDurability::TEMPORARY, 2)));
        REQUIRE(isSuccess(
            client.get("key", ContractDataDurability::PERSISTENT, 1)));
        REQUIRE(
            isSuccess(client.get("key", ContractDataDurability::TEMPORARY, 2)));
    }

    SECTION("footprint")
    {
        REQUIRE(isSuccess(
            client.put("key", ContractDataDurability::PERSISTENT, 0)));
        auto lk = client.getContract().getDataKey(
            makeSymbolSCVal("key"), ContractDataDurability::PERSISTENT);

        SECTION("unused readWrite key")
        {
            auto acc = test.getRoot().create(
                "acc", test.getApp().getLedgerManager().getLastMinBalance(1));
            auto loadAccount = [&]() {
                LedgerTxn ltx(test.getApp().getLedgerTxnRoot());
                auto ltxe = stellar::loadAccountWithoutRecord(ltx, acc);
                REQUIRE(ltxe);
                return ltxe.current();
            };
            LedgerEntry accountEntry = loadAccount();

            REQUIRE(isSuccess(
                client.put("key", ContractDataDurability::PERSISTENT, 0,
                           client.defaultSpecWithoutFootprint()
                               .setReadWriteFootprint({lk, accountKey(acc)})
                               .setWriteBytes(5000))));

            // Make sure account still exists and hasn't changed, besides
            // `lastModifiedLedgerSeq` change.
            accountEntry.lastModifiedLedgerSeq += 1;
            REQUIRE(accountEntry == loadAccount());
        }

        SECTION("no footprint")
        {
            // Failure: client data isn't in footprint
            REQUIRE(client.put("key", ContractDataDurability::PERSISTENT, 88,
                               client.defaultSpecWithoutFootprint()) ==
                    INVOKE_HOST_FUNCTION_TRAPPED);
            REQUIRE(client.get("key", ContractDataDurability::PERSISTENT,
                               std::nullopt,
                               client.defaultSpecWithoutFootprint()) ==
                    INVOKE_HOST_FUNCTION_TRAPPED);
            REQUIRE(client.has("key", ContractDataDurability::PERSISTENT,
                               std::nullopt,
                               client.defaultSpecWithoutFootprint()) ==
                    INVOKE_HOST_FUNCTION_TRAPPED);
            REQUIRE(client.del("key", ContractDataDurability::PERSISTENT,
                               client.defaultSpecWithoutFootprint()) ==
                    INVOKE_HOST_FUNCTION_TRAPPED);
        }

        SECTION("RO footprint for RW entry")
        {
            REQUIRE(
                client.put(
                    "key", ContractDataDurability::PERSISTENT, 88,
                    client.defaultSpecWithoutFootprint().setReadOnlyFootprint(
                        {lk})) == INVOKE_HOST_FUNCTION_TRAPPED);
            REQUIRE(
                client.del(
                    "key", ContractDataDurability::PERSISTENT,
                    client.defaultSpecWithoutFootprint().setReadOnlyFootprint(
                        {lk})) == INVOKE_HOST_FUNCTION_TRAPPED);
        }
    }
}

TEST_CASE_VERSIONS("state archival", "[tx][soroban][archival]")
{
    for_versions_from(20, getTestConfig(), [](Config const& cfg) {
        SorobanTest test(cfg, true, [](SorobanNetworkConfig& cfg) {
            cfg.mRentFee1KBSorobanStateSizeLow = 20'000;
            cfg.mRentFee1KBSorobanStateSizeHigh = 1'000'000;
        });
        auto const& stateArchivalSettings =
            test.getNetworkCfg().stateArchivalSettings();
        auto isSuccess = [](auto resultCode) {
            return resultCode == INVOKE_HOST_FUNCTION_SUCCESS;
        };
        ContractStorageTestClient client(test);
        SECTION("contract instance and Wasm archival")
        {
            uint32_t originalExpectedLiveUntilLedger =
                test.getLCLSeq() + stateArchivalSettings.minPersistentTTL - 1;

            for (uint32_t i =
                     test.getApp().getLedgerManager().getLastClosedLedgerNum();
                 i <= originalExpectedLiveUntilLedger + 1; ++i)
            {
                closeLedgerOn(test.getApp(), i, 2, 1, 2016);
            }

            // Contract instance and code should be expired
            auto const& contractKeys = client.getContract().getKeys();
            REQUIRE(!test.isEntryLive(contractKeys[0], test.getLCLSeq()));
            REQUIRE(!test.isEntryLive(contractKeys[1], test.getLCLSeq()));
            // Wasm is created 1 ledger before code, so lives for 1 ledger less.
            REQUIRE(test.getTTL(contractKeys[0]) ==
                    originalExpectedLiveUntilLedger - 1);
            REQUIRE(test.getTTL(contractKeys[1]) ==
                    originalExpectedLiveUntilLedger);

            // Contract instance and code are expired, any TX should fail
            REQUIRE(client.put("temp", ContractDataDurability::TEMPORARY, 0) ==
                    INVOKE_HOST_FUNCTION_ENTRY_ARCHIVED);

            // Note, that post protocol 23 the rent bumps are cheaper. The
            // reason for that is that due to the way the test is set up, the
            // rent bump fee has been dominated by the TTL write fee (because
            // the write fee itself is high). After protocol 23 the write fee
            // is just 3500, and the rent fee itself is small in both cases due
            // to large denominator.
            // We should eventually update this test to use the small
            // denominators instead of large write fees in order to get more
            // sensible numbers, but keeping it as is for now in order to
            // ensure that protocols before 23 are not broken.
            int const rentBumpForWasm =
                protocolVersionStartsFrom(test.getLedgerVersion(),
                                          ProtocolVersion::V_23)
                    ? 285
                    : 943;
            int const rentBumpForInstance =
                protocolVersionStartsFrom(test.getLedgerVersion(),
                                          ProtocolVersion::V_23)
                    ? 166
                    : 939;
            int const rentBumpForInstanceAndWasm =
                protocolVersionStartsFrom(test.getLedgerVersion(),
                                          ProtocolVersion::V_23)
                    ? 450
                    : 1881;

            SECTION("restore contract instance and wasm")
            {
                // Restore Instance and Wasm
                test.invokeRestoreOp(contractKeys, rentBumpForInstanceAndWasm +
                                                    40000 /* two LE-writes
                                                    */);
                auto newExpectedLiveUntilLedger =
                    test.getLCLSeq() + stateArchivalSettings.minPersistentTTL -
                    1;

                // Instance should now be useable
                REQUIRE(isSuccess(
                    client.put("temp", ContractDataDurability::TEMPORARY, 0)));
                REQUIRE(test.getTTL(contractKeys[0]) ==
                        newExpectedLiveUntilLedger);
                REQUIRE(test.getTTL(contractKeys[1]) ==
                        newExpectedLiveUntilLedger);
            }

            SECTION("restore contract instance, not wasm")
            {
                // Only restore client instance
                test.invokeRestoreOp({contractKeys[1]},
                                     rentBumpForInstance +
                                         20000 /* one LE write */);
                auto newExpectedLiveUntilLedger =
                    test.getLCLSeq() + stateArchivalSettings.minPersistentTTL -
                    1;

                // invocation should fail
                REQUIRE(client.put("temp", ContractDataDurability::TEMPORARY,
                                   0) == INVOKE_HOST_FUNCTION_ENTRY_ARCHIVED);
                // Wasm is created 1 ledger before code, so lives for 1 ledger
                // less.
                REQUIRE(test.getTTL(contractKeys[0]) ==
                        originalExpectedLiveUntilLedger - 1);
                REQUIRE(test.getTTL(contractKeys[1]) ==
                        newExpectedLiveUntilLedger);
            }

            SECTION("restore contract Wasm, not instance")
            {
                // Only restore Wasm
                test.invokeRestoreOp({contractKeys[0]},
                                     rentBumpForWasm +
                                         20000 /* one LE write */);
                auto newExpectedLiveUntilLedger =
                    test.getLCLSeq() + stateArchivalSettings.minPersistentTTL -
                    1;

                // invocation should fail
                REQUIRE(client.put("temp", ContractDataDurability::TEMPORARY,
                                   0) == INVOKE_HOST_FUNCTION_ENTRY_ARCHIVED);
                REQUIRE(test.getTTL(contractKeys[0]) ==
                        newExpectedLiveUntilLedger);
                REQUIRE(test.getTTL(contractKeys[1]) ==
                        originalExpectedLiveUntilLedger);
            }

            SECTION("lifetime extensions")
            {
                // Restore Instance and Wasm
                test.invokeRestoreOp(contractKeys,
                                     rentBumpForInstanceAndWasm +
                                         40000 /* two LE writes */);
                test.invokeExtendOp({contractKeys[0]}, 10'000);
                test.invokeExtendOp({contractKeys[1]}, 15'000);
                REQUIRE(test.getTTL(contractKeys[0]) ==
                        test.getLCLSeq() + 10'000 - 1);
                // No -1 here because instance lives for 1 ledger longer than
                // Wasm.
                REQUIRE(test.getTTL(contractKeys[1]) ==
                        test.getLCLSeq() + 15'000);
            }
        }

        SECTION("contract storage archival")
        {
            // Wasm and instance should not expire during test
            test.invokeExtendOp(client.getContract().getKeys(), 10'000);

            REQUIRE(isSuccess(client.put(
                "persistent", ContractDataDurability::PERSISTENT, 10)));
            // Extend the persistent entry, so that it doesn't expire while we
            // execute all the contract calls (every one closes a ledger).
            REQUIRE(isSuccess(client.extend(
                "persistent", ContractDataDurability::PERSISTENT, 20, 32)));
            auto expectedPersistentLiveUntilLedger = test.getLCLSeq() + 32;
            REQUIRE(isSuccess(
                client.put("temp", ContractDataDurability::TEMPORARY, 0)));
            auto expectedTempLiveUntilLedger =
                test.getLCLSeq() + stateArchivalSettings.minTemporaryTTL - 1;

            // Check for expected minimum lifetime values
            REQUIRE(client.getTTL("persistent",
                                  ContractDataDurability::PERSISTENT) ==
                    expectedPersistentLiveUntilLedger);
            REQUIRE(client.getTTL("temp", ContractDataDurability::TEMPORARY) ==
                    expectedTempLiveUntilLedger);

            // Close ledgers until temp entry expires
            uint32 nextLedgerSeq =
                test.getApp().getLedgerManager().getLastClosedLedgerNum();
            for (; nextLedgerSeq < expectedTempLiveUntilLedger; ++nextLedgerSeq)
            {
                closeLedgerOn(test.getApp(), nextLedgerSeq, 2, 1, 2016);
            }

            REQUIRE(test.getLCLSeq() == expectedTempLiveUntilLedger - 1);

            SECTION("entry accessible when currentLedger == liveUntilLedger")
            {
                // Entry should still be accessible when currentLedger ==
                // liveUntilLedgerSeq
                REQUIRE(isSuccess(client.has(
                    "temp", ContractDataDurability::TEMPORARY, true)));
            }

            SECTION("write does not increase TTL")
            {
                REQUIRE(isSuccess(
                    client.put("temp", ContractDataDurability::TEMPORARY, 42)));
                // TTL should not be network minimum since entry already exists
                REQUIRE(
                    client.getTTL("temp", ContractDataDurability::TEMPORARY) ==
                    expectedTempLiveUntilLedger);
            }

            SECTION("extendOp when currentLedger == liveUntilLedger")
            {
                test.invokeExtendOp({client.getContract().getDataKey(
                                        makeSymbolSCVal("temp"),
                                        ContractDataDurability::TEMPORARY)},
                                    10'000);
                REQUIRE(
                    client.getTTL("temp", ContractDataDurability::TEMPORARY) ==
                    test.getLCLSeq() + 10'000);
            }

            SECTION("TTL enforcement")
            {
                // Close one more ledger so temp entry is expired
                closeLedgerOn(test.getApp(), nextLedgerSeq++, 2, 1, 2016);
                REQUIRE(test.getLCLSeq() == expectedTempLiveUntilLedger);

                // Check that temp entry has expired in the current ledger, i.e.
                // after LCL.
                REQUIRE(!client.isEntryLive("temp",
                                            ContractDataDurability::TEMPORARY,
                                            test.getLCLSeq() + 1));

                // Get should fail since entry no longer exists
                REQUIRE(client.get("temp", ContractDataDurability::TEMPORARY,
                                   std::nullopt) ==
                        INVOKE_HOST_FUNCTION_TRAPPED);

                // Has should succeed since the entry is TEMPORARY, but should
                // return false
                REQUIRE(isSuccess(client.has(
                    "temp", ContractDataDurability::TEMPORARY, false)));

                // PERSISTENT entry is still live, has higher minimum TTL
                REQUIRE(client.isEntryLive("persistent",
                                           ContractDataDurability::PERSISTENT,
                                           test.getLCLSeq()));
                REQUIRE(isSuccess(client.has(
                    "persistent", ContractDataDurability::PERSISTENT, true)));

                // Check that we can recreate an expired TEMPORARY entry
                REQUIRE(isSuccess(
                    client.put("temp", ContractDataDurability::TEMPORARY, 42)));

                // Recreated entry should be live
                REQUIRE(
                    client.getTTL("temp", ContractDataDurability::TEMPORARY) ==
                    test.getLCLSeq() + stateArchivalSettings.minTemporaryTTL -
                        1);
                REQUIRE(isSuccess(
                    client.get("temp", ContractDataDurability::TEMPORARY, 42)));
                nextLedgerSeq = test.getLCLSeq() + 1;
                // Close ledgers until PERSISTENT entry liveUntilLedger
                for (; nextLedgerSeq < expectedPersistentLiveUntilLedger;
                     ++nextLedgerSeq)
                {
                    closeLedgerOn(test.getApp(), nextLedgerSeq, 2, 1, 2016);
                }

                SECTION(
                    "entry accessible when currentLedger == liveUntilLedger")
                {
                    REQUIRE(isSuccess(
                        client.has("persistent",
                                   ContractDataDurability::PERSISTENT, true)));
                }
                auto lk = client.getContract().getDataKey(
                    makeSymbolSCVal("persistent"),
                    ContractDataDurability::PERSISTENT);
                SECTION("restoreOp skips when currentLedger == liveUntilLedger")
                {
                    // Restore should skip entry, refund all of refundableFee
                    test.invokeRestoreOp({lk}, 0);

                    // TTL should be unchanged
                    REQUIRE(client.getTTL("persistent",
                                          ContractDataDurability::PERSISTENT) ==
                            expectedPersistentLiveUntilLedger);
                }

                // Close one more ledger so entry is expired
                closeLedgerOn(test.getApp(), nextLedgerSeq++, 2, 1, 2016);
                REQUIRE(
                    test.getApp().getLedgerManager().getLastClosedLedgerNum() ==
                    expectedPersistentLiveUntilLedger);

                // Check that persistent entry has expired in the current ledger
                REQUIRE(!client.isEntryLive("persistent",
                                            ContractDataDurability::PERSISTENT,
                                            test.getLCLSeq() + 1));

                // Check that we can't recreate expired PERSISTENT
                REQUIRE(client.put("persistent",
                                   ContractDataDurability::PERSISTENT,
                                   42) == INVOKE_HOST_FUNCTION_ENTRY_ARCHIVED);

                // Since entry is PERSISTENT, has should fail
                REQUIRE(client.has("persistent",
                                   ContractDataDurability::PERSISTENT,
                                   std::nullopt) ==
                        INVOKE_HOST_FUNCTION_ENTRY_ARCHIVED);

                client.put("persistent2", ContractDataDurability::PERSISTENT,
                           0);
                auto lk2 = client.getContract().getDataKey(
                    makeSymbolSCVal("persistent2"),
                    ContractDataDurability::PERSISTENT);
                uint32_t expectedLiveUntilLedger2 =
                    test.getLCLSeq() + stateArchivalSettings.minPersistentTTL -
                    1;
                REQUIRE(client.getTTL("persistent2",
                                      ContractDataDurability::PERSISTENT) ==
                        expectedLiveUntilLedger2);

                // Restore ARCHIVED key and LIVE key, should only be charged for
                // one
                test.invokeRestoreOp(
                    {lk, lk2}, /*charge for one entry*/
                    protocolVersionStartsFrom(test.getLedgerVersion(),
                                              ProtocolVersion::V_23)
                        ? 20'166
                        : 20'939);

                // Live entry TTL should be unchanged
                REQUIRE(client.getTTL("persistent2",
                                      ContractDataDurability::PERSISTENT) ==
                        expectedLiveUntilLedger2);

                // Check value and TTL of restored entry
                REQUIRE(client.getTTL("persistent",
                                      ContractDataDurability::PERSISTENT) ==
                        test.getLCLSeq() +
                            stateArchivalSettings.minPersistentTTL - 1);
                REQUIRE(isSuccess(client.get(
                    "persistent", ContractDataDurability::PERSISTENT, 10)));
            }
        }

        SECTION("conditional TTL extension")
        {
            // Large bump, followed by smaller bump
            REQUIRE(isSuccess(
                client.put("key", ContractDataDurability::PERSISTENT, 0)));
            REQUIRE(isSuccess(client.extend(
                "key", ContractDataDurability::PERSISTENT, 10'000, 10'000)));
            auto originalExpectedLiveUntilLedger = test.getLCLSeq() + 10'000;
            REQUIRE(client.getTTL("key", ContractDataDurability::PERSISTENT) ==
                    originalExpectedLiveUntilLedger);

            // Expiration already above 5'000, should be a no-op
            REQUIRE(isSuccess(client.extend(
                "key", ContractDataDurability::PERSISTENT, 5'000, 5'000)));
            REQUIRE(client.getTTL("key", ContractDataDurability::PERSISTENT) ==
                    originalExpectedLiveUntilLedger);

            // Small bump followed by larger bump
            REQUIRE(isSuccess(
                client.put("key2", ContractDataDurability::PERSISTENT, 0)));
            REQUIRE(isSuccess(client.extend(
                "key2", ContractDataDurability::PERSISTENT, 10'000, 10'000)));
            REQUIRE(client.getTTL("key2", ContractDataDurability::PERSISTENT) ==
                    test.getLCLSeq() + 10'000);

            REQUIRE(isSuccess(
                client.put("key3", ContractDataDurability::PERSISTENT, 0)));
            REQUIRE(isSuccess(client.extend(
                "key3", ContractDataDurability::PERSISTENT, 50'000, 50'000)));
            uint32_t key3ExpectedLiveUntilLedger = test.getLCLSeq() + 50'000;
            REQUIRE(client.getTTL("key3", ContractDataDurability::PERSISTENT) ==
                    key3ExpectedLiveUntilLedger);

            // Bump multiple keys to live 10100 ledger from now
            xdr::xvector<LedgerKey> keysToExtend = {
                client.getContract().getDataKey(
                    makeSymbolSCVal("key"), ContractDataDurability::PERSISTENT),
                client.getContract().getDataKey(
                    makeSymbolSCVal("key2"),
                    ContractDataDurability::PERSISTENT),
                client.getContract().getDataKey(
                    makeSymbolSCVal("key3"),
                    ContractDataDurability::PERSISTENT)};

            SECTION("calculate refund")
            {
                test.invokeExtendOp(keysToExtend, 10'100);
                REQUIRE(
                    client.getTTL("key", ContractDataDurability::PERSISTENT) ==
                    test.getLCLSeq() + 10'100);
                REQUIRE(
                    client.getTTL("key2", ContractDataDurability::PERSISTENT) ==
                    test.getLCLSeq() + 10'100);

                // No change for key3 since expiration is already past 10100
                // ledgers from now
                REQUIRE(
                    client.getTTL("key3", ContractDataDurability::PERSISTENT) ==
                    key3ExpectedLiveUntilLedger);
            }

            // Check same extendOp with hardcoded expected refund to detect if
            // refund logic changes unexpectedly
            SECTION("absolute refund")
            {
                int64_t const expectedRefund =
                    protocolVersionStartsFrom(test.getLedgerVersion(),
                                              ProtocolVersion::V_23)
                        ? 40'331
                        : 41'877;
                test.invokeExtendOp(keysToExtend, 10'100, expectedRefund);
                REQUIRE(
                    client.getTTL("key", ContractDataDurability::PERSISTENT) ==
                    test.getLCLSeq() + 10'100);
                REQUIRE(
                    client.getTTL("key2", ContractDataDurability::PERSISTENT) ==
                    test.getLCLSeq() + 10'100);

                // No change for key3 since expiration is already past 10100
                // ledgers from now
                REQUIRE(
                    client.getTTL("key3", ContractDataDurability::PERSISTENT) ==
                    key3ExpectedLiveUntilLedger);
            }
        }

        SECTION("TTL threshold")
        {
            REQUIRE(isSuccess(
                client.put("key", ContractDataDurability::PERSISTENT, 0)));
            uint32_t initialLiveUntilLedger =
                test.getLCLSeq() + stateArchivalSettings.minPersistentTTL - 1;
            REQUIRE(client.getTTL("key", ContractDataDurability::PERSISTENT) ==
                    initialLiveUntilLedger);
            // Try extending entry, but set the threshold 1 ledger below the
            // current TTL.
            uint32_t currentTTL = stateArchivalSettings.minPersistentTTL - 2;
            REQUIRE(isSuccess(client.extend("key",
                                            ContractDataDurability::PERSISTENT,
                                            currentTTL - 1, 50'000)));
            REQUIRE(client.getTTL("key", ContractDataDurability::PERSISTENT) ==
                    initialLiveUntilLedger);

            // Ledger has been advanced by client.extend, so now exactly the
            // same call should extend the TTL.
            REQUIRE(isSuccess(client.extend("key",
                                            ContractDataDurability::PERSISTENT,
                                            currentTTL - 1, 50'000)));
            REQUIRE(client.getTTL("key", ContractDataDurability::PERSISTENT) ==
                    test.getLCLSeq() + 50'000);

            // Check that threshold > extendTo fails
            REQUIRE(client.extend("key", ContractDataDurability::PERSISTENT,
                                  60'001,
                                  60'000) == INVOKE_HOST_FUNCTION_TRAPPED);
        }

        SECTION("max TTL extension")
        {
            REQUIRE(isSuccess(
                client.put("key", ContractDataDurability::PERSISTENT, 0)));
            auto lk = client.getContract().getDataKey(
                makeSymbolSCVal("key"), ContractDataDurability::PERSISTENT);

            SECTION("extension op")
            {
                // Max TTL includes current ledger, so subtract 1
                test.invokeExtendOp({lk},
                                    stateArchivalSettings.maxEntryTTL - 1);
                REQUIRE(test.getTTL(lk) ==
                        test.getLCLSeq() + stateArchivalSettings.maxEntryTTL -
                            1);
            }

            SECTION("extend host function persistent")
            {
                REQUIRE(isSuccess(client.extend(
                    "key", ContractDataDurability::PERSISTENT, 100, 100)));
                REQUIRE(test.getTTL(lk) == 100 + test.getLCLSeq());

                REQUIRE(isSuccess(client.extend(
                    "key", ContractDataDurability::PERSISTENT,
                    stateArchivalSettings.maxEntryTTL + 10,
                    stateArchivalSettings.maxEntryTTL + 10,
                    client
                        .readKeySpec("key", ContractDataDurability::PERSISTENT)
                        .setRefundableResourceFee(80'000))));

                // Capped at max (Max TTL includes current ledger, so subtract
                // 1)
                REQUIRE(test.getTTL(lk) ==
                        test.getLCLSeq() + stateArchivalSettings.maxEntryTTL -
                            1);
            }

            SECTION("extend host function temp")
            {
                REQUIRE(isSuccess(
                    client.put("key", ContractDataDurability::TEMPORARY, 0)));
                auto lkTemp = client.getContract().getDataKey(
                    makeSymbolSCVal("key"), ContractDataDurability::TEMPORARY);
                uint32_t ledgerSeq = test.getLCLSeq();
                REQUIRE(client.extend("key", ContractDataDurability::TEMPORARY,
                                      stateArchivalSettings.maxEntryTTL,
                                      stateArchivalSettings.maxEntryTTL) ==
                        INVOKE_HOST_FUNCTION_TRAPPED);
                REQUIRE(test.getTTL(lkTemp) ==
                        ledgerSeq + stateArchivalSettings.minTemporaryTTL - 1);

                // Max TTL includes current ledger, so subtract 1
                REQUIRE(isSuccess(
                    client.extend("key", ContractDataDurability::TEMPORARY,
                                  stateArchivalSettings.maxEntryTTL - 1,
                                  stateArchivalSettings.maxEntryTTL - 1)));
                REQUIRE(test.getTTL(lkTemp) ==
                        test.getLCLSeq() + stateArchivalSettings.maxEntryTTL -
                            1);
            }
        }
    });
}

TEST_CASE("charge rent fees for storage resize", "[tx][soroban]")
{
    SorobanTest test(getTestConfig(), true, [](SorobanNetworkConfig& cfg) {
        cfg.mRentFee1KBSorobanStateSizeLow = 1'000;
        cfg.mRentFee1KBSorobanStateSizeHigh = 1'000'000;
    });
    auto isSuccess = [](auto resultCode) {
        return resultCode == INVOKE_HOST_FUNCTION_SUCCESS;
    };

    ContractStorageTestClient client(test);
    auto spec = client.writeKeySpec("key", ContractDataDurability::PERSISTENT)
                    .setWriteBytes(10'000);
    // Create 1 KB entry extended to live for 1'000'000 ledgers
    REQUIRE(isSuccess(
        client.resizeStorageAndExtend("key", 1, 1'000'000, 1'000'000, spec)));

    SECTION("resize with no extend")
    {
        uint32_t const expectedRefundableFee = 15'844;
        REQUIRE(client.resizeStorageAndExtend(
                    "key", 5, 1'000'000 - 2, 1'000'000,
                    spec.setRefundableResourceFee(expectedRefundableFee - 1)) ==
                INVOKE_HOST_FUNCTION_INSUFFICIENT_REFUNDABLE_FEE);
        REQUIRE(isSuccess(client.resizeStorageAndExtend(
            "key", 5, 1'000'000 - 3, 1'000'000,
            spec.setRefundableResourceFee(expectedRefundableFee))));
    }

    SECTION("resize and extend")
    {
        uint32_t const expectedRefundableFee = 56107;
        REQUIRE(client.resizeStorageAndExtend(
                    "key", 5, 2'000'000, 2'000'000,
                    spec.setRefundableResourceFee(expectedRefundableFee - 1)) ==
                INVOKE_HOST_FUNCTION_INSUFFICIENT_REFUNDABLE_FEE);
        REQUIRE(isSuccess(client.resizeStorageAndExtend(
            "key", 5, 2'000'000, 2'000'000,
            spec.setRefundableResourceFee(expectedRefundableFee))));
    }
}

TEST_CASE_VERSIONS("archival meta", "[tx][soroban][archival]")
{
    auto test = [](Config& cfg) {
        TmpDirManager tdm(std::string("soroban-storage-meta-") +
                          binToHex(randomBytes(8)));
        TmpDir td = tdm.tmpDir("soroban-meta-ok");
        std::string metaPath = td.getName() + "/stream.xdr";

        cfg.METADATA_OUTPUT_STREAM = metaPath;

        SorobanTest test(cfg);
        ContractStorageTestClient client(test);
        auto const& contractKeys = client.getContract().getKeys();

        // Extend Wasm and instance
        test.invokeExtendOp(contractKeys, 10'000);

        auto invocation = client.getContract().prepareInvocation(
            "put_temporary", {makeSymbolSCVal("key"), makeU64SCVal(123)},
            client.writeKeySpec("key", ContractDataDurability::TEMPORARY));
        REQUIRE(invocation.withExactNonRefundableResourceFee().invoke());
        auto temporaryLk = client.getContract().getDataKey(
            makeSymbolSCVal("key"), ContractDataDurability::TEMPORARY);

        auto expectedLiveUntilLedger =
            test.getLCLSeq() +
            test.getNetworkCfg().stateArchivalSettings().minTemporaryTTL - 1;
        REQUIRE(test.getTTL(temporaryLk) == expectedLiveUntilLedger);
        auto const evictionLedger = 4097;

        // Close ledgers until temp entry is evicted
        for (uint32_t i = test.getLCLSeq(); i < evictionLedger - 2; ++i)
        {
            closeLedgerOn(test.getApp(), i, 2, 1, 2016);
        }

        REQUIRE(test.getTTL(temporaryLk) == expectedLiveUntilLedger);

        // This should be a noop
        test.invokeExtendOp({temporaryLk}, 10'000, 0);
        REQUIRE(test.getTTL(temporaryLk) == expectedLiveUntilLedger);

        // This will fail because the entry is expired
        REQUIRE(client.extend("key", ContractDataDurability::TEMPORARY, 10'000,
                              10'000) == INVOKE_HOST_FUNCTION_TRAPPED);
        REQUIRE(test.getTTL(temporaryLk) == expectedLiveUntilLedger);

        REQUIRE(!test.isEntryLive(temporaryLk, test.getLCLSeq()));

        SECTION("temp entry meta")
        {
            // close one more ledger to trigger the eviction
            closeLedgerOn(test.getApp(), evictionLedger, 2, 1, 2016);

            {
                LedgerTxn ltx(test.getApp().getLedgerTxnRoot());
                REQUIRE(!ltx.load(temporaryLk));
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
                    REQUIRE(lcm.v1().evictedKeys.size() == 2);
                    auto sortedKeys = lcm.v1().evictedKeys;
                    std::sort(sortedKeys.begin(), sortedKeys.end());
                    REQUIRE(sortedKeys[0] == temporaryLk);
                    REQUIRE(sortedKeys[1] == getTTLKey(temporaryLk));
                    evicted = true;
                }
                else
                {
                    REQUIRE(lcm.v1().evictedKeys.empty());
                }
            }

            REQUIRE(evicted);
        }
        SECTION("persistent entry meta")
        {
            auto persistentInvocation = client.getContract().prepareInvocation(
                "put_persistent", {makeSymbolSCVal("key"), makeU64SCVal(123)},
                client.writeKeySpec("key", ContractDataDurability::PERSISTENT));
            REQUIRE(persistentInvocation.withExactNonRefundableResourceFee()
                        .invoke());

            auto persistentKey = client.getContract().getDataKey(
                makeSymbolSCVal("key"), ContractDataDurability::PERSISTENT);

            LedgerEntry persistentLE;
            {
                LedgerTxn ltx(test.getApp().getLedgerTxnRoot());
                auto ltxe = ltx.load(persistentKey);
                REQUIRE(ltxe);
                persistentLE = ltxe.current();
            }

            auto testRestore = [&](std::optional<uint32_t> updatedValue =
                                       std::nullopt) {
                auto targetRestorationLedger = test.getLCLSeq();

                XDRInputFileStream in;
                in.open(metaPath);
                LedgerCloseMeta lcm;
                bool restoreMeta = false;
                bool seenUpdated = false;

                // TTL Restore, Data Restore, optional Data updated
                auto numChanges = updatedValue ? 3 : 2;

                LedgerKeySet keysToRestore = {persistentKey,
                                              getTTLKey(persistentKey)};
                LedgerKeySet updatedKeys =
                    updatedValue ? LedgerKeySet{persistentKey} : LedgerKeySet{};

                while (in.readOne(lcm))
                {
                    REQUIRE(lcm.v() == 1);
                    if (lcm.v1().ledgerHeader.header.ledgerSeq ==
                        targetRestorationLedger)
                    {
                        REQUIRE(lcm.v1().evictedKeys.empty());
                        REQUIRE(lcm.v1().unused.empty());

                        REQUIRE(lcm.v1().txProcessing.size() == 1);
                        auto txMeta = lcm.v1().txProcessing.front();
                        auto txApplyProcessing =
                            TransactionMetaFrame(txMeta.txApplyProcessing);
                        REQUIRE(txApplyProcessing.getNumOperations() == 1);

                        auto const& changes =
                            txApplyProcessing.getLedgerEntryChangesAtOp(0);
                        REQUIRE(changes.size() == numChanges);
                        for (auto const& change : changes)
                        {
                            if (change.type() ==
                                LedgerEntryChangeType::LEDGER_ENTRY_RESTORED)
                            {
                                auto le = change.restored();
                                auto lk = LedgerEntryKey(le);
                                REQUIRE(keysToRestore.find(lk) !=
                                        keysToRestore.end());
                                keysToRestore.erase(lk);

                                // If the restored value was also updated,
                                // RESTORE meta will hold the previous value
                                if (updatedKeys.find(lk) == updatedKeys.end())
                                {
                                    LedgerTxn ltx(
                                        test.getApp().getLedgerTxnRoot());
                                    auto ltxe = ltx.load(lk);
                                    REQUIRE(ltxe);
                                    REQUIRE(ltxe.current() == le);
                                }
                            }
                            else if (change.type() == LedgerEntryChangeType::
                                                          LEDGER_ENTRY_UPDATED)
                            {
                                auto le = change.updated();
                                auto lk = LedgerEntryKey(le);
                                REQUIRE(updatedKeys.find(lk) !=
                                        updatedKeys.end());
                                seenUpdated = true;

                                LedgerTxn ltx(test.getApp().getLedgerTxnRoot());
                                auto ltxe = ltx.load(lk);
                                REQUIRE(ltxe);
                                REQUIRE(ltxe.current() == le);
                                REQUIRE(
                                    ltxe.current().data.contractData().val ==
                                    makeU64SCVal(*updatedValue));
                            }
                            else
                            {
                                FAIL();
                            }
                        }

                        restoreMeta = true;
                        break;
                    }
                }

                REQUIRE(restoreMeta);
                REQUIRE(seenUpdated == (updatedValue.has_value()));
                REQUIRE(keysToRestore.empty());
            };

            auto persistentLk = client.getContract().getDataKey(
                makeSymbolSCVal("key"), ContractDataDurability::PERSISTENT);

            // First, close ledgers until entry is expired, but not yet evicted
            auto expirationLedger =
                test.getLCLSeq() +
                test.getNetworkCfg().stateArchivalSettings().minPersistentTTL;
            for (uint32_t i = test.getLCLSeq(); i <= expirationLedger; ++i)
            {
                closeLedgerOn(test.getApp(), i, 2, 1, 2016);
            }
            REQUIRE(!test.isEntryLive(persistentKey, test.getLCLSeq()));

            auto spec =
                client.defaultSpecWithoutFootprint()
                    .setReadWriteFootprint({persistentKey})
                    .setArchivedIndexes({0})
                    .setReadOnlyFootprint(client.getContract().getKeys())
                    .setReadBytes(5000)
                    .setWriteBytes(5000)
                    .setRefundableResourceFee(1'000'000);

            // First, check restoration meta in non-evicted case. This should be
            // identical to meta in the evicted case.
            SECTION("restore expired but not evicted")
            {
                SECTION("manual restore")
                {
                    test.invokeRestoreOp({persistentKey}, 20'166);
                    testRestore();
                }

                SECTION("autorestore")
                {
                    if (protocolVersionStartsFrom(
                            cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION,
                            AUTO_RESTORE_PROTOCOL_VERSION))
                    {
                        auto invocation =
                            client.getContract().prepareInvocation(
                                "has_persistent", {makeSymbolSCVal("key")},
                                spec, /* addContractKeys */ false);
                        REQUIRE(invocation.withExactNonRefundableResourceFee()
                                    .invoke());
                        testRestore();
                    }
                }

                SECTION("autorestore with updated value")
                {
                    if (protocolVersionStartsFrom(
                            cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION,
                            AUTO_RESTORE_PROTOCOL_VERSION))
                    {
                        auto invocation =
                            client.getContract().prepareInvocation(
                                "put_persistent",
                                {makeSymbolSCVal("key"), makeU64SCVal(999)},
                                spec, /* addContractKeys */ false);
                        REQUIRE(invocation.withExactNonRefundableResourceFee()
                                    .invoke());
                        testRestore(999);
                    }
                }
            }

            SECTION("entry evicted")
            {
                // Entry must merge down the BucketList until it is in the first
                // scan level
                auto evictionLedger = 8193;

                // Close ledgers until entry is evicted
                for (uint32_t i = test.getLCLSeq(); i <= evictionLedger; ++i)
                {
                    closeLedgerOn(test.getApp(), i, 2, 1, 2016);
                }

                if (protocolVersionStartsFrom(
                        cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION,
                        LiveBucket::
                            FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION))
                {
                    LedgerTxn ltx(test.getApp().getLedgerTxnRoot());
                    REQUIRE(!ltx.load(persistentKey));
                }

                SECTION("eviction meta")
                {
                    XDRInputFileStream in;
                    in.open(metaPath);
                    LedgerCloseMeta lcm;
                    bool evicted = false;
                    LedgerKeySet keysToEvict = {persistentKey,
                                                getTTLKey(persistentKey)};
                    while (in.readOne(lcm))
                    {
                        REQUIRE(lcm.v() == 1);
                        if (lcm.v1().ledgerHeader.header.ledgerSeq ==
                            evictionLedger)
                        {
                            // Only support persistent eviction meta >= p23
                            if (protocolVersionStartsFrom(
                                    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION,
                                    LiveBucket::
                                        FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION))
                            {
                                REQUIRE(lcm.v1().evictedKeys.size() == 2);
                                for (auto const& key : lcm.v1().evictedKeys)
                                {
                                    REQUIRE(keysToEvict.find(key) !=
                                            keysToEvict.end());
                                    keysToEvict.erase(key);
                                }

                                // This field should always be empty and never
                                // used. The field only exists for legacy
                                // reasons.
                                REQUIRE(lcm.v1().unused.empty());
                                evicted = true;
                            }
                            else
                            {
                                REQUIRE(lcm.v1().evictedKeys.empty());
                                REQUIRE(lcm.v1().unused.empty());
                                evicted = false;
                            }

                            break;
                        }
                    }

                    if (protocolVersionStartsFrom(
                            cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION,
                            LiveBucket::
                                FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION))
                    {
                        REQUIRE(evicted);
                        REQUIRE(keysToEvict.empty());
                    }
                    else
                    {
                        REQUIRE(!evicted);
                    }
                }

                SECTION("manual restore")
                {
                    test.invokeRestoreOp({persistentKey}, 20'166);
                    testRestore();
                }

                SECTION("autorestore")
                {
                    if (protocolVersionStartsFrom(
                            cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION,
                            LiveBucket::
                                FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION))
                    {
                        auto invocation =
                            client.getContract().prepareInvocation(
                                "has_persistent", {makeSymbolSCVal("key")},
                                spec, /* addContractKeys */ false);
                        REQUIRE(invocation.withExactNonRefundableResourceFee()
                                    .invoke());
                        testRestore();
                    }
                }

                SECTION("autorestore with updated value")
                {
                    if (protocolVersionStartsFrom(
                            cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION,
                            LiveBucket::
                                FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION))
                    {
                        auto invocation =
                            client.getContract().prepareInvocation(
                                "put_persistent",
                                {makeSymbolSCVal("key"), makeU64SCVal(999)},
                                spec, /* addContractKeys */ false);
                        REQUIRE(invocation.withExactNonRefundableResourceFee()
                                    .invoke());
                        testRestore(999);
                    }
                }
            }
        }
        SECTION(
            "Create temp entry with same key as an expired entry on eviction "
            "ledger")
        {
            REQUIRE(client.put("key", ContractDataDurability::TEMPORARY, 234) ==
                    INVOKE_HOST_FUNCTION_SUCCESS);
            {
                LedgerTxn ltx(test.getApp().getLedgerTxnRoot());
                REQUIRE(ltx.load(temporaryLk));
            }

            // Verify that we're on the ledger where the entry would get evicted
            // it wasn't recreated.
            REQUIRE(test.getLCLSeq() == evictionLedger);

            // Entry is live again
            REQUIRE(test.isEntryLive(temporaryLk, test.getLCLSeq()));

            // Verify that we didn't emit an eviction
            XDRInputFileStream in;
            in.open(metaPath);
            LedgerCloseMeta lcm;
            while (in.readOne(lcm))
            {
                REQUIRE(lcm.v1().evictedKeys.empty());
            }
        }
    };

    auto cfg = getTestConfig();
    for_versions(20, Config::CURRENT_LEDGER_PROTOCOL_VERSION, cfg, test);
}

TEST_CASE_VERSIONS("state archival operation errors", "[tx][soroban][archival]")
{
    auto cfg = getTestConfig();
    if (protocolVersionIsBefore(cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION,
                                SOROBAN_PROTOCOL_VERSION))
    {
        return;
    }

    SorobanTest test(cfg);
    ContractStorageTestClient client(test);
    auto const& stateArchivalSettings =
        test.getNetworkCfg().stateArchivalSettings();

    REQUIRE(client.resizeStorageAndExtend("k1", 5, 0, 0) ==
            INVOKE_HOST_FUNCTION_SUCCESS);
    REQUIRE(client.resizeStorageAndExtend("k2", 3, 0, 0) ==
            INVOKE_HOST_FUNCTION_SUCCESS);
    uint32_t k2LiveUntilLedger =
        test.getLCLSeq() + stateArchivalSettings.minPersistentTTL - 1;
    xdr::xvector<LedgerKey> dataKeys = {
        client.getContract().getDataKey(makeSymbolSCVal("k1"),
                                        ContractDataDurability::PERSISTENT),
        client.getContract().getDataKey(makeSymbolSCVal("k2"),
                                        ContractDataDurability::PERSISTENT)};

    SECTION("restore operation")
    {
        for (uint32_t i =
                 test.getApp().getLedgerManager().getLastClosedLedgerNum();
             i <= k2LiveUntilLedger; ++i)
        {
            closeLedgerOn(test.getApp(), i, 2, 1, 2016);
        }
        SorobanResources restoreResources;
        restoreResources.footprint.readWrite = dataKeys;
        restoreResources.diskReadBytes = 9'000;
        restoreResources.writeBytes = 9'000;

        auto const nonRefundableResourceFee =
            sorobanResourceFee(test.getApp(), restoreResources, 400, 0);
        auto const rentFee = 100'000;
        auto const resourceFee = nonRefundableResourceFee + rentFee;

        SECTION("insufficient refundable fee")
        {
            auto tx = test.createRestoreTx(restoreResources, 1'000,
                                           nonRefundableResourceFee);
            auto result = test.invokeTx(tx);
            REQUIRE(!isSuccessResult(result));
            REQUIRE(result.result.results()[0]
                        .tr()
                        .restoreFootprintResult()
                        .code() ==
                    RESTORE_FOOTPRINT_INSUFFICIENT_REFUNDABLE_FEE);
        }

        SECTION("exceeded readBytes")
        {
            auto resourceCopy = restoreResources;
            resourceCopy.diskReadBytes = 8'000;
            auto tx = test.createRestoreTx(resourceCopy, 1'000, resourceFee);
            auto result = test.invokeTx(tx);
            REQUIRE(!isSuccessResult(result));
            REQUIRE(result.result.results()[0]
                        .tr()
                        .restoreFootprintResult()
                        .code() == RESTORE_FOOTPRINT_RESOURCE_LIMIT_EXCEEDED);
        }

        SECTION("exceeded writeBytes")
        {
            auto resourceCopy = restoreResources;
            resourceCopy.writeBytes = 8'000;
            auto tx = test.createRestoreTx(resourceCopy, 1'000, resourceFee);
            auto result = test.invokeTx(tx);
            REQUIRE(!isSuccessResult(result));
            REQUIRE(result.result.results()[0]
                        .tr()
                        .restoreFootprintResult()
                        .code() == RESTORE_FOOTPRINT_RESOURCE_LIMIT_EXCEEDED);
        }
        SECTION("success")
        {
            auto tx =
                test.createRestoreTx(restoreResources, 1'000, resourceFee);
            REQUIRE(isSuccessResult(test.invokeTx(tx)));
            REQUIRE(test.isEntryLive(dataKeys[0], test.getLCLSeq()));
            REQUIRE(test.isEntryLive(dataKeys[1], test.getLCLSeq()));
        }
    }

    SECTION("extend operation")
    {

        SorobanResources extendResources;
        extendResources.footprint.readOnly = dataKeys;
        extendResources.diskReadBytes = 9'000;
        SECTION("too large extension")
        {
            auto tx = test.createExtendOpTx(extendResources,
                                            stateArchivalSettings.maxEntryTTL,
                                            1'000, DEFAULT_TEST_RESOURCE_FEE);
            REQUIRE(!test.isTxValid(tx));
        }

        SECTION("readBytes limit")
        {
            // With p23, eead byte limits no longer apply to extension, since
            // the footprint is always live soroban state
            if (protocolVersionStartsFrom(
                    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION,
                    AUTO_RESTORE_PROTOCOL_VERSION))
            {
                auto resourceCopy = extendResources;
                resourceCopy.diskReadBytes = 0;

                auto tx = test.createExtendOpTx(resourceCopy, 10'000, 1'000,
                                                DEFAULT_TEST_RESOURCE_FEE);
                auto result = test.invokeTx(tx);
                REQUIRE(isSuccessResult(result));
            }
            else
            {
                auto resourceCopy = extendResources;
                resourceCopy.diskReadBytes = 8'000;

                auto tx = test.createExtendOpTx(resourceCopy, 10'000, 1'000,
                                                DEFAULT_TEST_RESOURCE_FEE);
                auto result = test.invokeTx(tx);
                REQUIRE(!isSuccessResult(result));
                REQUIRE(result.result.results()[0]
                            .tr()
                            .extendFootprintTTLResult()
                            .code() ==
                        EXTEND_FOOTPRINT_TTL_RESOURCE_LIMIT_EXCEEDED);
            }
        }

        SECTION("insufficient refundable fee")
        {
            auto tx =
                test.createExtendOpTx(extendResources, 10'000, 30'000, 30'000);
            auto result = test.invokeTx(tx);
            REQUIRE(!isSuccessResult(result));
            REQUIRE(result.result.results()[0]
                        .tr()
                        .extendFootprintTTLResult()
                        .code() ==
                    EXTEND_FOOTPRINT_TTL_INSUFFICIENT_REFUNDABLE_FEE);
        }
        SECTION("success")
        {
            auto tx = test.createExtendOpTx(extendResources, 10'000, 1'000,
                                            DEFAULT_TEST_RESOURCE_FEE);
            REQUIRE(isSuccessResult(test.invokeTx(tx)));
        }
    }
}

TEST_CASE("persistent entry archival", "[tx][soroban][archival]")
{
    auto test = [](bool evict) {
        auto cfg = getTestConfig();
        SorobanTest test(cfg, true, [evict](SorobanNetworkConfig& cfg) {
            cfg.stateArchivalSettings().startingEvictionScanLevel =
                evict ? 1 : 5;
            cfg.stateArchivalSettings().minPersistentTTL =
                MinimumSorobanNetworkConfig::MINIMUM_PERSISTENT_ENTRY_LIFETIME;
        });

        ContractStorageTestClient client(test);

        // WASM and instance should not expire
        test.invokeExtendOp(client.getContract().getKeys(), 10'000);

        auto writeInvocation = client.getContract().prepareInvocation(
            "put_persistent", {makeSymbolSCVal("key"), makeU64SCVal(123)},
            client.writeKeySpec("key", ContractDataDurability::PERSISTENT));
        REQUIRE(writeInvocation.withExactNonRefundableResourceFee().invoke());

        auto evictionLedger =
            test.getLCLSeq() +
            MinimumSorobanNetworkConfig::MINIMUM_PERSISTENT_ENTRY_LIFETIME;

        // Close ledgers until entry is evicted
        for (uint32_t ledgerSeq = test.getLCLSeq() + 1;
             ledgerSeq <= evictionLedger; ++ledgerSeq)
        {
            closeLedgerOn(test.getApp(), ledgerSeq, 2, 1, 2016);
        }

        auto lk = client.getContract().getDataKey(
            makeSymbolSCVal("key"), ContractDataDurability::PERSISTENT);

        auto hotArchive = test.getApp()
                              .getBucketManager()
                              .getBucketSnapshotManager()
                              .copySearchableHotArchiveBucketListSnapshot();

        if (evict)
        {
            REQUIRE(hotArchive->load(lk));
            REQUIRE(!hotArchive->load(getTTLKey(lk)));
            {
                LedgerTxn ltx(test.getApp().getLedgerTxnRoot());
                REQUIRE(!ltx.load(lk));
                REQUIRE(!ltx.load(getTTLKey(lk)));
            }
        }
        else
        {
            REQUIRE(!hotArchive->load(lk));
            REQUIRE(!hotArchive->load(getTTLKey(lk)));
            {
                LedgerTxn ltx(test.getApp().getLedgerTxnRoot());
                REQUIRE(ltx.load(lk));
                REQUIRE(ltx.load(getTTLKey(lk)));
            }
        }

        // Rewriting entry should fail since key is archived
        REQUIRE(!writeInvocation.withExactNonRefundableResourceFee().invoke());

        // Reads should also fail
        REQUIRE(client.has("key", ContractDataDurability::PERSISTENT,
                           std::nullopt) ==
                INVOKE_HOST_FUNCTION_ENTRY_ARCHIVED);

        // Rent extension is a no op
        SorobanResources resources;
        resources.footprint.readOnly = {lk};

        resources.instructions = 0;
        resources.diskReadBytes = 1000;
        resources.writeBytes = 1000;
        auto resourceFee = 1'000'000;

        auto extendTx =
            test.createExtendOpTx(resources, 100, 1'000, resourceFee);
        auto result = test.invokeTx(extendTx);
        REQUIRE(isSuccessResult(result));

        REQUIRE(client.has("key", ContractDataDurability::PERSISTENT,
                           std::nullopt) ==
                INVOKE_HOST_FUNCTION_ENTRY_ARCHIVED);

        // Restore should succeed. Fee should be the same for both evicted and
        // nonevicted case
        SECTION("restore with nonexistent key")
        {
            // Get random CONTRACT_CODE key that doesn't exist in ledger
            UnorderedSet<LedgerKey> excludedKeys{
                client.getContract().getKeys().begin(),
                client.getContract().getKeys().end()};
            auto randomKey =
                LedgerTestUtils::generateValidUniqueLedgerKeysWithTypes(
                    {CONTRACT_CODE}, 1, excludedKeys)
                    .at(0);

            // Restore should skip nonexistent key and charge same fees
            test.invokeRestoreOp({lk, randomKey}, 20'166);
        }

        SECTION("key accessible after restore")
        {
            auto check = [&]() {
                auto const& stateArchivalSettings =
                    test.getNetworkCfg().stateArchivalSettings();
                auto newExpectedLiveUntilLedger =
                    test.getLCLSeq() + stateArchivalSettings.minPersistentTTL -
                    1;
                REQUIRE(test.getTTL(lk) == newExpectedLiveUntilLedger);

                client.get("key", ContractDataDurability::PERSISTENT, 123);

                test.getApp()
                    .getBucketManager()
                    .getBucketSnapshotManager()
                    .maybeCopySearchableHotArchiveBucketListSnapshot(
                        hotArchive);

                // Restored entries are deleted from Hot Archive
                REQUIRE(!hotArchive->load(lk));
            };

            SECTION("restore op")
            {
                test.invokeRestoreOp({lk}, 20'166);
                check();
            }

            SECTION("autorestore")
            {
                // Mark single key in read-write footprint as archived
                auto spec =
                    client
                        .writeKeySpec("key", ContractDataDurability::PERSISTENT)
                        .setArchivedIndexes({0});
                client.put("key", ContractDataDurability::PERSISTENT, 123,
                           spec);
                check();
            }
        }

        SECTION("key accessible after restore in same ledger")
        {
            auto extendSrcAccount = test.getRoot().create("src", 500000000);

            SorobanResources restoreResources;
            restoreResources.footprint.readOnly = {lk};
            restoreResources.instructions = 0;
            restoreResources.diskReadBytes = 10'000;

            auto bumpLedgers = 10'000;
            auto resourceFee = 300'000 + 40'000;
            auto extendTx =
                test.createExtendOpTx(restoreResources, bumpLedgers, 1'000,
                                      resourceFee, &extendSrcAccount);

            SECTION("manual restore")
            {
                SorobanResources restoreResources;
                restoreResources.footprint.readWrite = {lk};
                restoreResources.instructions = 0;
                restoreResources.diskReadBytes = 10'000;
                restoreResources.writeBytes = 10'000;

                auto resourceFee = 300'000 + 40'000;
                auto restoreTx =
                    test.createRestoreTx(restoreResources, 1'000, resourceFee);

                closeLedger(test.getApp(), {restoreTx, extendTx},
                            /*strictOrder=*/true);

                // Require that the restore went through and the entry could be
                // bumped in the subsequent TX
                REQUIRE(test.getTTL(lk) == bumpLedgers + test.getLCLSeq());
            }

            SECTION("autorestore")
            {
                auto writeInvocation = client.getContract().prepareInvocation(
                    "put_persistent",
                    {makeSymbolSCVal("key"), makeU64SCVal(200)},
                    client
                        .writeKeySpec("key", ContractDataDurability::PERSISTENT)
                        .setArchivedIndexes({0}));

                auto writeTx =
                    writeInvocation.withExactNonRefundableResourceFee()
                        .createTx();

                closeLedger(test.getApp(), {writeTx, extendTx},
                            /*strictOrder=*/true);

                // Require that the write TX restored the entry which could be
                // bumped in the subsequent TX
                REQUIRE(test.getTTL(lk) == bumpLedgers + test.getLCLSeq());
            }
        }
    };

    SECTION("eviction")
    {
        test(true);
    }

    SECTION("expiration without eviction")
    {
        test(false);
    }
}

TEST_CASE("autorestore contract instance", "[tx][soroban][archival]")
{
    auto cfg = getTestConfig();
    SorobanTest test(cfg, true, [](SorobanNetworkConfig& cfg) {
        cfg.stateArchivalSettings().minPersistentTTL =
            MinimumSorobanNetworkConfig::MINIMUM_PERSISTENT_ENTRY_LIFETIME;

        // Never snapshot bucket list so we have stable rent fees
        cfg.mStateArchivalSettings.liveSorobanStateSizeWindowSamplePeriod =
            10'000;
    });

    ContractStorageTestClient client(test);

    client.put("key", ContractDataDurability::PERSISTENT, 123);

    auto expirationLedger =
        test.getLCLSeq() +
        MinimumSorobanNetworkConfig::MINIMUM_PERSISTENT_ENTRY_LIFETIME;

    // Close ledgers until ContractData entry, contract code, and instance are
    // all expired
    for (uint32_t ledgerSeq = test.getLCLSeq() + 1;
         ledgerSeq <= expirationLedger; ++ledgerSeq)
    {
        closeLedgerOn(test.getApp(), ledgerSeq, 2, 1, 2016);
    }

    auto lk = client.getContract().getDataKey(
        makeSymbolSCVal("key"), ContractDataDurability::PERSISTENT);

    // We need to restore instance, wasm, and data entry
    auto const refundableRestoreCost = 60'498;
    auto keysToRestore = client.getContract().getKeys();
    keysToRestore.push_back(lk);
    REQUIRE(client.get("key", ContractDataDurability::PERSISTENT,
                       std::nullopt) == INVOKE_HOST_FUNCTION_ENTRY_ARCHIVED);

    SECTION("manual restore")
    {
        test.invokeRestoreOp(keysToRestore, refundableRestoreCost);

        // Contract and entry should be restored
        client.get("key", ContractDataDurability::PERSISTENT, 123);
    }

    SECTION("autorestore fees")
    {
        // Now submit an invocation with all keys in the write footprint so
        // everything is restored
        auto spec = client.defaultSpecWithoutFootprint()
                        .setReadWriteFootprint(keysToRestore)
                        .setArchivedIndexes({0, 1, 2})
                        .setWriteBytes(10000)
                        .setRefundableResourceFee(70'000);

        auto invocation = client.getContract().prepareInvocation(
            "put_persistent", {makeSymbolSCVal("key"), makeU64SCVal(123)}, spec,
            /*addContractKeys=*/false);
        auto autorestoreTx =
            invocation.withExactNonRefundableResourceFee().createTx();

        auto initialBalance = test.getAccountBalance();
        TransactionMetaFrame txm;
        REQUIRE(test.invokeTx(autorestoreTx, &txm).result.code() ==
                TransactionResultCode::txSUCCESS);

        // Check that the refundable fee charged matches what we expect for
        // restoration
        auto const eventSize = 4;
        test.checkRefundableFee(initialBalance, autorestoreTx, txm,
                                refundableRestoreCost, eventSize);

        // Entry should be live again
        client.get("key", ContractDataDurability::PERSISTENT, 123);
    }

    SECTION("missing archived key index")
    {
        // Omit index 1
        auto spec = client.defaultSpecWithoutFootprint()
                        .setReadWriteFootprint(keysToRestore)
                        .setArchivedIndexes({0, 2})
                        .setWriteBytes(10000)
                        .setRefundableResourceFee(70'000);

        auto invocation = client.getContract().prepareInvocation(
            "put_persistent", {makeSymbolSCVal("key"), makeU64SCVal(123)}, spec,
            /*addContractKeys=*/false);
        REQUIRE(!invocation.withExactNonRefundableResourceFee().invoke());
    }

    SECTION("restore with write")
    {
        auto spec = client.defaultSpecWithoutFootprint()
                        .setReadWriteFootprint(keysToRestore)
                        .setArchivedIndexes({0, 1, 2})
                        .setWriteBytes(10000)
                        .setRefundableResourceFee(70'000);

        // Write new value to key
        auto invocation = client.getContract().prepareInvocation(
            "put_persistent", {makeSymbolSCVal("key"), makeU64SCVal(999)}, spec,
            /*addContractKeys=*/false);
        REQUIRE(invocation.withExactNonRefundableResourceFee().invoke());
        client.get("key", ContractDataDurability::PERSISTENT, 999);
    }

    // Check that entries are properly restored and persisted even if we don't
    // write anything
    SECTION("restore with no writes")
    {
        auto spec = client.defaultSpecWithoutFootprint()
                        .setReadWriteFootprint(keysToRestore)
                        .setArchivedIndexes({0, 1, 2})
                        .setWriteBytes(10000)
                        .setRefundableResourceFee(70'000);

        SECTION("insufficient read bytes")
        {
            auto badSpec = spec.setReadBytes(0);
            auto invocation = client.getContract().prepareInvocation(
                "has_persistent", {makeSymbolSCVal("key")}, badSpec,
                /*addContractKeys=*/false);
            REQUIRE(!invocation.withExactNonRefundableResourceFee().invoke());
        }

        SECTION("classic entries count towards read bytes")
        {
            auto contractKeys = client.getContract().getKeys();
            auto specWithoutClassic = client.defaultSpecWithoutFootprint()
                                          .setReadWriteFootprint({lk})
                                          .setArchivedIndexes({0})
                                          .setReadOnlyFootprint(contractKeys)
                                          .setRefundableResourceFee(70'000);

            auto const expectedSize = 80;

            // Restore wasm and instance so we just restore data later
            test.invokeRestoreOp(contractKeys, 40'333);

            SECTION("insufficient read bytes")
            {
                auto invocation = client.getContract().prepareInvocation(
                    "has_persistent", {makeSymbolSCVal("key")},
                    specWithoutClassic.setWriteBytes(expectedSize)
                        .setReadBytes(expectedSize - 1),
                    /*addContractKeys=*/false);
                REQUIRE(
                    !invocation.withExactNonRefundableResourceFee().invoke());
            }

            SECTION("insufficient write bytes")
            {
                auto invocation = client.getContract().prepareInvocation(
                    "has_persistent", {makeSymbolSCVal("key")},
                    specWithoutClassic.setWriteBytes(expectedSize - 1)
                        .setReadBytes(expectedSize),
                    /*addContractKeys=*/false);
                REQUIRE(
                    !invocation.withExactNonRefundableResourceFee().invoke());
            }

            SECTION("classic keys count towards read bytes")
            {
                auto readOnly = contractKeys;
                readOnly.push_back(accountKey(test.getRoot().getPublicKey()));
                auto specWithClassic =
                    specWithoutClassic.setReadOnlyFootprint(readOnly)
                        .setWriteBytes(expectedSize)
                        .setReadBytes(expectedSize);
                auto invocation = client.getContract().prepareInvocation(
                    "has_persistent", {makeSymbolSCVal("key")}, specWithClassic,
                    /*addContractKeys=*/false);
                REQUIRE(
                    !invocation.withExactNonRefundableResourceFee().invoke());
            }

            SECTION("Success")
            {
                auto invocation = client.getContract().prepareInvocation(
                    "has_persistent", {makeSymbolSCVal("key")},
                    specWithoutClassic.setWriteBytes(expectedSize)
                        .setReadBytes(expectedSize),
                    /*addContractKeys=*/false);
                REQUIRE(
                    invocation.withExactNonRefundableResourceFee().invoke());
            }
        }

        // Invocation writes no new state
        auto invocation = client.getContract().prepareInvocation(
            "has_persistent", {makeSymbolSCVal("key")}, spec,
            /*addContractKeys=*/false);
        REQUIRE(invocation.withExactNonRefundableResourceFee().invoke());

        // Check that entry still exists and is live
        client.get("key", ContractDataDurability::PERSISTENT, 123);
    }
}

TEST_CASE("autorestore with storage resize", "[tx][soroban][archival]")
{
    auto cfg = getTestConfig();
    SorobanTest test(cfg, true, [](SorobanNetworkConfig& cfg) {
        cfg.stateArchivalSettings().minPersistentTTL =
            MinimumSorobanNetworkConfig::MINIMUM_PERSISTENT_ENTRY_LIFETIME;

        cfg.mRentFee1KBSorobanStateSizeLow = 20'000;
        cfg.mRentFee1KBSorobanStateSizeHigh = 1'000'000;

        // Never snapshot bucket list so we have stable rent fees
        cfg.mStateArchivalSettings.liveSorobanStateSizeWindowSamplePeriod =
            10'000;
    });

    auto isSuccess = [](auto resultCode) {
        return resultCode == INVOKE_HOST_FUNCTION_SUCCESS;
    };

    ContractStorageTestClient client(test);

    // Contract should not expire
    test.invokeExtendOp(client.getContract().getKeys(), 100'000);

    auto initSpec =
        client.writeKeySpec("key", ContractDataDurability::PERSISTENT)
            .setWriteBytes(10'000);
    REQUIRE(isSuccess(client.resizeStorageAndExtend("key", 1, 0, 0, initSpec)));
    auto lk = client.getContract().getDataKey(
        makeSymbolSCVal("key"), ContractDataDurability::PERSISTENT);

    auto const bumpLedgers = 1'000'000;
    auto const resizeKb = 3;

    auto expirationLedger =
        test.getLCLSeq() +
        MinimumSorobanNetworkConfig::MINIMUM_PERSISTENT_ENTRY_LIFETIME;

    // Close ledgers until ContractData entry, contract code, and instance
    // are all expired
    for (uint32_t ledgerSeq = test.getLCLSeq() + 1;
         ledgerSeq <= expirationLedger; ++ledgerSeq)
    {
        closeLedgerOn(test.getApp(), ledgerSeq, 2, 1, 2016);
    }

    REQUIRE(!test.isEntryLive(lk, test.getLCLSeq()));

    auto const costToRestore1KB = 20'166;
    auto const costToBump = 105'259;
    auto const costToBumpAfterResize = 263'688;

    auto getExpectedRestoredLedger = [&]() {
        return test.getLCLSeq() +
               MinimumSorobanNetworkConfig::MINIMUM_PERSISTENT_ENTRY_LIFETIME -
               1;
    };

    // First, test without autorestore to establish baseline expected fees
    SECTION("manual restore")
    {
        test.invokeRestoreOp({lk}, costToRestore1KB);
        REQUIRE(test.isEntryLive(lk, test.getLCLSeq()));
        REQUIRE(test.getTTL(lk) == getExpectedRestoredLedger());

        SECTION("extend only")
        {
            // Offset by MINIMUM_PERSISTENT_ENTRY_LIFETIME since the restoreOp
            // above already partially bumped rent
            test.invokeExtendOp({lk},
                                bumpLedgers +
                                    MinimumSorobanNetworkConfig::
                                        MINIMUM_PERSISTENT_ENTRY_LIFETIME,
                                costToBump);
        }

        SECTION("extend after resize")
        {
            REQUIRE(isSuccess(
                client.resizeStorageAndExtend("key", resizeKb, 0, 0)));

            // Offset by MINIMUM_PERSISTENT_ENTRY_LIFETIME since the restoreOp
            // above already partially bumped rent
            test.invokeExtendOp({lk},
                                bumpLedgers +
                                    MinimumSorobanNetworkConfig::
                                        MINIMUM_PERSISTENT_ENTRY_LIFETIME,
                                costToBumpAfterResize);
        }
    }

    // Now submit an invocation with all keys in the write footprint so
    // everything is restored
    auto autorestoreSpec = client.defaultSpecWithoutFootprint()
                               .setReadWriteFootprint({lk})
                               .setArchivedIndexes({0})
                               .setReadBytes(10000)
                               .setWriteBytes(10000)
                               .setRefundableResourceFee(1'000'000);

    auto invokeAndCheck = [&](auto invocation,
                              auto expectedRefundableFeeCharged, auto eventSize,
                              auto expectedLiveUntilLedger) {
        auto autorestoreTx =
            invocation.withExactNonRefundableResourceFee().createTx();

        auto initialBalance = test.getAccountBalance();
        TransactionMetaFrame txm;
        REQUIRE(test.invokeTx(autorestoreTx, &txm).result.code() ==
                TransactionResultCode::txSUCCESS);

        // Check that the refundable fee charged matches what we expect
        // for restoration
        test.checkRefundableFee(initialBalance, autorestoreTx, txm,
                                expectedRefundableFeeCharged, eventSize);
        REQUIRE(test.isEntryLive(lk, test.getLCLSeq()));

        // We close a ledger during the invocation so offset by one
        REQUIRE(test.getTTL(lk) == expectedLiveUntilLedger + 1);
    };

    SECTION("autorestore")
    {
        // Write 1 KB at the same key with no rent extension, triggering
        // autorestore. Should be equivalent to RestoreOp from a rent fee
        // perspective
        auto invocation = client.getContract().prepareInvocation(
            "replace_with_bytes_and_extend",
            {makeSymbolSCVal("key"), makeU32(1), makeU32(0), makeU32(0)},
            autorestoreSpec);
        invokeAndCheck(invocation, costToRestore1KB, 4,
                       getExpectedRestoredLedger());
    }

    SECTION("autorestore with rent bump")
    {
        // autorestore and extend in the same tx. Rent fees should be
        // identical to a an extension of bumpLedgers
        auto invocation = client.getContract().prepareInvocation(
            "replace_with_bytes_and_extend",
            {makeSymbolSCVal("key"), makeU32(1), makeU32(bumpLedgers),
             makeU32(bumpLedgers)},
            autorestoreSpec);

        invokeAndCheck(invocation, costToBump, 4,
                       bumpLedgers + test.getLCLSeq());
    }

    SECTION("autorestore with rent bump and resize")
    {
        // autorestore and extend in the same tx
        auto invocation = client.getContract().prepareInvocation(
            "replace_with_bytes_and_extend",
            {makeSymbolSCVal("key"), makeU32(resizeKb), makeU32(bumpLedgers),
             makeU32(bumpLedgers)},
            autorestoreSpec);

        invokeAndCheck(invocation, costToBumpAfterResize, 4,
                       bumpLedgers + test.getLCLSeq());
    }
}

/*
 This test uses the same utils (SettingsUpgradeUtils.h) as the
 get-settings-upgrade-txs command to make sure the transactions have the
 proper resources set.
*/
TEST_CASE("settings upgrade command line utils", "[tx][soroban][upgrades]")
{
    VirtualClock clock;
    auto cfg = getTestConfig(0, Config::TESTDB_IN_MEMORY);
    cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS = true;
    auto app = createTestApplication(clock, cfg);
    auto root = app->getRoot();
    auto& lm = app->getLedgerManager();

    // Update the snapshot period and close a ledger to update
    // mAverageBucketListSize
    modifySorobanNetworkConfig(*app, [](SorobanNetworkConfig& cfg) {
        cfg.mStateArchivalSettings.liveSorobanStateSizeWindowSamplePeriod = 1;
        // These are required to allow for an upgrade of all settings at once.
        cfg.mMaxContractDataEntrySizeBytes = 3200;
    });

    const int64_t startingBalance =
        app->getLedgerManager().getLastMinBalance(50);

    auto a1 = root->create("A", startingBalance);

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

    xdr::xvector<ConfigSettingEntry> initialEntries;
    xdr::xvector<ConfigSettingEntry> updatedEntries;
    for (auto t : xdr::xdr_traits<ConfigSettingID>::enum_values())
    {
        auto type = static_cast<ConfigSettingID>(t);
        if (SorobanNetworkConfig::isNonUpgradeableConfigSettingEntry(type))
        {
            continue;
        }

        LedgerTxn ltx(app->getLedgerTxnRoot());
        auto entry = ltx.load(configSettingKey(type));

        // Store the initial entries before we modify the cost types below
        initialEntries.emplace_back(entry.current().data.configSetting());

        // We're writing the latest cpu and mem settings below to makes sure
        // they don't brick the settings upgrade process. We would ideally pull
        // this from pubnet_phase1.json, but core doesn't know how to parse JSON
        // XDR.
        auto const& vals = xdr::xdr_traits<ContractCostType>::enum_values();
        if (entry.current().data.configSetting().configSettingID() ==
            CONFIG_SETTING_CONTRACT_COST_PARAMS_CPU_INSTRUCTIONS)
        {
            auto& params = entry.current()
                               .data.configSetting()
                               .contractCostParamsCpuInsns();
            for (auto val : vals)
            {
                switch (val)
                {
                case WasmInsnExec:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 4, 0};
                    break;
                case MemAlloc:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 434, 16};
                    break;
                case MemCpy:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 42, 16};
                    break;
                case MemCmp:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 44, 16};
                    break;
                case DispatchHostFunction:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 295, 0};
                    break;
                case VisitObject:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 60, 0};
                    break;
                case ValSer:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 221, 26};
                    break;
                case ValDeser:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 331, 4369};
                    break;
                case ComputeSha256Hash:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 3636, 7013};
                    break;
                case ComputeEd25519PubKey:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 40256, 0};
                    break;
                case VerifyEd25519Sig:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 377551, 4059};
                    break;
                case VmInstantiation:
                    params[val] = ContractCostParamEntry{ExtensionPoint{0},
                                                         417482, 45712};
                    break;
                case VmCachedInstantiation:
                    params[val] = ContractCostParamEntry{ExtensionPoint{0},
                                                         417482, 45712};
                    break;
                case InvokeVmFunction:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 1945, 0};
                    break;
                case ComputeKeccak256Hash:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 6481, 5943};
                    break;
                case DecodeEcdsaCurve256Sig:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 711, 0};
                    break;
                case RecoverEcdsaSecp256k1Key:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 2314804, 0};
                    break;
                case Int256AddSub:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 4176, 0};
                    break;
                case Int256Mul:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 4716, 0};
                    break;
                case Int256Div:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 4680, 0};
                    break;
                case Int256Pow:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 4256, 0};
                    break;
                case Int256Shift:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 884, 0};
                    break;
                case ChaCha20DrawBytes:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 1059, 502};
                    break;
                }
            }
        }
        if (entry.current().data.configSetting().configSettingID() ==
            CONFIG_SETTING_CONTRACT_COST_PARAMS_MEMORY_BYTES)
        {
            auto& params = entry.current()
                               .data.configSetting()
                               .contractCostParamsMemBytes();
            for (auto val : vals)
            {
                switch (val)
                {
                case WasmInsnExec:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
                    break;
                case MemAlloc:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 16, 128};
                    break;
                case MemCpy:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
                    break;
                case MemCmp:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
                    break;
                case DispatchHostFunction:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
                    break;
                case VisitObject:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
                    break;
                case ValSer:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 242, 384};
                    break;
                case ValDeser:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 0, 384};
                    break;
                case ComputeSha256Hash:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
                    break;
                case ComputeEd25519PubKey:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
                    break;
                case VerifyEd25519Sig:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
                    break;
                case VmInstantiation:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 132773, 4903};
                    break;
                case VmCachedInstantiation:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 132773, 4903};
                    break;
                case InvokeVmFunction:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 14, 0};
                    break;
                case ComputeKeccak256Hash:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
                    break;
                case DecodeEcdsaCurve256Sig:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
                    break;
                case RecoverEcdsaSecp256k1Key:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 181, 0};
                    break;
                case Int256AddSub:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 99, 0};
                    break;
                case Int256Mul:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 99, 0};
                    break;
                case Int256Div:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 99, 0};
                    break;
                case Int256Pow:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 99, 0};
                    break;
                case Int256Shift:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 99, 0};
                    break;
                case ChaCha20DrawBytes:
                    params[val] =
                        ContractCostParamEntry{ExtensionPoint{0}, 0, 0};
                    break;
                }
            }
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

    closeLedger(*app);

    // Update sorobanStateTargetSizeBytes so sorobanStateRentFeeGrowthFactor
    // comes into play
    auto const& blSize = app->getLedgerManager()
                             .getLastClosedSorobanNetworkConfig()
                             .getAverageBucketListSize();
    {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        auto costKey = configSettingKey(
            ConfigSettingID::CONFIG_SETTING_CONTRACT_LEDGER_COST_V0);

        auto costEntry = ltx.load(costKey);
        costEntry.current()
            .data.configSetting()
            .contractLedgerCost()
            .sorobanStateTargetSizeBytes = static_cast<int64>(blSize * .95);
        costEntry.current()
            .data.configSetting()
            .contractLedgerCost()
            .sorobanStateRentFeeGrowthFactor = 1000;
        ltx.commit();
        closeLedger(*app);
    }

    for (auto& txEnv : txsToSign)
    {
        txEnv.v1().signatures.emplace_back(SignatureUtils::sign(
            a1.getSecretKey(),
            sha256(xdr::xdr_to_opaque(app->getNetworkID(), ENVELOPE_TYPE_TX,
                                      txEnv.v1().tx))));

        auto const& rawTx = TransactionFrameBase::makeTransactionFromWire(
            app->getNetworkID(), txEnv);
        auto tx = TransactionTestFrame::fromTxFrame(rawTx);
        LedgerTxn ltx(app->getLedgerTxnRoot());
        TransactionMetaBuilder txm(true, *tx,
                                   ltx.loadHeader().current().ledgerVersion,
                                   app->getAppConnector());
        REQUIRE(tx->checkValidForTesting(app->getAppConnector(), ltx, 0, 0, 0));
        REQUIRE(tx->apply(app->getAppConnector(), ltx, txm));
        ltx.commit();
    }

    auto& commandHandler = app->getCommandHandler();

    std::string command = "mode=set&configupgradesetkey=";
    command += decoder::encode_b64(xdr::xdr_to_opaque(upgradeSetKey));
    command += "&upgradetime=2000-07-21T22:04:00Z";

    std::string ret;
    commandHandler.upgrades(command, ret);
    REQUIRE(ret == "");

    auto checkSettings = [&](xdr::xvector<ConfigSettingEntry> const& entries) {
        auto expectedIndex = 0;
        for (auto t : xdr::xdr_traits<ConfigSettingID>::enum_values())
        {
            auto type = static_cast<ConfigSettingID>(t);
            if (SorobanNetworkConfig::isNonUpgradeableConfigSettingEntry(type))
            {
                continue;
            }

            LedgerTxn ltx(app->getLedgerTxnRoot());
            auto entry = ltx.load(configSettingKey(type));

            REQUIRE(entry.current().data.configSetting() ==
                    entries.at(expectedIndex));
            ++expectedIndex;
        }
    };

    SECTION("success")
    {
        {
            // trigger upgrade
            auto ledgerUpgrade = LedgerUpgrade{LEDGER_UPGRADE_CONFIG};
            ledgerUpgrade.newConfig() = upgradeSetKey;

            auto const& lcl = lm.getLastClosedLedgerHeader();
            auto txSet = TxSetXDRFrame::makeEmpty(lcl);
            auto lastCloseTime = lcl.header.scpValue.closeTime;

            app->getHerder().externalizeValue(
                txSet, lcl.header.ledgerSeq + 1, lastCloseTime,
                {LedgerTestUtils::toUpgradeType(ledgerUpgrade)});

            checkSettings(updatedEntries);
        }

        // now revert to make sure the new settings didn't lock us in.
        ConfigUpgradeSet upgradeSet2;
        upgradeSet2.updatedEntry = initialEntries;

        auto invokeRes2 =
            getInvokeTx(a1.getPublicKey(), contractCodeLedgerKey,
                        contractSourceRefLedgerKey, contractID, upgradeSet2,
                        a1.getLastSequenceNumber() + 4);

        auto const& upgradeSetKey2 = invokeRes2.second;

        invokeRes2.first.v1().signatures.emplace_back(SignatureUtils::sign(
            a1.getSecretKey(),
            sha256(xdr::xdr_to_opaque(app->getNetworkID(), ENVELOPE_TYPE_TX,
                                      invokeRes2.first.v1().tx))));

        auto const& txRaw = TransactionFrameBase::makeTransactionFromWire(
            app->getNetworkID(), invokeRes2.first);
        auto txRevertSettings = TransactionTestFrame::fromTxFrame(txRaw);
        LedgerTxn ltx(app->getLedgerTxnRoot());
        TransactionMetaBuilder txm(true, *txRevertSettings,
                                   ltx.loadHeader().current().ledgerVersion,
                                   app->getAppConnector());
        REQUIRE(txRevertSettings->checkValidForTesting(app->getAppConnector(),
                                                       ltx, 0, 0, 0));
        REQUIRE(txRevertSettings->apply(app->getAppConnector(), ltx, txm));
        ltx.commit();

        std::string command2 = "mode=set&configupgradesetkey=";
        command2 += decoder::encode_b64(xdr::xdr_to_opaque(upgradeSetKey2));
        command2 += "&upgradetime=2000-07-21T22:04:00Z";

        std::string ret2;
        commandHandler.upgrades(command2, ret2);
        REQUIRE(ret2 == "");

        auto ledgerUpgrade = LedgerUpgrade{LEDGER_UPGRADE_CONFIG};
        ledgerUpgrade.newConfig() = upgradeSetKey2;

        auto const& lcl = lm.getLastClosedLedgerHeader();
        auto txSet = TxSetXDRFrame::makeEmpty(lcl);
        auto lastCloseTime = lcl.header.scpValue.closeTime;

        app->getHerder().externalizeValue(
            txSet, lcl.header.ledgerSeq + 1, lastCloseTime,
            {LedgerTestUtils::toUpgradeType(ledgerUpgrade)});

        checkSettings(initialEntries);
    }

    // The rest are failure tests, so flip back our cost overrides so we can
    // check that the initial settings haven't changed.
    {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        auto costKey = configSettingKey(
            ConfigSettingID::CONFIG_SETTING_CONTRACT_LEDGER_COST_V0);

        auto costEntry = ltx.load(costKey);
        costEntry.current()
            .data.configSetting()
            .contractLedgerCost()
            .sorobanStateTargetSizeBytes =
            InitialSorobanNetworkConfig::BUCKET_LIST_TARGET_SIZE_BYTES;
        costEntry.current()
            .data.configSetting()
            .contractLedgerCost()
            .sorobanStateRentFeeGrowthFactor =
            InitialSorobanNetworkConfig::STATE_SIZE_RENT_FEE_GROWTH_FACTOR;
        ltx.commit();
        closeLedger(*app);
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

        auto txSet = TxSetXDRFrame::makeEmpty(lcl);
        auto lastCloseTime = lcl.header.scpValue.closeTime;

        app->getHerder().externalizeValue(
            txSet, lcl.header.ledgerSeq + 1, lastCloseTime,
            {LedgerTestUtils::toUpgradeType(ledgerUpgrade)});

        // No upgrade due to expired entry
        checkSettings(initialEntries);
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

        auto txSet = TxSetXDRFrame::makeEmpty(lcl);
        auto lastCloseTime = lcl.header.scpValue.closeTime;

        app->getHerder().externalizeValue(
            txSet, lcl.header.ledgerSeq + 1, lastCloseTime,
            {LedgerTestUtils::toUpgradeType(ledgerUpgrade)});

        // No upgrade due to tampered entry
        checkSettings(initialEntries);
    };

    SECTION("Invalid XDR")
    {
        SCVal b(SCV_BYTES);
        updateBytes(b);
    }

    SECTION("Valid XDR but hash mismatch")
    {
        ConfigSettingEntry costSetting(CONFIG_SETTING_CONTRACT_LEDGER_COST_V0);
        costSetting.contractLedgerCost().feeDiskRead1KB = 1234;

        ConfigUpgradeSet upgradeSet2;
        upgradeSet2.updatedEntry.emplace_back(costSetting);

        auto upgradeSetBytes(xdr::xdr_to_opaque(upgradeSet2));
        SCVal b(SCV_BYTES);
        b.bytes() = upgradeSetBytes;
        updateBytes(b);
    }

    SECTION("Invalid ledger cost parameters")
    {
        auto costEntryIter =
            find_if(upgradeSet.updatedEntry.begin(),
                    upgradeSet.updatedEntry.end(), [](const auto& entry) {
                        return entry.configSettingID() ==
                               CONFIG_SETTING_CONTRACT_LEDGER_COST_V0;
                    });

        SECTION("Invalid sorobanStateRentFeeGrowthFactor")
        {
            // Value is too high due to the check in validateConfigUpgradeSet
            costEntryIter->contractLedgerCost()
                .sorobanStateRentFeeGrowthFactor = 50'001;
            REQUIRE_THROWS_AS(
                getInvokeTx(a1.getPublicKey(), contractCodeLedgerKey,
                            contractSourceRefLedgerKey, contractID, upgradeSet,
                            a1.getLastSequenceNumber() + 3),
                std::runtime_error);
        }
    }
}

TEST_CASE("overly large soroban values are handled gracefully", "[tx][soroban]")
{
    Config cfg = getTestConfig();
    cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS = true;
    SorobanTest test(cfg);
    auto& contract =
        test.deployWasmContract(rust_bridge::get_hostile_large_val_wasm());

    auto spec =
        SorobanInvocationSpec()
            .setInstructions(test.getNetworkCfg().txMaxInstructions())
            .setReadBytes(test.getNetworkCfg().txMaxDiskReadBytes())
            .setWriteBytes(test.getNetworkCfg().txMaxWriteBytes())
            // Put client instance to RW footprint in order to be able to
            // use the instance storage.
            .setReadWriteFootprint(contract.getKeys())
            .extendReadWriteFootprint({contract.getDataKey(
                makeSymbolSCVal("val"), ContractDataDurability::PERSISTENT)})
            .setRefundableResourceFee(200'000'000);

    auto invoke = [&](std::string const& fnName, uint32_t width,
                      uint32_t depth) {
        auto invocation =
            contract
                .prepareInvocation(fnName, {makeU32(width), makeU32(depth)},
                                   spec)
                .withDeduplicatedFootprint();
        invocation.invoke();
        return invocation.getResultCode();
    };

    auto runTest = [&](std::string const& fnName, bool omitSuccess = false) {
        if (!omitSuccess)
        {
            // Successful call (5^2 entries)
            REQUIRE(invoke(fnName, 30, 2) ==
                    InvokeHostFunctionResultCode::INVOKE_HOST_FUNCTION_SUCCESS);
        }
        // Non-serializable value, but not too deep -
        // should run out of budget (2^99 entries).
        REQUIRE(invoke(fnName, 2, 99) ==
                InvokeHostFunctionResultCode::
                    INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
        // A wide and not so deep value should exceed budget as well.
        REQUIRE(invoke(fnName, 10000, 50) ==
                InvokeHostFunctionResultCode::
                    INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
        // One more depth level, now we've exceeded the host depth
        // limit of 100 and should fail well before running of budget
        // due to reaching the depth limit.
        REQUIRE(invoke(fnName, 2, 100) ==
                InvokeHostFunctionResultCode::INVOKE_HOST_FUNCTION_TRAPPED);
        // Sanity-check with even bigger depth.
        REQUIRE(invoke(fnName, 2, 1000) ==
                InvokeHostFunctionResultCode::INVOKE_HOST_FUNCTION_TRAPPED);
    };
    SECTION("serialize")
    {
        runTest("serialize");
    }
    SECTION("store value")
    {
        runTest("store");
    }
    SECTION("store key")
    {
        // Omit the success case as we won't be able to build a valid key
        // for it due to exceeding the key size limit.
        runTest("store_key", true);
    }
    SECTION("store in instance storage")
    {
        runTest("store_instance");
    }
    SECTION("store in instance storage key")
    {
        runTest("store_instance_key");
    }
    SECTION("event topic")
    {
        runTest("event_topic");
    }
    SECTION("event data")
    {
        runTest("event_data");
    }
    SECTION("return value")
    {
        runTest("return_value");
    }
    SECTION("diagnostics")
    {
        // Excessive serialization for diagnostics call shouldn't
        // affect the function result.
        REQUIRE(invoke("diagnostics", 2, 50) ==
                InvokeHostFunctionResultCode::INVOKE_HOST_FUNCTION_SUCCESS);
        REQUIRE(invoke("diagnostics", 2, 1000) ==
                InvokeHostFunctionResultCode::INVOKE_HOST_FUNCTION_SUCCESS);
    }
}

TEST_CASE("Soroban classic account authentication", "[tx][soroban]")
{
    auto cfg = getTestConfig();
    cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS = true;
    SorobanTest test(cfg);
    auto defaultSpec =
        SorobanInvocationSpec()
            .setInstructions(test.getNetworkCfg().txMaxInstructions())
            .setReadBytes(test.getNetworkCfg().txMaxDiskReadBytes())
            .setWriteBytes(test.getNetworkCfg().txMaxWriteBytes());
    TestContract& authContract = test.deployWasmContract(
        rust_bridge::get_auth_wasm(), defaultSpec.getResources());
    AuthTestTreeNode singleInvocationTree(authContract.getAddress());
    auto singleInvocation = [&](SorobanSigner const& signer,
                                std::optional<SorobanCredentials>
                                    credentialsOverride = std::nullopt) {
        auto credentials = credentialsOverride.value_or(
            signer.sign(singleInvocationTree.toAuthorizedInvocation()));

        auto invocation =
            authContract
                .prepareInvocation(
                    "tree_fn",
                    {
                        makeVecSCVal({signer.getAddressVal()}),
                        singleInvocationTree.toSCVal(1),
                    },
                    defaultSpec.extendReadOnlyFootprint(signer.getLedgerKeys()))
                .withAuthorization(
                    singleInvocationTree.toAuthorizedInvocation(), credentials);
        invocation.invoke();
        return invocation.getResultCode();
    };
    auto runAccountTest = [&](TestAccount& account,
                              std::vector<TestAccount*> signers = {}) {
        if (signers.empty())
        {
            signers = {&account};
        }
        auto signer = test.createClassicAccountSigner(account, signers);
        auto baseCredentials =
            signer.sign(singleInvocationTree.toAuthorizedInvocation());
        auto& signatureVec =
            baseCredentials.address().signature.vec().activate();
        auto& fieldsMap = signatureVec[0].map().activate();
        SECTION("success")
        {
            REQUIRE(singleInvocation(signer, baseCredentials) ==
                    InvokeHostFunctionResultCode::INVOKE_HOST_FUNCTION_SUCCESS);
        }
        if (signers.size() > 1)
        {
            SECTION("wrong signature order")
            {
                std::swap(signatureVec[0], signatureVec.back());
                REQUIRE(
                    singleInvocation(signer, baseCredentials) ==
                    InvokeHostFunctionResultCode::INVOKE_HOST_FUNCTION_TRAPPED);
            }
        }
        SECTION("wrong signature type")
        {
            baseCredentials.address().signature = SCVal(SCV_VOID);
            REQUIRE(singleInvocation(signer, baseCredentials) ==
                    InvokeHostFunctionResultCode::INVOKE_HOST_FUNCTION_TRAPPED);
        }
        SECTION("uninitialized vector signature")
        {
            baseCredentials.address().signature = SCVal(SCV_VEC);
            REQUIRE(singleInvocation(signer, baseCredentials) ==
                    InvokeHostFunctionResultCode::INVOKE_HOST_FUNCTION_TRAPPED);
        }
        SECTION("empty vector signature")
        {
            SCVal signature(SCV_VEC);
            signature.vec().activate();
            baseCredentials.address().signature = signature;
            REQUIRE(singleInvocation(signer, baseCredentials) ==
                    InvokeHostFunctionResultCode::INVOKE_HOST_FUNCTION_TRAPPED);
        }
        SECTION("uninitialized map")
        {
            SCVal signature(SCV_VEC);
            signature.vec().activate().push_back(SCVal(SCV_MAP));
            baseCredentials.address().signature = signature;
            REQUIRE(singleInvocation(signer, baseCredentials) ==
                    InvokeHostFunctionResultCode::INVOKE_HOST_FUNCTION_TRAPPED);
        }
        SECTION("empty map")
        {
            SCVal signature(SCV_VEC);
            SCVal m(SCV_MAP);
            m.map().activate();
            signature.vec().activate().push_back(m);
            baseCredentials.address().signature = signature;
            REQUIRE(singleInvocation(signer, baseCredentials) ==
                    InvokeHostFunctionResultCode::INVOKE_HOST_FUNCTION_TRAPPED);
        }
        SECTION("missing signature field")
        {
            baseCredentials.address()
                .signature.vec()
                .activate()[0]
                .map()
                .activate()
                .pop_back();
            REQUIRE(singleInvocation(signer, baseCredentials) ==
                    InvokeHostFunctionResultCode::INVOKE_HOST_FUNCTION_TRAPPED);
        }

        SECTION("missing signature field")
        {
            fieldsMap.pop_back();
            REQUIRE(singleInvocation(signer, baseCredentials) ==
                    InvokeHostFunctionResultCode::INVOKE_HOST_FUNCTION_TRAPPED);
        }
        SECTION("missing key field")
        {
            fieldsMap.erase(fieldsMap.begin());
            REQUIRE(singleInvocation(signer, baseCredentials) ==
                    InvokeHostFunctionResultCode::INVOKE_HOST_FUNCTION_TRAPPED);
        }
        SECTION("wrong key name")
        {
            fieldsMap[0].key = makeSymbolSCVal("public_ke");
            REQUIRE(singleInvocation(signer, baseCredentials) ==
                    InvokeHostFunctionResultCode::INVOKE_HOST_FUNCTION_TRAPPED);
        }
        SECTION("wrong key type")
        {

            fieldsMap[0].key = makeBytesSCVal(std::string("public_key"));
            REQUIRE(singleInvocation(signer, baseCredentials) ==
                    InvokeHostFunctionResultCode::INVOKE_HOST_FUNCTION_TRAPPED);
        }
        SECTION("wrong signature name")
        {
            fieldsMap[1].key = makeSymbolSCVal("ignature");
            REQUIRE(singleInvocation(signer, baseCredentials) ==
                    InvokeHostFunctionResultCode::INVOKE_HOST_FUNCTION_TRAPPED);
        }
        SECTION("wrong key type")
        {
            fieldsMap[1].key = makeBytesSCVal(std::string("signature"));
            REQUIRE(singleInvocation(signer, baseCredentials) ==
                    InvokeHostFunctionResultCode::INVOKE_HOST_FUNCTION_TRAPPED);
        }
        SECTION("incomplete key")
        {
            fieldsMap[0].val.bytes().pop_back();
            REQUIRE(singleInvocation(signer, baseCredentials) ==
                    InvokeHostFunctionResultCode::INVOKE_HOST_FUNCTION_TRAPPED);
        }
        SECTION("incomplete signature")
        {
            fieldsMap[1].val.bytes().pop_back();
            REQUIRE(singleInvocation(signer, baseCredentials) ==
                    InvokeHostFunctionResultCode::INVOKE_HOST_FUNCTION_TRAPPED);
        }
        SECTION("wrong field order")
        {
            std::swap(fieldsMap[0], fieldsMap[1]);
            REQUIRE(singleInvocation(signer, baseCredentials) ==
                    InvokeHostFunctionResultCode::INVOKE_HOST_FUNCTION_TRAPPED);
        }
    };

    auto account = test.getRoot().create(
        "a1", test.getApp().getLedgerManager().getLastMinBalance(1) * 100);
    auto signerAccount = test.getRoot().create(
        "a2", test.getApp().getLedgerManager().getLastMinBalance(1) * 100);
    auto signerAccount2 = test.getRoot().create(
        "a3", test.getApp().getLedgerManager().getLastMinBalance(1) * 100);
    SECTION("default account")
    {
        runAccountTest(account);
    }
    SECTION("account with weights")
    {
        account.setOptions(setMasterWeight(5) | setMedThreshold(5));
        runAccountTest(account);
    }
    SECTION("default account with additional signer")
    {
        account.setOptions(
            setSigner(makeSigner(signerAccount.getSecretKey(), 1)));
        runAccountTest(account, {&signerAccount});
        runAccountTest(account, {&account});
        runAccountTest(account, {&account, &signerAccount});
    }
    SECTION("account with weights and additional signer")
    {
        account.setOptions(
            setMedThreshold(10) |
            setSigner(makeSigner(signerAccount.getSecretKey(), 10)));
        runAccountTest(account, {&signerAccount});
        runAccountTest(account, {&account});
        runAccountTest(account, {&account, &signerAccount});
    }
    SECTION("account with required multisig")
    {
        account.setOptions(
            setMedThreshold(30) | setMasterWeight(5) |
            setSigner(makeSigner(signerAccount.getSecretKey(), 15)));
        account.setOptions(
            setSigner(makeSigner(signerAccount2.getSecretKey(), 10)));
        runAccountTest(account, {&account, &signerAccount, &signerAccount2});
    }

    SECTION("duplicate signature not allowed with sufficient single signature "
            "threshold")
    {
        REQUIRE(singleInvocation(test.createClassicAccountSigner(
                    account, {&account, &account})) ==
                InvokeHostFunctionResultCode::INVOKE_HOST_FUNCTION_TRAPPED);
    }
    SECTION("duplicate signature not allowed with insufficient single "
            "signature threshold")
    {
        account.setOptions(setMedThreshold(2));
        REQUIRE(singleInvocation(test.createClassicAccountSigner(
                    account, {&account, &account})) ==
                InvokeHostFunctionResultCode::INVOKE_HOST_FUNCTION_TRAPPED);
    }
    SECTION("duplicate signature not allowed with multisig")
    {
        account.setOptions(
            setMedThreshold(4) |
            setSigner(makeSigner(signerAccount.getSecretKey(), 3)));
        REQUIRE(singleInvocation(test.createClassicAccountSigner(
                    account, {&account, &signerAccount, &signerAccount})) ==
                InvokeHostFunctionResultCode::INVOKE_HOST_FUNCTION_TRAPPED);
    }
    SECTION("duplicate signature not allowed with multisig and insufficient "
            "threshold")
    {
        account.setOptions(
            setMedThreshold(4) |
            setSigner(makeSigner(signerAccount.getSecretKey(), 2)));
        REQUIRE(singleInvocation(test.createClassicAccountSigner(
                    account, {&account, &signerAccount, &signerAccount})) ==
                InvokeHostFunctionResultCode::INVOKE_HOST_FUNCTION_TRAPPED);
    }
    SECTION("account with too high med threshold not authenticated")
    {
        account.setOptions(setMedThreshold(2));
        REQUIRE(singleInvocation(
                    test.createClassicAccountSigner(account, {&account})) ==
                InvokeHostFunctionResultCode::INVOKE_HOST_FUNCTION_TRAPPED);
    }
    SECTION("account with too low master weight not authenticated")
    {
        account.setOptions(setMasterWeight(4) | setMedThreshold(5));
        REQUIRE(singleInvocation(
                    test.createClassicAccountSigner(account, {&account})) ==
                InvokeHostFunctionResultCode::INVOKE_HOST_FUNCTION_TRAPPED);
    }
    SECTION("additional signer with insufficient weight not authenticated")
    {
        account.setOptions(
            setMedThreshold(2) |
            setSigner(makeSigner(signerAccount.getSecretKey(), 1)));
        REQUIRE(singleInvocation(test.createClassicAccountSigner(
                    account, {&signerAccount})) ==
                InvokeHostFunctionResultCode::INVOKE_HOST_FUNCTION_TRAPPED);
    }
    SECTION("multiple signers with insufficient weight not authenticated")
    {
        account.setOptions(
            setMedThreshold(5) |
            setSigner(makeSigner(signerAccount.getSecretKey(), 3)));
        REQUIRE(singleInvocation(test.createClassicAccountSigner(
                    account, {&account, &signerAccount})) ==
                InvokeHostFunctionResultCode::INVOKE_HOST_FUNCTION_TRAPPED);
    }
}

TEST_CASE("Soroban custom account authentication", "[tx][soroban]")
{
    auto cfg = getTestConfig();
    cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS = true;
    SorobanTest test(cfg);

    auto defaultSpec =
        SorobanInvocationSpec()
            .setInstructions(test.getNetworkCfg().txMaxInstructions())
            .setReadBytes(test.getNetworkCfg().txMaxDiskReadBytes())
            .setWriteBytes(test.getNetworkCfg().txMaxWriteBytes());
    TestContract& accountContract = test.deployWasmContract(
        rust_bridge::get_custom_account_wasm(), defaultSpec.getResources());
    TestContract& authContract = test.deployWasmContract(
        rust_bridge::get_auth_wasm(), defaultSpec.getResources());

    SCVal ownerKeyVal = makeVecSCVal({makeSymbolSCVal("Owner")});
    LedgerKey ownerKey = accountContract.getDataKey(
        ownerKeyVal, ContractDataDurability::PERSISTENT);
    auto accountInvocationSpec =
        defaultSpec.extendReadWriteFootprint({ownerKey});

    AuthTestTreeNode singleInvocationTree(authContract.getAddress());

    auto singleInvocation =
        [&](SorobanSigner const& signer,
            std::optional<SorobanCredentials> credentialsOverride =
                std::nullopt) {
            auto credentials = credentialsOverride.value_or(
                signer.sign(singleInvocationTree.toAuthorizedInvocation()));

            auto invocation =
                authContract
                    .prepareInvocation(
                        "tree_fn",
                        {
                            makeVecSCVal({signer.getAddressVal()}),
                            singleInvocationTree.toSCVal(1),
                        },
                        accountInvocationSpec.extendReadOnlyFootprint(
                            signer.getLedgerKeys()))
                    .withAuthorization(
                        singleInvocationTree.toAuthorizedInvocation(),
                        credentials);
            invocation.invoke();
            return invocation.getResultCode();
        };

    auto accountSecretKey = SecretKey::pseudoRandomForTesting();
    REQUIRE(accountContract
                .prepareInvocation(
                    "init",
                    {makeBytesSCVal(accountSecretKey.getPublicKey().ed25519())},
                    accountInvocationSpec)
                .invoke());
    auto signWithKey = [&](SecretKey const& key, uint256 payload) {
        return makeBytesSCVal(key.sign(payload));
    };

    auto signer =
        test.createContractSigner(accountContract, [&](uint256 payload) {
            return signWithKey(accountSecretKey, payload);
        });
    auto baseCredentials =
        signer.sign(singleInvocationTree.toAuthorizedInvocation());
    SECTION("successful authentication")
    {
        REQUIRE(singleInvocation(signer, baseCredentials) ==
                InvokeHostFunctionResultCode::INVOKE_HOST_FUNCTION_SUCCESS);
    }
    SECTION("void signature")
    {
        baseCredentials.address().signature = SCVal(SCV_VOID);
        REQUIRE(singleInvocation(signer, baseCredentials) ==
                InvokeHostFunctionResultCode::INVOKE_HOST_FUNCTION_TRAPPED);
    }
    SECTION("wrong signature type")
    {
        auto signatureBytes = baseCredentials.address().signature.bytes();
        baseCredentials.address().signature.type(SCV_STRING);
        baseCredentials.address().signature.str().assign(signatureBytes.begin(),
                                                         signatureBytes.end());
        REQUIRE(singleInvocation(signer, baseCredentials) ==
                InvokeHostFunctionResultCode::INVOKE_HOST_FUNCTION_TRAPPED);
    }
    SECTION("uninitialized vector signature")
    {
        baseCredentials.address().signature = SCVal(SCV_VEC);
        REQUIRE(singleInvocation(signer, baseCredentials) ==
                InvokeHostFunctionResultCode::INVOKE_HOST_FUNCTION_TRAPPED);
    }
    SECTION("empty bytes signature")
    {
        baseCredentials.address().signature.bytes().clear();
        REQUIRE(singleInvocation(signer, baseCredentials) ==
                InvokeHostFunctionResultCode::INVOKE_HOST_FUNCTION_TRAPPED);
    }
    SECTION("owner change")
    {
        auto newAccountSecretKey = SecretKey::pseudoRandomForTesting();
        auto newSigner =
            test.createContractSigner(accountContract, [&](uint256 payload) {
                return signWithKey(newAccountSecretKey, payload);
            });

        REQUIRE(!accountContract
                     .prepareInvocation(
                         "set_owner",
                         {makeBytesSCVal(
                             newAccountSecretKey.getPublicKey().ed25519())},
                         accountInvocationSpec)
                     .withAuthorizedTopCall(newSigner)
                     .withDeduplicatedFootprint()
                     .invoke());
        REQUIRE(accountContract
                    .prepareInvocation(
                        "set_owner",
                        {makeBytesSCVal(
                            newAccountSecretKey.getPublicKey().ed25519())},
                        accountInvocationSpec)
                    .withAuthorizedTopCall(signer)
                    .withDeduplicatedFootprint()
                    .invoke());
        REQUIRE(singleInvocation(signer) ==
                InvokeHostFunctionResultCode::INVOKE_HOST_FUNCTION_TRAPPED);
        REQUIRE(singleInvocation(newSigner) ==
                InvokeHostFunctionResultCode::INVOKE_HOST_FUNCTION_SUCCESS);
    }
}

TEST_CASE("Soroban authorization", "[tx][soroban]")
{
    auto cfg = getTestConfig();
    cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS = true;
    SorobanTest test(cfg);

    std::vector<TestContract*> authContracts;
    auto defaultSpec =
        SorobanInvocationSpec()
            .setInstructions(test.getNetworkCfg().txMaxInstructions())
            .setReadBytes(test.getNetworkCfg().txMaxDiskReadBytes())
            .setWriteBytes(test.getNetworkCfg().txMaxWriteBytes());
    auto account = test.getRoot().create(
        "a1", test.getApp().getLedgerManager().getLastMinBalance(1) * 100);

    auto& accountContract =
        test.deployWasmContract(rust_bridge::get_custom_account_wasm());

    SCVal ownerKeyVal = makeVecSCVal({makeSymbolSCVal("Owner")});
    LedgerKey ownerKey = accountContract.getDataKey(
        ownerKeyVal, ContractDataDurability::PERSISTENT);
    auto accountInvocationSpec =
        defaultSpec.extendReadWriteFootprint({ownerKey});
    auto accountSecretKey = SecretKey::pseudoRandomForTesting();
    REQUIRE(accountContract
                .prepareInvocation(
                    "init",
                    {makeBytesSCVal(accountSecretKey.getPublicKey().ed25519())},
                    accountInvocationSpec)
                .invoke());

    auto authContractSpec = accountInvocationSpec;
    for (int i = 0; i < 4; ++i)
    {
        authContracts.emplace_back(&test.deployWasmContract(
            rust_bridge::get_auth_wasm(), defaultSpec.getResources()));
        // Only add Wasm once to the footprint.
        if (i == 0)
        {
            authContractSpec = authContractSpec.extendReadOnlyFootprint(
                {authContracts.back()->getKeys()});
        }
        else
        {
            authContractSpec = authContractSpec.extendReadOnlyFootprint(
                {authContracts.back()->getKeys()[1]});
        }
    }

    auto authTestsForInvocationTree = [&](std::vector<SorobanSigner> const&
                                              signers,
                                          AuthTestTreeNode const& tree) {
        std::vector<SCVal> signerAddresses;
        xdr::xvector<LedgerKey> signerKeys;
        for (auto const& signer : signers)
        {
            signerAddresses.emplace_back(signer.getAddressVal());
        }
        SCVal addressesArg = makeVecSCVal(signerAddresses);

        auto invocationAuth = tree.toAuthorizedInvocation();

        auto invocation =
            authContracts[0]
                ->prepareInvocation("tree_fn",
                                    {
                                        addressesArg,
                                        tree.toSCVal(signers.size()),
                                    },
                                    authContractSpec)
                .withDeduplicatedFootprint();
        SECTION("no auth")
        {
            REQUIRE(!invocation.invoke());
        }
        SECTION("incorrect signature payload")
        {
            std::vector<SorobanCredentials> credentials;
            xdr::xvector<LedgerKey> keys;

            auto wrongAuth = invocationAuth;
            wrongAuth.function.contractFn().functionName = makeSymbol("tree_f");
            for (size_t i = 0; i < signers.size(); ++i)
            {
                auto const& signer = signers[i];
                credentials.push_back(signer.sign(
                    i == signers.size() - 1 ? wrongAuth : invocationAuth));

                keys.insert(keys.end(), signer.getLedgerKeys().begin(),
                            signer.getLedgerKeys().end());

                invocation.withAuthorization(invocationAuth,
                                             credentials.back());
            }
            invocation.withSpec(
                invocation.getSpec().extendReadOnlyFootprint(keys));
            REQUIRE(!invocation.invoke());
        }
        SECTION("success")
        {
            for (auto const& signer : signers)
            {
                invocation.withAuthorization(invocationAuth, signer);
            }
            REQUIRE(invocation.invoke());
            // Try to reuse the same nonce and fail
            REQUIRE(!invocation.invoke());
        }
        SECTION("unused auth entries")
        {
            auto runUnusedAuthEntriesTest = [&](std::string const& fnName) {
                auto extraAuth = invocationAuth;
                extraAuth.function.contractFn().functionName =
                    makeSymbol(fnName);
                for (auto const& signer : signers)
                {
                    invocation.withAuthorization(invocationAuth, signer);
                    // Signature doesn't need to be valid for the extra, so
                    // use the signature for `invocationAuth` instead of
                    // the `extraAuth`.
                    invocation.withAuthorization(extraAuth,
                                                 signer.sign(invocationAuth));
                }
                return invocation.invoke();
            };
            SECTION("success with arbitrary valid function")
            {
                REQUIRE(runUnusedAuthEntriesTest("tree_fn2"));
            }
            SECTION("failure with malformed function name")
            {
                REQUIRE(!runUnusedAuthEntriesTest("tree_fn"
                                                  "\xFF"));
            }
            SECTION("failure with long malformed function name")
            {
                REQUIRE(!runUnusedAuthEntriesTest("tree_fn123456"
                                                  "\xC2"
                                                  "789"));
            }
        }
        SECTION("success with duplicate auth entries")
        {
            std::vector<SorobanCredentials> credentials;
            xdr::xvector<LedgerKey> keys;
            for (int i = 0; i < 2; ++i)
            {
                for (auto const& signer : signers)
                {
                    credentials.push_back(signer.sign(invocationAuth));
                    if (i == 0)
                    {
                        keys.insert(keys.end(), signer.getLedgerKeys().begin(),
                                    signer.getLedgerKeys().end());
                    }
                    invocation.withAuthorization(invocationAuth,
                                                 credentials.back());
                }
            }
            invocation.withSpec(
                invocation.getSpec().extendReadOnlyFootprint(keys));
            REQUIRE(invocation.invoke());
            // Even though there is an extra set of valid auth entries,
            // the invocation will fail on the first entry due to
            // duplicate nonce.
            REQUIRE(!invocation.invoke());

            // Setup a new invocation that only uses the unused entries.
            auto invocation_without_used_entries =
                authContracts[0]
                    ->prepareInvocation(
                        "tree_fn", {addressesArg, tree.toSCVal(signers.size())},
                        authContractSpec)
                    .withDeduplicatedFootprint();
            for (size_t i = credentials.size() / 2; i < credentials.size(); ++i)
            {
                invocation_without_used_entries.withAuthorization(
                    invocationAuth, credentials[i]);
            }
            REQUIRE(invocation_without_used_entries
                        .withSpec(invocation_without_used_entries.getSpec()
                                      .extendReadOnlyFootprint(keys))
                        .invoke());
            REQUIRE(!invocation.invoke());
        }
        SECTION("success for tree with extra nodes")
        {
            auto authTree = tree;
            authTree.add({tree});
            for (auto const& signer : signers)
            {
                invocation.withAuthorization(authTree.toAuthorizedInvocation(),
                                             signer);
            }
            REQUIRE(invocation.invoke());
            // Try to reuse the same nonce and fail
            REQUIRE(!invocation.invoke());
        }
        SECTION("failure for tree with missing node")
        {
            auto authTree = tree;
            authTree.setAddress(authContracts[1]->getAddress());

            for (size_t i = 0; i < signers.size(); ++i)
            {
                if (i != signers.size() - 1)
                {
                    invocation.withAuthorization(invocationAuth, signers[i]);
                }
                else
                {
                    invocation.withAuthorization(
                        authTree.toAuthorizedInvocation(), signers[i]);
                }
            }
            REQUIRE(!invocation.invoke());
        }
    };

    auto genericAuthTest = [&](std::vector<SorobanSigner> signers) {
        SECTION("single call")
        {
            AuthTestTreeNode tree(authContracts[0]->getAddress());
            authTestsForInvocationTree(signers, tree);
        }
        SECTION("wide tree")
        {
            AuthTestTreeNode tree(authContracts[0]->getAddress());
            tree.add({AuthTestTreeNode(authContracts[1]->getAddress()),
                      AuthTestTreeNode(authContracts[2]->getAddress()),
                      AuthTestTreeNode(authContracts[3]->getAddress())});
            authTestsForInvocationTree(signers, tree);
        }
        SECTION("deep tree")
        {
            AuthTestTreeNode tree(authContracts[0]->getAddress());
            tree.add(
                {AuthTestTreeNode(authContracts[1]->getAddress())
                     .add({AuthTestTreeNode(authContracts[2]->getAddress())
                               .add({AuthTestTreeNode(
                                   authContracts[3]->getAddress())})}),
                 AuthTestTreeNode(authContracts[2]->getAddress())
                     .add({AuthTestTreeNode(authContracts[3]->getAddress())}),
                 AuthTestTreeNode(authContracts[3]->getAddress())});

            authTestsForInvocationTree(signers, tree);
        }
    };

    SECTION("default classic account")
    {
        auto signer = test.createClassicAccountSigner(account, {&account});
        genericAuthTest({signer});
    }
    SECTION("classic account with weights")
    {
        account.setOptions(setMasterWeight(5) | setLowThreshold(1) |
                           setMedThreshold(5) | setHighThreshold(10));
        genericAuthTest({test.createClassicAccountSigner(account, {&account})});
    }
    SECTION("multisig classic account")
    {
        auto signerAccount = test.getRoot().create(
            "a2", test.getApp().getLedgerManager().getLastMinBalance(1) * 100);
        account.setOptions(
            setMasterWeight(5) | setLowThreshold(1) | setMedThreshold(10) |
            setHighThreshold(100) |
            setSigner(makeSigner(signerAccount.getSecretKey(), 5)));
        genericAuthTest({test.createClassicAccountSigner(
            account, {&account, &signerAccount})});
    }
    SECTION("custom account")
    {
        auto signer =
            test.createContractSigner(accountContract, [&](uint256 payload) {
                return makeBytesSCVal(accountSecretKey.sign(payload));
            });
        genericAuthTest({signer});
    }
    SECTION("classic and custom accounts")
    {
        auto accountSigner =
            test.createClassicAccountSigner(account, {&account});
        auto customAccountSigner =
            test.createContractSigner(accountContract, [&](uint256 payload) {
                return makeBytesSCVal(accountSecretKey.sign(payload));
            });
        genericAuthTest({accountSigner, customAccountSigner});
    }
}

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

TEST_CASE("Vm instantiation tightening", "[tx][soroban]")
{
    VirtualClock clock;
    auto cfg = getTestConfig(0);
    cfg.USE_CONFIG_FOR_GENESIS = false;

    auto app = createTestApplication(clock, cfg);

    auto upgrade20 = LedgerUpgrade{LEDGER_UPGRADE_VERSION};
    upgrade20.newLedgerVersion() = static_cast<int>(SOROBAN_PROTOCOL_VERSION);
    executeUpgrade(*app, upgrade20);

    // First upload a wasm in v20, upgrade to v21, and then re-upload wasm to
    // create the module cache. Also validate with invocations to the same
    // contract that the required instructions drops after the module cache is
    // created.
    auto wasm = rust_bridge::get_test_wasm_add_i32();
    SorobanTest test(app);

    auto const& addContract = test.deployWasmContract(wasm);
    auto const& wasmHash = addContract.getKeys().front().contractCode().hash;
    {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        auto ltxe = loadContractCode(ltx, wasmHash);
        auto const& code = ltxe.current().data.contractCode();
        REQUIRE(code.ext.v() == 0);
    }

    auto invocation = [&](int64_t instructions) -> bool {
        auto fnName = "add";
        auto sc7 = makeI32(7);
        auto sc16 = makeI32(16);

        auto invocationSpec = SorobanInvocationSpec()
                                  .setInstructions(instructions)
                                  .setReadBytes(2'000)
                                  .setInclusionFee(12345);

        uint32_t const expectedRefund = 100'000;
        auto spec = invocationSpec.setNonRefundableResourceFee(33'000)
                        .setRefundableResourceFee(expectedRefund);

        auto invocation = addContract.prepareInvocation(fnName, {sc7, sc16},
                                                        spec, expectedRefund);
        auto tx = invocation.createTx();
        return isSuccessResult(test.invokeTx(tx));
    };

    // Two million instructions is enough, but one million isn't without the
    // module cache. Run the same invocations after the v21 upgrade as well.
    REQUIRE(invocation(2'000'000));
    REQUIRE(!invocation(1'000'000));

    auto upgrade21 = LedgerUpgrade{LEDGER_UPGRADE_VERSION};
    upgrade21.newLedgerVersion() = static_cast<int>(ProtocolVersion::V_21);
    executeUpgrade(*app, upgrade21);

    REQUIRE(invocation(2'000'000));
    REQUIRE(!invocation(1'000'000));

    auto resources = defaultUploadWasmResourcesWithoutFootprint(
        wasm, getLclProtocolVersion(test.getApp()));
    auto tx = makeSorobanWasmUploadTx(test.getApp(), test.getRoot(), wasm,
                                      resources, 1000);

    auto r = closeLedger(test.getApp(), {tx});
    checkTx(0, r, txSUCCESS);

    {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        auto ltxe = loadContractCode(ltx, wasmHash);
        auto const& code = ltxe.current().data.contractCode();
        REQUIRE(code.ext.v() == 1);

        auto const& inputs = code.ext.v1().costInputs;
        REQUIRE(inputs.nInstructions == 119);
        REQUIRE(inputs.nFunctions == 3);
        REQUIRE(inputs.nGlobals == 3);
        REQUIRE(inputs.nTableEntries == 0);
        REQUIRE(inputs.nTypes == 3);
        REQUIRE(inputs.nDataSegments == 0);
        REQUIRE(inputs.nElemSegments == 0);
        REQUIRE(inputs.nImports == 2);
        REQUIRE(inputs.nExports == 5);
        REQUIRE(inputs.nDataSegmentBytes == 0);
    }

    // After the upload adding the module cache, 1 million instructions is
    // enough.
    REQUIRE(invocation(1'000'000));

    // Now upload a new wasm in v21
    auto const& contract2 =
        test.deployWasmContract(rust_bridge::get_hostile_large_val_wasm());
    {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        auto ltxe = loadContractCode(
            ltx, contract2.getKeys().front().contractCode().hash);
        auto const& code = ltxe.current().data.contractCode();
        REQUIRE(code.ext.v() == 1);

        auto const& inputs = code.ext.v1().costInputs;
        REQUIRE(inputs.nInstructions == 417);
        REQUIRE(inputs.nFunctions == 18);
        REQUIRE(inputs.nGlobals == 3);
        REQUIRE(inputs.nTableEntries == 0);
        REQUIRE(inputs.nTypes == 9);
        REQUIRE(inputs.nDataSegments == 0);
        REQUIRE(inputs.nElemSegments == 0);
        REQUIRE(inputs.nImports == 6);
        REQUIRE(inputs.nExports == 14);
        REQUIRE(inputs.nDataSegmentBytes == 0);
    }
}

TEST_CASE("contract constructor support", "[tx][soroban]")
{
    Config cfg = getTestConfig();
    cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS = true;
    SorobanTest test(cfg);
    auto defaultSpec =
        SorobanInvocationSpec()
            .setInstructions(test.getNetworkCfg().txMaxInstructions())
            .setReadBytes(test.getNetworkCfg().txMaxDiskReadBytes())
            .setWriteBytes(test.getNetworkCfg().txMaxWriteBytes());
    auto constructorDefaultSpec = [&]() {
        auto contractAddress = test.nextContractID();
        return SorobanInvocationSpec().setReadWriteFootprint(
            {contractDataKey(contractAddress, makeSymbolSCVal("key"),
                             ContractDataDurability::PERSISTENT),
             contractDataKey(contractAddress, makeSymbolSCVal("key"),
                             ContractDataDurability::TEMPORARY)});
    };

    SECTION("constructor with no arguments")
    {

        auto testNoArgConstructor =
            [&](ConstructorParams::HostFnVersion hostFnVersion,
                ConstructorParams::HostFnVersion authFnVersion) {
                ConstructorParams params;
                params.additionalResources =
                    std::make_optional(constructorDefaultSpec().getResources());
                params.forceHostFnVersion = hostFnVersion;
                params.forceAuthHostFnVersion = authFnVersion;
                auto& contract = test.deployWasmContract(
                    rust_bridge::get_no_arg_constructor_wasm(), params);
                auto invocation = contract.prepareInvocation(
                    "get_data", {makeSymbolSCVal("key")},
                    defaultSpec.setReadOnlyFootprint(
                        params.additionalResources->footprint.readWrite));
                REQUIRE(invocation.invoke());
                REQUIRE(invocation.getReturnValue().u32() == 6);
            };
        SECTION("v1 host function, v1 auth")
        {
            testNoArgConstructor(ConstructorParams::HostFnVersion::V1,
                                 ConstructorParams::HostFnVersion::V1);
        }
        SECTION("v1 host function, v2 auth")
        {
            testNoArgConstructor(ConstructorParams::HostFnVersion::V1,
                                 ConstructorParams::HostFnVersion::V2);
        }
        SECTION("v2 host function, v1 auth")
        {
            testNoArgConstructor(ConstructorParams::HostFnVersion::V2,
                                 ConstructorParams::HostFnVersion::V1);
        }
        SECTION("v2 host function, v2 auth")
        {
            testNoArgConstructor(ConstructorParams::HostFnVersion::V2,
                                 ConstructorParams::HostFnVersion::V2);
        }
    }

    SECTION("constructor with arguments and auth")
    {
        auto authContract =
            test.deployWasmContract(rust_bridge::get_auth_wasm());

        auto sourceAccountVal =
            makeAddressSCVal(makeAccountAddress(test.getRoot().getPublicKey()));
        ConstructorParams params;
        params.constructorArgs = {sourceAccountVal, makeSymbolSCVal("key"),
                                  makeU32(100),
                                  makeAddressSCVal(authContract.getAddress())};
        params.additionalResources =
            constructorDefaultSpec()
                .setReadOnlyFootprint(authContract.getKeys())
                .setReadBytes(rust_bridge::get_auth_wasm().data.size() + 100)
                .getResources();

        auto& subInvocation = params.additionalAuthInvocations.emplace_back();
        auto& constructorInvocation = subInvocation.function.contractFn();
        constructorInvocation.contractAddress = test.nextContractID();
        constructorInvocation.functionName = makeSymbol("__constructor");
        constructorInvocation.args = params.constructorArgs;

        auto& authInvocation =
            subInvocation.subInvocations.emplace_back().function.contractFn();
        authInvocation.contractAddress = authContract.getAddress();
        authInvocation.functionName = makeSymbol("do_auth");
        authInvocation.args = {sourceAccountVal, makeU32(100)};

        auto& contract = test.deployWasmContract(
            rust_bridge::get_constructor_with_args_p22_wasm(), params);
        auto invocation = contract.prepareInvocation(
            "get_data", {makeSymbolSCVal("key")},
            defaultSpec.setReadOnlyFootprint(
                params.additionalResources->footprint.readWrite));
        REQUIRE(invocation.invoke());
        REQUIRE(invocation.getReturnValue().u32() == 303);
    }
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
    auto moduleCache = app.getLedgerManager().getModuleCache();
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

static const int64_t INVOKE_ADD_UNCACHED_COST_PASS = 500'000;
static const int64_t INVOKE_ADD_UNCACHED_COST_FAIL = 400'000;

static const int64_t INVOKE_ADD_CACHED_COST_PASS = 300'000;
static const int64_t INVOKE_ADD_CACHED_COST_FAIL = 200'000;

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
    auto moduleCache = lm.getModuleCache();
    auto const wasmHash = sha256(wasm);
    REQUIRE(!moduleCache->contains_module(
        proto, ::rust::Slice{wasmHash.data(), wasmHash.size()}));

    SECTION("scenario A: restore in one ledger, invoke in next")
    {
        // Restore contract in ledger N+1
        test.invokeRestoreOp(contractKeys, 40333);

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
