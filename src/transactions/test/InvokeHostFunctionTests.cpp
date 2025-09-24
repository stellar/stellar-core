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
#include <numeric>
#include <stdexcept>
#include <xdrpp/printer.h>

#include "crypto/Random.h"
#include "crypto/SecretKey.h"
#include "herder/Herder.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTypeUtils.h"
#include "ledger/test/LedgerTestUtils.h"
#include "main/Application.h"
#include "main/CommandHandler.h"
#include "main/SettingsUpgradeUtils.h"
#include "rust/RustBridge.h"
#include "test/Catch2.h"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "transactions/InvokeHostFunctionOpFrame.h"
#include "transactions/SignatureUtils.h"
#include "transactions/TransactionUtils.h"
#include "transactions/test/SorobanTxTestUtils.h"
#include "transactions/test/SponsorshipTestUtils.h"
#include "util/Decoder.h"
#include "util/MetaUtils.h"
#include "util/TmpDir.h"
#include "util/XDRCereal.h"
#include "xdr/Stellar-contract.h"
#include "xdr/Stellar-ledger-entries.h"
#include <autocheck/autocheck.hpp>
#include <fmt/format.h>
#include <limits>
#include <type_traits>
#include <variant>

#include "ledger/LedgerManagerImpl.h"

using namespace stellar;
using namespace stellar::txtest;

namespace
{

void
checkResults(TransactionResultSet& r, int expectedSuccess, int expectedFailed)
{
    int successCounter = 0;
    int failedCounter = 0;
    for (size_t i = 0; i < r.results.size(); ++i)
    {
        r.results[i].result.result.code() == txSUCCESS ||
                r.results[i].result.result.code() == txFEE_BUMP_INNER_SUCCESS
            ? ++successCounter
            : ++failedCounter;
    }

    REQUIRE(successCounter == expectedSuccess);
    REQUIRE(expectedFailed == expectedFailed);
};

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

    // Test batch_transfer with 5 destinations
    {
        auto batchDest1 = root.create("batchDest1", minBalance);
        auto batchDest2 = root.create("batchDest2", minBalance);
        auto batchDest3 = root.create("batchDest3", minBalance);
        auto batchDest4 = root.create("batchDest4", minBalance);
        auto batchDest5 = root.create("batchDest5", minBalance);

        auto batchDestAddr1 = makeAccountAddress(batchDest1);
        auto batchDestAddr2 = makeAccountAddress(batchDest2);
        auto batchDestAddr3 = makeAccountAddress(batchDest3);
        auto batchDestAddr4 = makeAccountAddress(batchDest4);
        auto batchDestAddr5 = makeAccountAddress(batchDest5);

        // Get initial balances
        auto batchDest1InitialBalance = batchDest1.getBalance();
        auto batchDest2InitialBalance = batchDest2.getBalance();
        auto batchDest3InitialBalance = batchDest3.getBalance();
        auto batchDest4InitialBalance = batchDest4.getBalance();
        auto batchDest5InitialBalance = batchDest5.getBalance();

        // Prepare the destination addresses vector with 5 addresses
        std::vector<SCVal> destinations = {
            makeAddressSCVal(batchDestAddr1), makeAddressSCVal(batchDestAddr2),
            makeAddressSCVal(batchDestAddr3), makeAddressSCVal(batchDestAddr4),
            makeAddressSCVal(batchDestAddr5)};
        SCVal destinationsVec = makeVecSCVal(destinations);

        auto batchSpec = invocationSpec.extendReadWriteFootprint(
            {fromBalanceKey, // transferContract's balance
             client.makeBalanceKey(batchDestAddr1),
             client.makeBalanceKey(batchDestAddr2),
             client.makeBalanceKey(batchDestAddr3),
             client.makeBalanceKey(batchDestAddr4),
             client.makeBalanceKey(batchDestAddr5)});

        auto batchInvocation = transferContract.prepareInvocation(
            "batch_transfer",
            {makeAddressSCVal(client.getContract().getAddress()),
             destinationsVec},
            batchSpec);

        REQUIRE(batchInvocation.invoke());

        // Check that all transfers succeeded:
        // - transferContract balance should decrease by 5 (from 8 to 3)
        // - Each destination should receive 1 stroop
        REQUIRE(client.getBalance(transferContract.getAddress()) == 3);
        REQUIRE(batchDest1.getBalance() == batchDest1InitialBalance + 1);
        REQUIRE(batchDest2.getBalance() == batchDest2InitialBalance + 1);
        REQUIRE(batchDest3.getBalance() == batchDest3InitialBalance + 1);
        REQUIRE(batchDest4.getBalance() == batchDest4InitialBalance + 1);
        REQUIRE(batchDest5.getBalance() == batchDest5InitialBalance + 1);
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

TEST_CASE_VERSIONS("multiple soroban ops in a tx", "[tx][soroban]")
{
    Config cfg = getTestConfig();
    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);

    for_versions_from(20, *app, [&] {
        SorobanTest test(app);
        ContractStorageTestClient client(test);

        const int64_t startingBalance =
            test.getApp().getLedgerManager().getLastMinBalance(50);

        auto& root = test.getRoot();
        auto a1 = root.create("a1", startingBalance);

        Operation op1;
        op1.body.type(EXTEND_FOOTPRINT_TTL);
        op1.body.extendFootprintTTLOp().extendTo = 1000;
        Operation op2 = op1;

        SorobanResources extendResources;
        extendResources.footprint.readOnly = {
            client.getContract().getKeys()[0]};
        extendResources.diskReadBytes = 10'000;

        SorobanInvocationSpec spec(extendResources, 10'000, 30'000, 500'000);

        auto tx = sorobanTransactionFrameFromOps(
            app->getNetworkID(), a1, {op1, op2}, {}, spec.getResources(),
            spec.getInclusionFee(), spec.getResourceFee());

        // The tx is invalid, so it won't be included in the ledger.
        SECTION("without fee bump")
        {
            auto r = closeLedger(test.getApp(), {tx});
            REQUIRE(r.results.size() == 0);
        }
        SECTION("with fee bump")
        {
            auto feeBumper =
                test.getRoot().create("feeBumper", startingBalance);

            int64_t feeBumpFullFee = tx->getEnvelope().v1().tx.fee * 5;
            auto feeBumpTxFrame =
                feeBump(test.getApp(), feeBumper, tx, feeBumpFullFee,
                        /*useInclusionAsFullFee=*/true);

            auto r = closeLedger(test.getApp(), {feeBumpTxFrame});
            REQUIRE(r.results.size() == 0);
        }
    });
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

        auto getRootBalance = [&]() {
            LedgerTxn ltx(test.getApp().getLedgerTxnRoot());
            auto entry = stellar::loadAccount(ltx, rootAccount.getPublicKey());
            return entry.current().data.account().balance;
        };

        int64_t initBalance = getRootBalance();
        auto invocation = contract.prepareInvocation(functionName, args, spec,
                                                     addContractKeys);
        auto tx = invocation.createTx(&rootAccount);
        auto result = test.invokeTx(tx);

        REQUIRE(tx->getFullFee() ==
                spec.getInclusionFee() + spec.getResourceFee());
        REQUIRE(tx->getInclusionFee() == spec.getInclusionFee());
        auto txm = test.getLastTxMeta();
        auto refundChanges =
            protocolVersionIsBefore(test.getLedgerVersion(),
                                    PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION)
                ? txm.getChangesAfter()
                : test.getLastLcm().getPostTxApplyFeeProcessing(0);

        // In case of failure we simply refund the whole refundable fee portion.
        if (!expectedRefund)
        {
            REQUIRE(!isSuccessResult(result)); // We expect a failure here.
            // Compute the exact refundable fee (so we don't need spec
            // to have the exact refundable fee set).
            auto nonRefundableFee =
                sorobanResourceFee(test.getApp(), tx->sorobanResources(),
                                   xdr::xdr_size(tx->getEnvelope()), 0);
            expectedRefund = spec.getResourceFee() - nonRefundableFee;
        }

        // Verify refund meta
        REQUIRE(refundChanges.size() == 2);
        REQUIRE(refundChanges[1].updated().data.account().balance -
                    refundChanges[0].state().data.account().balance ==
                *expectedRefund);

        REQUIRE(initBalance - result.feeCharged == getRootBalance());

        return std::make_tuple(txm, std::move(result));
    };

    auto failedInvoke =
        [&](TestContract& contract, std::string const& functionName,
            std::vector<SCVal> const& args, SorobanInvocationSpec const& spec,
            bool addContractKeys = true) {
            auto successesBefore = hostFnSuccessMeter.count();
            auto failuresBefore = hostFnFailureMeter.count();
            auto [mtxm, result] = invoke(contract, functionName, args, spec,
                                         std::nullopt, addContractKeys);
            REQUIRE(hostFnSuccessMeter.count() - successesBefore == 0);
            REQUIRE(hostFnFailureMeter.count() - failuresBefore == 1);
            REQUIRE(result.result.code() == txFAILED);
            return result.result.results()[0]
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

    // After p23, in-memory reads are not charged fees
    uint32_t const expectedResourceFee =
        protocolVersionIsBefore(cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION,
                                ProtocolVersion::V_23)
            ? 32'702
            : 22'702;
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

        auto [txm, result] =
            invoke(addContract, fnName, {sc7, sc16}, spec, expectedRefund);

        REQUIRE(hostFnSuccessMeter.count() - successesBefore == 1);
        REQUIRE(hostFnFailureMeter.count() - failuresBefore == 0);
        REQUIRE(isSuccessResult(result));

        auto const& ores = result.result.results().at(0);
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
        if (protocolVersionStartsFrom(
                cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION,
                ProtocolVersion::V_23))
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
                                    ProtocolVersion::V_23))
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

        auto result = test.invokeTx(tx);

        REQUIRE(isSuccessResult(result));
        return test.getLastTxMeta();
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
    auto appCfg = getTestConfig();
    if (protocolVersionIsBefore(appCfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION,
                                SOROBAN_PROTOCOL_VERSION))
    {
        return;
    }

    SorobanTest test(appCfg, true, [](SorobanNetworkConfig& cfg) {
        cfg.mTxMaxWriteLedgerEntries = cfg.mTxMaxDiskReadEntries;
        cfg.mTxMaxFootprintEntries = (cfg.mTxMaxDiskReadEntries * 2) + 10;
    });
    auto cfg = test.getNetworkCfg();

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

    auto testReadWritesLimits = [&](bool readOnly, bool addArchivedEntries,
                                    std::unordered_set<LedgerEntryType> types,
                                    auto count, bool shouldBeValid) {
        UnorderedSet<LedgerKey> seenKeys;
        auto entries = LedgerTestUtils::generateValidUniqueLedgerKeysWithTypes(
            types, count, seenKeys);
        std::optional<std::vector<uint32_t>> archivedIndexes;

        for (size_t i = 0; i < entries.size(); ++i)
        {
            auto const& entry = entries[i];
            if (readOnly)
            {
                resources.footprint.readOnly.emplace_back(entry);
            }
            else if (addArchivedEntries)
            {
                resources.footprint.readWrite.emplace_back(entry);
                if (!archivedIndexes)
                {
                    archivedIndexes = std::vector<uint32_t>();
                }
                archivedIndexes->push_back(i);
            }
            else
            {
                if (rand_flip())
                {
                    resources.footprint.readWrite.emplace_back(entry);
                }
                else
                {
                    resources.footprint.readOnly.emplace_back(entry);
                }
            }
        }
        testValidInvoke(shouldBeValid, archivedIndexes);
    };

    if (protocolVersionStartsFrom(
            appCfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION,
            ProtocolVersion::V_23))
    {

        auto const maxDiskReads = cfg.mTxMaxDiskReadEntries;

        // contract instance and WASM entry are already in the fooprint
        auto const maxTotalReads = cfg.mTxMaxFootprintEntries - 2;

        SECTION("classic entries counted as disk")
        {
            SECTION("read only")
            {
                SECTION("max + 1")
                {
                    testReadWritesLimits(/*readOnly=*/true,
                                         /*addArchivedEntries=*/false,
                                         {ACCOUNT}, maxDiskReads + 1,
                                         /*shouldBeValid=*/false);
                }

                SECTION("max")
                {
                    testReadWritesLimits(/*readOnly=*/true,
                                         /*addArchivedEntries=*/false,
                                         {ACCOUNT}, maxDiskReads,
                                         /*shouldBeValid=*/true);
                }
            }

            SECTION("read write")
            {
                SECTION("max + 1")
                {
                    testReadWritesLimits(/*readOnly=*/false,
                                         /*addArchivedEntries=*/false,
                                         {ACCOUNT}, maxDiskReads + 1,
                                         /*shouldBeValid=*/false);
                }

                SECTION("max")
                {
                    testReadWritesLimits(/*readOnly=*/false,
                                         /*addArchivedEntries=*/false,
                                         {ACCOUNT}, maxDiskReads,
                                         /*shouldBeValid=*/true);
                }
            }
        }

        SECTION("soroban entries")
        {
            SECTION("in-memory")
            {
                SECTION("max + 1")
                {
                    testReadWritesLimits(/*readOnly=*/true,
                                         /*addArchivedEntries=*/false,
                                         {CONTRACT_CODE}, maxTotalReads + 1,
                                         /*shouldBeValid=*/false);
                }

                SECTION("max")
                {
                    testReadWritesLimits(/*readOnly=*/true,
                                         /*addArchivedEntries=*/false,
                                         {CONTRACT_CODE}, maxTotalReads,
                                         /*shouldBeValid=*/true);
                }
            }

            SECTION("archived entries")
            {
                SECTION("max + 1")
                {
                    testReadWritesLimits(/*readOnly=*/false,
                                         /*addArchivedEntries=*/true,
                                         {CONTRACT_CODE}, maxDiskReads + 1,
                                         /*shouldBeValid=*/false);
                }

                SECTION("max")
                {
                    testReadWritesLimits(/*readOnly=*/false,
                                         /*addArchivedEntries=*/true,
                                         {CONTRACT_CODE}, maxDiskReads,
                                         /*shouldBeValid=*/true);
                }
            }
        }
    }
    else
    {
        auto const maxReads = cfg.mTxMaxDiskReadEntries;

        SECTION("soroban only")
        {
            SECTION("max + 1")
            {
                SECTION("read only")
                {
                    testReadWritesLimits(/*readOnly=*/true,
                                         /*addArchivedEntries=*/false,
                                         {CONTRACT_CODE}, maxReads + 1,
                                         /*shouldBeValid=*/false);
                }

                SECTION("read and write")
                {
                    testReadWritesLimits(/*readOnly=*/false,
                                         /*addArchivedEntries=*/false,
                                         {CONTRACT_CODE}, maxReads + 1,
                                         /*shouldBeValid=*/false);
                }
            }
        }

        SECTION("soroban classic mixed")
        {
            SECTION("read only")
            {
                SECTION("max + 1")
                {
                    testReadWritesLimits(/*readOnly=*/true,
                                         /*addArchivedEntries=*/false,
                                         {ACCOUNT, CONTRACT_CODE}, maxReads + 1,
                                         /*shouldBeValid=*/false);
                }

                SECTION("max")
                {
                    testReadWritesLimits(/*readOnly=*/true,
                                         /*addArchivedEntries=*/false,
                                         {ACCOUNT, CONTRACT_CODE}, maxReads,
                                         /*shouldBeValid=*/true);
                }
            }

            SECTION("read and write")
            {
                SECTION("max + 1")
                {
                    testReadWritesLimits(/*readOnly=*/false,
                                         /*addArchivedEntries=*/false,
                                         {ACCOUNT, CONTRACT_CODE}, maxReads + 1,
                                         /*shouldBeValid=*/false);
                }

                SECTION("max")
                {
                    testReadWritesLimits(/*readOnly=*/false,
                                         /*addArchivedEntries=*/false,
                                         {ACCOUNT, CONTRACT_CODE}, maxReads,
                                         /*shouldBeValid=*/true);
                }
            }
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
    /// We're using specifically 1000 here because that's also the minimum rent
    // write fee set in Soroban host. Thus this will be the write fee for
    // protocols before 23 (because the bucket list is small enough to fall back
    // to the minimum fee), and in protocols 23 and later we just set this
    // value for the flat rate write fee.
    uint32_t const writeFee = 1000;

    VirtualClock clock;
    auto cfg = getTestConfig();
    auto app = createTestApplication(clock, cfg);

    for_versions_from(20, *app, [&] {
        auto cfgModifyFn = [&](SorobanNetworkConfig& cfg) {
            cfg.mFeeRatePerInstructionsIncrement = 1000;
            cfg.mFeeDiskReadLedgerEntry = 2000;
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
            auto config =
                app.getLedgerManager().getLastClosedSorobanNetworkConfig();
            // Sanity check the tx fee computation logic.
            auto actualFeePair = validTx->getRawTransactionFrame()
                                     .computePreApplySorobanResourceFee(
                                         app.getLedgerManager()
                                             .getLastClosedLedgerHeader()
                                             .header.ledgerVersion,
                                         config, app.getConfig());
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
        {
            INFO("tx size fees");
            SorobanResources resources;
            checkFees(resources, baseTxFee);
        }
        {
            INFO("compute fee");
            SorobanResources resources;
            resources.instructions = 12'345'678;
            checkFees(resources, 1'234'568 + baseTxFee);
        }

        {
            INFO("footprint entries");
            // Fee for additonal 6 footprint entries (6 * 36 = 216 bytes)
            // ceil(216 * 6000 / 1024) + ceil(216 * 8000 / 1024) == 2954
            const int64_t additionalTxSizeFee = 2954;
            {
                INFO("RO only");
                SorobanResources resources;
                LedgerKey lk(LedgerEntryType::CONTRACT_CODE);
                for (uint8_t i = 0; i < 6; ++i)
                {
                    lk.contractCode().hash[0] = i;
                    resources.footprint.readOnly.push_back(lk);
                }

                // In-memory reads are not charged fees after protocol 23.
                int64_t expectedFee = baseTxFee + additionalTxSizeFee;
                if (protocolVersionIsBefore(test.getLedgerVersion(),
                                            ProtocolVersion::V_23))
                {
                    expectedFee += 2000 * 6;
                }
                checkFees(resources, expectedFee);
            }
            {
                INFO("RW only");
                SorobanResources resources;
                LedgerKey lk(LedgerEntryType::CONTRACT_CODE);
                for (uint8_t i = 0; i < 6; ++i)
                {
                    lk.contractCode().hash[0] = i;
                    resources.footprint.readWrite.push_back(lk);
                }

                // In-memory reads are not charged fees after protocol 23.
                int64_t expectedFee =
                    baseTxFee + additionalTxSizeFee + 3000 * 6;
                if (protocolVersionIsBefore(test.getLedgerVersion(),
                                            ProtocolVersion::V_23))
                {
                    expectedFee += 2000 * 6;
                }
                checkFees(resources, expectedFee);
            }
            {
                INFO("RW and RO");
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

                // In-memory reads are not charged fees after protocol 23.
                int64_t expectedFee =
                    baseTxFee + additionalTxSizeFee + 3000 * 3;
                if (protocolVersionIsBefore(test.getLedgerVersion(),
                                            ProtocolVersion::V_23))
                {
                    expectedFee += 2000 * (3 + 3);
                }

                checkFees(resources, expectedFee);
            }
        }

        {
            INFO("readBytes fee");
            SorobanResources resources;
            resources.diskReadBytes = 5 * 1024 + 1;
            checkFees(resources, 20'004 + baseTxFee);
        }
        {
            INFO("writeBytes fee");
            SorobanResources resources;
            resources.writeBytes = 5 * 1024 + 1;
            checkFees(resources, 5001 + baseTxFee);
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
        REQUIRE(r.results.size() == 2);

        checkTx(0, r, txSUCCESS);

        // The source account of the soroban tx was merged during the classic
        // phase
        checkTx(1, r, txNO_ACCOUNT);

        // The inclusion fee is 1000, but the tx is not surge priced, so remove
        // everything above the min baseFee (100)
        int64_t initialFee = tx->getEnvelope().v1().tx.fee - 900;

        REQUIRE(r.results[1].result.feeCharged == initialFee);

        auto const& txEvents = test.getLastTxMeta(1).getTxEvents();

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

        auto const& txEvents = test.getLastTxMeta(1).getTxEvents();

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

        auto const& txEvents = test.getLastTxMeta(1).getTxEvents();

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
                ? 981'248
                : 981'527;
        int64_t initialFee = tx->getEnvelope().v1().tx.fee;
        REQUIRE(a1.getBalance() ==
                a1StartingBalance - initialFee + expectedRefund);

        // DEFAULT_TEST_RESOURCE_FEE is added onto the calculated soroban
        // resource fee, so the total cost would be greater than
        // DEFAULT_TEST_RESOURCE_FEE without the refund.
        REQUIRE(initialFee - expectedRefund < DEFAULT_TEST_RESOURCE_FEE);

        auto const& txEvents = test.getLastTxMeta().getTxEvents();

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
        auto ledgerVersion = test.getLedgerVersion();

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

        int64_t feeBumpFullFee = tx->getEnvelope().v1().tx.fee * 5;
        auto feeBumpTxFrame =
            feeBump(test.getApp(), feeBumper, tx, feeBumpFullFee,
                    /*useInclusionAsFullFee=*/true);
        auto r = closeLedger(test.getApp(), {feeBumpTxFrame});
        checkTx(0, r, txFEE_BUMP_INNER_SUCCESS);

        bool afterV20 =
            protocolVersionStartsFrom(ledgerVersion, ProtocolVersion::V_21);

        int64_t expectedRefund =
            protocolVersionStartsFrom(test.getLedgerVersion(),
                                      ProtocolVersion::V_23)
                ? 981'248
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

        // Verify refund meta
        auto const& txm = test.getLastTxMeta();
        auto refundChanges =
            protocolVersionIsBefore(test.getLedgerVersion(),
                                    PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION)
                ? txm.getChangesAfter()
                : test.getLastLcm().getPostTxApplyFeeProcessing(0);

        auto const& txEvents = txm.getTxEvents();

        REQUIRE(refundChanges.size() == 2);
        REQUIRE(refundChanges[1].updated().data.account().balance -
                    refundChanges[0].state().data.account().balance ==
                expectedRefund);

        REQUIRE(txEvents.size() == 2);
        validateFeeEvent(txEvents[0], feeBumper.getPublicKey(), initialFee,
                         ledgerVersion, false);
        validateFeeEvent(txEvents[1], feeBumper.getPublicKey(), -expectedRefund,
                         ledgerVersion, true);
    });
}

TEST_CASE("resource fee exceeds uint32", "[tx][soroban][feebump]")
{
    Config cfg = getTestConfig();
    cfg.EMIT_CLASSIC_EVENTS = true;
    cfg.BACKFILL_STELLAR_ASSET_EVENTS = true;

    int64_t const stroopsInXlm = 10'000'000;

    SorobanTest test(getTestConfig(), true, [&](SorobanNetworkConfig& cfg) {
        // Set the rent fee settings to rather high, but somewhat realistic
        // values: 120 days for min persistent TTL, and 500k stroops per 1KB of
        // rent ( the rent fee is realistic if storage enters 'surge pricing'
        // mode past the target size).
        cfg.mStateArchivalSettings.minPersistentTTL = 120 * 24 * 3600 / 5;
        cfg.mRentFee1KBSorobanStateSizeLow = 500'000;
        cfg.mRentFee1KBSorobanStateSizeHigh =
            cfg.mRentFee1KBSorobanStateSizeLow;
    });

    int64_t const expectedRentFee = 8'395'575'720LL;
    int64_t const uploadEventsSize = 40;

    auto runTest = [&](int64_t feeBumperBalance, int64_t inclusionFee,
                       int64_t rentFee, bool selfFeeBump) {
        auto a1 = test.getRoot().create(
            "A", test.getApp().getLedgerManager().getLastMinBalance(1));
        auto feeBumper = test.getRoot().create("B", feeBumperBalance);

        auto a1StartingBalance = a1.getBalance();
        auto feeBumperStartingBalance = feeBumper.getBalance();
        auto wasm = rust_bridge::get_test_wasm_add_i32();
        auto resources = defaultUploadWasmResourcesWithoutFootprint(
            wasm, test.getLedgerVersion());
        auto& innerAccount = selfFeeBump ? feeBumper : a1;

        auto initTx = makeSorobanWasmUploadTx(test.getApp(), innerAccount, wasm,
                                              resources, 0, 0);
        auto txResourceFee = sorobanResourceFee(
            test.getApp(), initTx->sorobanResources(),
            xdr::xdr_size(initTx->getEnvelope()), uploadEventsSize);
        auto txEnvelope = initTx->getEnvelope();
        int64_t resourceFee = txResourceFee + rentFee;
        txEnvelope.v1().tx.ext.sorobanData().resourceFee = resourceFee;
        txEnvelope.v1().signatures.clear();
        txtest::sign(test.getApp().getNetworkID(), innerAccount.getSecretKey(),
                     txEnvelope.v1());
        auto tx = TransactionFrameBase::makeTransactionFromWire(
            test.getApp().getNetworkID(), txEnvelope);
        LedgerSnapshot ls(test.getApp());
        auto diagnostics = DiagnosticEventManager::createDisabled();
        auto innerCheckValidResult = tx->checkValid(
            test.getApp().getAppConnector(), ls, 0, 0, 0, diagnostics);

        int64_t feeBumpFullFee = resourceFee + inclusionFee;
        auto feeBumpTx = feeBump(test.getApp(), feeBumper, tx, feeBumpFullFee,
                                 /*useInclusionAsFullFee=*/true);

        REQUIRE(feeBumpTx->getInclusionFee() == inclusionFee);

        auto checkValidResult = feeBumpTx->checkValid(
            test.getApp().getAppConnector(), ls, 0, 0, 0, diagnostics);
        if (!checkValidResult->isSuccess())
        {
            return checkValidResult->getResultCode();
        }
        auto result = test.invokeTx(feeBumpTx);
        // There should be no change to a1's balance
        REQUIRE(a1.getBalance() == a1StartingBalance);
        bool success = isSuccessResult(result);
        test.checkRefundableFee(feeBumperStartingBalance, feeBumpTx,
                                success ? expectedRentFee : 0,
                                /*eventsSize=*/40, success);
        return result.result.code();
    };

    auto runTests = [&](bool selfFeeBump) {
        SECTION("success")
        {

            SECTION("low inclusion fee")
            {
                REQUIRE(runTest(10000 * stroopsInXlm, 200, expectedRentFee,
                                selfFeeBump) == txFEE_BUMP_INNER_SUCCESS);
            }
            SECTION("inclusion fee exceeds uint32")
            {
                REQUIRE(runTest(10000 * stroopsInXlm, 700 * stroopsInXlm,
                                expectedRentFee,
                                selfFeeBump) == txFEE_BUMP_INNER_SUCCESS);
            }
            SECTION("inclusion fee and refund exceed uint32")
            {
                int64_t const rentFee = 2000 * stroopsInXlm;
                // Make sure that refund will also exceed uint32 max (rentFee is
                // not the full resource fee, but makes up for a bulk of it).
                REQUIRE(rentFee - expectedRentFee >
                        std::numeric_limits<uint32_t>::max());

                REQUIRE(runTest(10000 * stroopsInXlm, 700 * stroopsInXlm,
                                rentFee,
                                selfFeeBump) == txFEE_BUMP_INNER_SUCCESS);
            }
        }
        SECTION("failure")
        {
            SECTION("insufficient fee bumper balance")
            {
                REQUIRE(runTest(expectedRentFee, 200, expectedRentFee,
                                selfFeeBump) == txINSUFFICIENT_BALANCE);
            }
            SECTION("fee bump fee is not sufficient to cover the resource fee")
            {
                REQUIRE(runTest(10000 * stroopsInXlm, 200, expectedRentFee - 1,
                                selfFeeBump) == txFEE_BUMP_INNER_FAILED);
            }
            SECTION(
                "fee bump fee is not sufficient to cover the resource fee with "
                "high inclusion fee")
            {
                REQUIRE(runTest(10000 * stroopsInXlm, 1000 * stroopsInXlm,
                                expectedRentFee - 1,
                                selfFeeBump) == txFEE_BUMP_INNER_FAILED);
            }
            SECTION(
                "fee bump fee is not sufficient to cover the resource fee with "
                "low resource fee")
            {
                REQUIRE(runTest(10000 * stroopsInXlm, 1000,
                                std::numeric_limits<uint32_t>::max(),
                                selfFeeBump) == txFEE_BUMP_INNER_FAILED);
            }
        }
    };
    SECTION("other account fee bump")
    {
        runTests(false);
    }
    SECTION("self fee bump")
    {
        runTests(true);
    }
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
    auto cfg = test.getNetworkCfg();
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
                                SOROBAN_PROTOCOL_VERSION))
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
                ProtocolVersion::V_23))
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
        auto stateArchivalSettings =
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
                    ? 8'793
                    : 943;
            int const rentBumpForInstance =
                protocolVersionStartsFrom(test.getLedgerVersion(),
                                          ProtocolVersion::V_23)
                    ? 199
                    : 939;
            int const rentBumpForInstanceAndWasm =
                protocolVersionStartsFrom(test.getLedgerVersion(),
                                          ProtocolVersion::V_23)
                    ? 8'991
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
                        ? 20'194
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
                        ? 40'599
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
                        .setRefundableResourceFee(10'000'000))));

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
                REQUIRE(isSuccess(client.extend(
                    "key", ContractDataDurability::TEMPORARY,
                    stateArchivalSettings.maxEntryTTL - 1,
                    stateArchivalSettings.maxEntryTTL - 1,
                    client.readKeySpec("key", ContractDataDurability::TEMPORARY)
                        .setRefundableResourceFee(10'000'000))));
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
        uint32_t const expectedRefundableFee = 3'292'183;
        SECTION("failed due to low fee")
        {
            REQUIRE(
                client.resizeStorageAndExtend(
                    "key", 5, 1'000'000 - 2, 1'000'000,
                    spec.setRefundableResourceFee(expectedRefundableFee - 1)) ==
                INVOKE_HOST_FUNCTION_INSUFFICIENT_REFUNDABLE_FEE);
        }
        SECTION("success")
        {
            REQUIRE(isSuccess(client.resizeStorageAndExtend(
                "key", 5, 1'000'000 - 3, 1'000'000,
                spec.setRefundableResourceFee(expectedRefundableFee))));
        }
    }

    SECTION("resize and extend")
    {
        uint32_t const expectedRefundableFee = 7'488'664;
        SECTION("failed due to low fee")
        {
            REQUIRE(
                client.resizeStorageAndExtend(
                    "key", 5, 2'000'000, 2'000'000,
                    spec.setRefundableResourceFee(expectedRefundableFee - 1)) ==
                INVOKE_HOST_FUNCTION_INSUFFICIENT_REFUNDABLE_FEE);
        }
        SECTION("success")
        {
            REQUIRE(isSuccess(client.resizeStorageAndExtend(
                "key", 5, 2'000'000, 2'000'000,
                spec.setRefundableResourceFee(expectedRefundableFee))));
        }
    }
}

TEST_CASE_VERSIONS("archival meta", "[tx][soroban][archival]")
{
    // This test verifies that archival metadata is properly emitted when:
    // 1. Temporary entries are evicted
    // 2. Persistent entries are evicted
    // 3. Entries are restored manually or via autorestore (both before and
    //    after eviction)
    // 4. Entries are updated or deleted in the same TX that restores them
    // 5. Entries are restored then deleted, then recreated by another TX in
    //    the same ledger.

    auto test = [](Config& cfg) {
        TmpDirManager tdm(std::string("soroban-storage-meta-") +
                          binToHex(randomBytes(8)));
        TmpDir td = tdm.tmpDir("soroban-meta-ok");
        std::string metaPath = td.getName() + "/stream.xdr";

        cfg.METADATA_OUTPUT_STREAM = metaPath;
        auto restoreCost = 20'167;
        if (protocolVersionIsBefore(cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION,
                                    ProtocolVersion::V_23))
        {
            restoreCost -= 119;
        }

        SorobanTest test(cfg, true, [](SorobanNetworkConfig& cfg) {
            cfg.mStateArchivalSettings.minPersistentTTL = 16;
            cfg.mStateArchivalSettings.minTemporaryTTL = 16;
            cfg.mStateArchivalSettings.startingEvictionScanLevel = 2;
        });
        ContractStorageTestClient client(test);
        auto acc1 = test.getRoot().create(
            "acc1", test.getApp().getLedgerManager().getLastMinBalance(2));
        auto const& contractKeys = client.getContract().getKeys();

        auto getMetaForLedger =
            [&metaPath](
                uint32_t ledgerSeq) -> std::optional<LedgerCloseMetaFrame> {
            XDRInputFileStream in;
            in.open(metaPath);
            LedgerCloseMeta lcm;

            while (in.readOne(lcm))
            {
                LedgerCloseMetaFrame frame(lcm);
                if (frame.getLedgerHeader().ledgerSeq == ledgerSeq)
                {
                    return frame;
                }
            }
            return std::nullopt;
        };

        auto verifyEvictions = [&](uint32_t ledgerSeq,
                                   xdr::xvector<LedgerKey> expectedKeys) {
            auto frame = getMetaForLedger(ledgerSeq);
            REQUIRE(frame.has_value());

            auto evictedKeys = frame->getEvictedKeys();
            std::sort(evictedKeys.begin(), evictedKeys.end());
            std::sort(expectedKeys.begin(), expectedKeys.end());
            REQUIRE(evictedKeys == expectedKeys);
        };

        auto verifySingleTxMeta =
            [&](uint32_t ledgerSeq,
                std::function<void(TransactionMetaFrame const&)> verify) {
                auto frame = getMetaForLedger(ledgerSeq);
                REQUIRE(frame.has_value());
                REQUIRE(frame->getEvictedKeys().empty());
                REQUIRE(frame->getTransactionResultMetaCount() == 1);

                auto txMeta = frame->getTransactionMeta(0);
                TransactionMetaFrame txFrame(txMeta);
                REQUIRE(txFrame.getNumOperations() == 1);
                verify(txFrame);
            };

        // Used to check meta for ledgers that apply 2 TXs, first a restore TX,
        // then a TX that updates the value or recreates it.
        auto verifyDualTxMeta =
            [&](uint32_t ledgerSeq,
                std::function<void(TransactionMetaFrame const&)> verifyRestore,
                std::function<void(TransactionMetaFrame const&)> verifyUpdate) {
                auto frame = getMetaForLedger(ledgerSeq);
                REQUIRE(frame.has_value());
                REQUIRE(frame->getEvictedKeys().empty());
                REQUIRE(frame->getTransactionResultMetaCount() == 2);

                auto restoreMeta = frame->getTransactionMeta(0);
                auto updateMeta = frame->getTransactionMeta(1);

                verifyRestore(TransactionMetaFrame(restoreMeta));
                verifyUpdate(TransactionMetaFrame(updateMeta));
            };

        // Extend contract instance lifetime to ensure it doesn't expire during
        // test
        test.invokeExtendOp(contractKeys, 10'000);
        auto temporaryLk = client.getContract().getDataKey(
            makeSymbolSCVal("key"), ContractDataDurability::TEMPORARY);
        auto const tempEntryEvictionLedger = 33;

        SECTION("temp entry meta")
        {
            // Create a temporary storage entry that will be used for
            // eviction testing
            auto invocation = client.getContract().prepareInvocation(
                "put_temporary", {makeSymbolSCVal("key"), makeU64SCVal(123)},
                client.writeKeySpec("key", ContractDataDurability::TEMPORARY));
            REQUIRE(invocation.withExactNonRefundableResourceFee().invoke());

            auto expectedLiveUntilLedger =
                test.getLCLSeq() +
                test.getNetworkCfg().stateArchivalSettings().minTemporaryTTL -
                1;
            REQUIRE(test.getTTL(temporaryLk) == expectedLiveUntilLedger);

            // Advance ledgers to just before eviction
            for (uint32_t i = test.getLCLSeq(); i < tempEntryEvictionLedger - 2;
                 ++i)
            {
                closeLedgerOn(test.getApp(), i, 2, 1, 2016);
            }

            REQUIRE(test.getTTL(temporaryLk) == expectedLiveUntilLedger);

            // Verify extend operation is a no-op for expired temporary
            // entries
            test.invokeExtendOp({temporaryLk}, 10'000, 0);
            REQUIRE(test.getTTL(temporaryLk) == expectedLiveUntilLedger);

            // Verify contract function fails when accessing expired entry
            REQUIRE(client.extend("key", ContractDataDurability::TEMPORARY,
                                  10'000,
                                  10'000) == INVOKE_HOST_FUNCTION_TRAPPED);
            REQUIRE(test.getTTL(temporaryLk) == expectedLiveUntilLedger);

            REQUIRE(!test.isEntryLive(temporaryLk, test.getLCLSeq()));

            // Close one more ledger to trigger the eviction
            closeLedgerOn(test.getApp(), tempEntryEvictionLedger, 2, 1, 2016);

            // Verify the entry is deleted from eviction
            {
                LedgerTxn ltx(test.getApp().getLedgerTxnRoot());
                REQUIRE(!ltx.load(temporaryLk));
            }

            xdr::xvector<LedgerKey> expectedEvictedKeys = {
                temporaryLk, getTTLKey(temporaryLk)};
            verifyEvictions(tempEntryEvictionLedger, expectedEvictedKeys);
        }

        SECTION("Create temp entry with same key as an expired entry on "
                "eviction ledger")
        {
            // Verify that we're on the ledger where the entry would get
            // evicted it wasn't recreated.
            for (uint32_t i = test.getLCLSeq(); i < tempEntryEvictionLedger;
                 ++i)
            {
                closeLedgerOn(test.getApp(), i, 2, 1, 2016);
            }

            REQUIRE(client.put("key", ContractDataDurability::TEMPORARY, 234) ==
                    INVOKE_HOST_FUNCTION_SUCCESS);
            {
                LedgerTxn ltx(test.getApp().getLedgerTxnRoot());
                REQUIRE(ltx.load(temporaryLk));
            }

            REQUIRE(test.getLCLSeq() == tempEntryEvictionLedger);

            // Entry is live again
            REQUIRE(test.isEntryLive(temporaryLk, test.getLCLSeq()));

            // Verify that we didn't emit an eviction
            XDRInputFileStream in;
            in.open(metaPath);
            LedgerCloseMeta lcm;
            while (in.readOne(lcm))
            {
                LedgerCloseMetaFrame lcmFrame(lcm);
                REQUIRE(lcmFrame.getEvictedKeys().empty());
            }

            // Check that we have the new value of the entry
            REQUIRE(client.get("key", ContractDataDurability::TEMPORARY, 234) ==
                    INVOKE_HOST_FUNCTION_SUCCESS);
        }

        SECTION("persistent entry meta")
        {
            // For perstent entry restoration, we'll write an entry initially
            // with kOldValue. Some tests will restore and update the value to
            // kUpdatedValue to test autorestore + update scenarios.
            auto const kOldValue = 123;
            auto const kUpdatedValue = 999;
            auto persistentKey = client.getContract().getDataKey(
                makeSymbolSCVal("key"), ContractDataDurability::PERSISTENT);

            auto persistentInvocation = client.getContract().prepareInvocation(
                "put_persistent",
                {makeSymbolSCVal("key"), makeU64SCVal(kOldValue)},
                client.writeKeySpec("key", ContractDataDurability::PERSISTENT));
            REQUIRE(persistentInvocation.withExactNonRefundableResourceFee()
                        .invoke());

            auto const originalLastModifiedLedgerSeq = test.getLCLSeq();

            LedgerEntry persistentLE;
            {
                LedgerTxn ltx(test.getApp().getLedgerTxnRoot());
                auto ltxe = ltx.load(persistentKey);
                REQUIRE(ltxe);
                persistentLE = ltxe.current();
            }

            // Helper for dual-transaction restore+update scenarios. If
            // wasDeletedByFirstTx is true, first TX is expected to autorestore
            // then delete the restored key. Otherwise, first TX is expected to
            // restore the entry. The 2nd transaction is expected to update the
            // key to updatedValue, or delete it if wasDeletedBySecondTx is
            // true.
            auto verifyRestoreAndUpdate = [&](uint32_t updatedValue,
                                              bool restoreRequiresDataWrite,
                                              bool wasDeletedByFirstTx,
                                              bool wasDeletedBySecondTx =
                                                  false) {
                if (wasDeletedByFirstTx && wasDeletedBySecondTx)
                {
                    throw std::runtime_error(
                        "Cannot delete in both first and second transaction");
                }

                auto targetLedger = test.getLCLSeq();

                // If restore does not require a write, the data entry should
                // not be re-written and meta should reflect the old
                // lastModifiedLedgerSeq. TTL is always written on restores.
                uint32_t expectedDataRestoreLastModified =
                    restoreRequiresDataWrite ? targetLedger
                                             : originalLastModifiedLedgerSeq;
                uint32_t expectedTTLRestoreLastModified = targetLedger;

                // First TX will restore the entry and delete it if
                // wasDeletedByFirstTx is true.
                auto verifyRestoreTx = [&](TransactionMetaFrame const&
                                               restoreTx) {
                    auto const& restoreChanges =
                        restoreTx.getLedgerEntryChangesAtOp(0);
                    LedgerKeySet keysToRestore = {persistentKey,
                                                  getTTLKey(persistentKey)};
                    LedgerKeySet keysToDelete =
                        wasDeletedByFirstTx ? keysToRestore : LedgerKeySet{};

                    // Make sure we see init meta changes
                    // (CREATED, RESTORED, STATE) before any other change types
                    // for a given key. We also need to check that all meta
                    // changes for the same key are adjacent to each other.
                    std::unordered_set<LedgerKey> nonInitMetaChanges;
                    std::optional<LedgerKey> nextExpectedKey = std::nullopt;

                    for (auto const& change : restoreChanges)
                    {
                        if (change.type() ==
                            LedgerEntryChangeType::LEDGER_ENTRY_RESTORED)
                        {
                            auto le = change.restored();
                            auto lk = LedgerEntryKey(le);
                            REQUIRE(keysToRestore.erase(lk) == 1);
                            REQUIRE(nonInitMetaChanges.find(lk) ==
                                    nonInitMetaChanges.end());
                            nextExpectedKey = lk;

                            if (lk.type() == CONTRACT_DATA)
                            {
                                REQUIRE(le.data.contractData().val ==
                                        makeU64SCVal(kOldValue));
                                REQUIRE(le.lastModifiedLedgerSeq ==
                                        expectedDataRestoreLastModified);
                            }
                            else
                            {
                                REQUIRE(le.lastModifiedLedgerSeq ==
                                        expectedTTLRestoreLastModified);
                            }
                        }
                        else if (change.type() ==
                                 LedgerEntryChangeType::LEDGER_ENTRY_REMOVED)
                        {
                            auto lk = change.removed();
                            REQUIRE(keysToDelete.erase(lk) == 1);
                            nonInitMetaChanges.emplace(lk);
                            REQUIRE(nextExpectedKey.has_value());
                            REQUIRE(lk == *nextExpectedKey);
                        }
                        else
                        {
                            FAIL();
                        }
                    }

                    REQUIRE(keysToRestore.empty());
                    REQUIRE(keysToDelete.empty());
                };

                // Second TX will recreate the entry if wasDeletedByFirstTx is
                // true, delete it if wasDeletedBySecondTx is true, or update
                // the entry otherwise.
                auto verifyUpdateTx = [&](TransactionMetaFrame const&
                                              updateTx) {
                    LedgerKeySet keysToCreate;
                    LedgerKeySet keysToUpdate;
                    LedgerKeySet keysToDelete;
                    LedgerKeySet expectedStateKeys;

                    if (wasDeletedByFirstTx)
                    {
                        // First TX deleted the entry, so second TX must
                        // recreate it
                        releaseAssert(wasDeletedBySecondTx == false);
                        keysToCreate = {persistentKey,
                                        getTTLKey(persistentKey)};
                    }
                    else if (wasDeletedBySecondTx)
                    {
                        // First TX restored the entry, second TX will delete it
                        keysToDelete = {persistentKey,
                                        getTTLKey(persistentKey)};
                        expectedStateKeys = keysToDelete;
                    }
                    else
                    {
                        // Neither TX deletes - first TX restores, second TX
                        // updates
                        keysToUpdate = {persistentKey};
                        expectedStateKeys = keysToUpdate;
                    }

                    auto const& updateChanges =
                        updateTx.getLedgerEntryChangesAtOp(0);

                    // Make sure we see init meta changes
                    // (CREATED, RESTORED, STATE) before any other change types
                    // for a given key. We also need to check that all meta
                    // changes for the same key are adjacent to each other.
                    std::unordered_set<LedgerKey> nonInitMetaChanges;
                    std::optional<LedgerKey> nextExpectedKey = std::nullopt;

                    for (auto const& change : updateChanges)
                    {
                        if (change.type() ==
                            LedgerEntryChangeType::LEDGER_ENTRY_UPDATED)
                        {
                            auto le = change.updated();
                            auto lk = LedgerEntryKey(le);
                            nonInitMetaChanges.emplace(lk);
                            REQUIRE(le.data.contractData().val ==
                                    makeU64SCVal(updatedValue));
                            REQUIRE(keysToUpdate.erase(LedgerEntryKey(le)) ==
                                    1);
                            REQUIRE(le.lastModifiedLedgerSeq == targetLedger);

                            REQUIRE(nextExpectedKey.has_value());
                            REQUIRE(lk == *nextExpectedKey);
                        }
                        else if (change.type() ==
                                 LedgerEntryChangeType::LEDGER_ENTRY_CREATED)
                        {
                            auto le = change.created();
                            auto lk = LedgerEntryKey(le);
                            REQUIRE(nonInitMetaChanges.find(lk) ==
                                    nonInitMetaChanges.end());
                            if (LedgerEntryKey(le).type() == CONTRACT_DATA)
                            {
                                REQUIRE(le.data.contractData().val ==
                                        makeU64SCVal(updatedValue));
                            }

                            REQUIRE(keysToCreate.erase(LedgerEntryKey(le)) ==
                                    1);
                            REQUIRE(le.lastModifiedLedgerSeq == targetLedger);
                            nextExpectedKey = lk;
                        }
                        else if (change.type() ==
                                 LedgerEntryChangeType::LEDGER_ENTRY_STATE)
                        {
                            auto le = change.state();
                            auto lk = LedgerEntryKey(le);
                            REQUIRE(nonInitMetaChanges.find(lk) ==
                                    nonInitMetaChanges.end());
                            REQUIRE(expectedStateKeys.erase(
                                        LedgerEntryKey(le)) == 1);
                            if (LedgerEntryKey(le).type() == CONTRACT_DATA)
                            {
                                REQUIRE(le.lastModifiedLedgerSeq ==
                                        expectedDataRestoreLastModified);
                                REQUIRE(le.data.contractData().val ==
                                        makeU64SCVal(kOldValue));
                            }
                            else
                            {
                                // We should only see TTL state here if we are
                                // deleting the entry.
                                REQUIRE(wasDeletedBySecondTx);
                                REQUIRE(le.lastModifiedLedgerSeq ==
                                        expectedTTLRestoreLastModified);
                            }
                            nextExpectedKey = lk;
                        }
                        else if (change.type() ==
                                 LedgerEntryChangeType::LEDGER_ENTRY_REMOVED)
                        {
                            auto lk = change.removed();
                            nonInitMetaChanges.emplace(lk);
                            REQUIRE(keysToDelete.erase(lk) == 1);
                            REQUIRE(nextExpectedKey.has_value());
                            REQUIRE(lk == *nextExpectedKey);
                        }
                        else
                        {
                            FAIL();
                        }
                    }

                    // Check that we saw all expected keys
                    REQUIRE(keysToUpdate.empty());
                    REQUIRE(keysToCreate.empty());
                    REQUIRE(keysToDelete.empty());
                };

                verifyDualTxMeta(targetLedger, verifyRestoreTx, verifyUpdateTx);
            };

            // Helper for manual and autorestore TXs. If updatedValue is not
            // nullopt, the TX is an autorestore and updates the entry after
            // restoration. If deleted is true, the TX is an autorestore that
            // deletes the entry after restoration.
            auto verifyRestore = [&](bool restoreRequiresDataWrite,
                                     std::optional<uint32_t> updatedValue =
                                         std::nullopt,
                                     bool deleted = false) {
                if (updatedValue && deleted)
                {
                    throw "Invalid test case: updatedValue and deleted cannot "
                          "both be set";
                }

                // If restore does not require a write, the data entry should
                // not be re-written and meta should reflect the old
                // lastModifiedLedgerSeq. TTL is always written on restores.
                auto targetLedger = test.getLCLSeq();
                uint32_t expectedDataLastModified =
                    restoreRequiresDataWrite ? targetLedger
                                             : originalLastModifiedLedgerSeq;
                uint32_t expectedTTLLastModified = targetLedger;

                auto verifySingleTx = [&](TransactionMetaFrame const& txFrame) {
                    auto const& changes = txFrame.getLedgerEntryChangesAtOp(0);

                    LedgerKeySet keysToRestore = {persistentKey,
                                                  getTTLKey(persistentKey)};
                    LedgerKeySet keysToUpdate =
                        updatedValue ? LedgerKeySet{persistentKey}
                                     : LedgerKeySet{};
                    LedgerKeySet keysToDelete =
                        deleted ? keysToRestore : LedgerKeySet{};

                    // Make sure we see init meta changes
                    // (CREATED, RESTORED, STATE) before any other change types
                    // for a given key. We also need to check that all meta
                    // changes for the same key are adjacent to each other.
                    std::unordered_set<LedgerKey> nonInitMetaChanges;
                    std::optional<LedgerKey> nextExpectedKey = std::nullopt;

                    for (auto const& change : changes)
                    {
                        if (change.type() ==
                            LedgerEntryChangeType::LEDGER_ENTRY_RESTORED)
                        {
                            auto le = change.restored();
                            auto lk = LedgerEntryKey(le);
                            REQUIRE(nonInitMetaChanges.find(lk) ==
                                    nonInitMetaChanges.end());
                            REQUIRE(keysToRestore.erase(lk) == 1);

                            if (lk.type() == CONTRACT_DATA)
                            {
                                REQUIRE(le.lastModifiedLedgerSeq ==
                                        expectedDataLastModified);
                            }
                            else
                            {
                                REQUIRE(le.lastModifiedLedgerSeq ==
                                        expectedTTLLastModified);
                            }
                            nextExpectedKey = lk;
                        }
                        else if (change.type() ==
                                 LedgerEntryChangeType::LEDGER_ENTRY_UPDATED)
                        {
                            REQUIRE(updatedValue.has_value());
                            auto le = change.updated();
                            auto lk = LedgerEntryKey(le);
                            nonInitMetaChanges.emplace(lk);
                            REQUIRE(le.data.contractData().val ==
                                    makeU64SCVal(*updatedValue));
                            REQUIRE(keysToUpdate.erase(lk) == 1);
                            REQUIRE(nextExpectedKey.has_value());
                            REQUIRE(lk == *nextExpectedKey);
                        }
                        else if (change.type() ==
                                 LedgerEntryChangeType::LEDGER_ENTRY_REMOVED)
                        {
                            auto lk = change.removed();
                            nonInitMetaChanges.emplace(lk);
                            REQUIRE(keysToDelete.erase(lk) == 1);
                            REQUIRE(nextExpectedKey.has_value());
                            REQUIRE(lk == *nextExpectedKey);
                        }
                        else
                        {
                            FAIL();
                        }
                    }

                    REQUIRE(keysToRestore.empty());
                    REQUIRE(keysToUpdate.empty());
                    REQUIRE(keysToDelete.empty());
                };

                verifySingleTxMeta(targetLedger, verifySingleTx);
            };

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

            auto createRestoreTx = [&](const LedgerKey& keyToRestore,
                                       TestAccount* sourceAccount = nullptr) {
                SorobanResources resources;
                resources.footprint.readWrite = {keyToRestore};
                resources.instructions = 0;
                resources.diskReadBytes = 10'000;
                resources.writeBytes = 10'000;

                auto resourceFee = 300'000 + 40'000;
                return test.createRestoreTx(resources, 1'000, resourceFee,
                                            sourceAccount);
            };

            // Helper to run common restore test cases (used by both evicted
            // and non-evicted sections)
            auto runRestoreTestCases = [&](bool wasEvicted) {
                SECTION("manual restore")
                {
                    test.invokeRestoreOp({persistentKey}, restoreCost);
                    // RestoreOp of non-evicted state is the only time restores
                    // don't rewrite data entry
                    verifyRestore(/*restoreRequiresDataWrite=*/wasEvicted);
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
                        verifyRestore(/*restoreRequiresDataWrite=*/true);
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
                                {makeSymbolSCVal("key"),
                                 makeU64SCVal(kUpdatedValue)},
                                spec, /* addContractKeys */ false);
                        REQUIRE(invocation.withExactNonRefundableResourceFee()
                                    .invoke());
                        verifyRestore(/*restoreRequiresDataWrite=*/true,
                                      kUpdatedValue);
                    }
                }

                SECTION("restore, update value in next tx")
                {
                    auto spec = client.defaultSpecWithoutFootprint()
                                    .setReadWriteFootprint({persistentKey})
                                    .setReadOnlyFootprint(
                                        client.getContract().getKeys())
                                    .setReadBytes(5000)
                                    .setWriteBytes(5000)
                                    .setRefundableResourceFee(1'000'000);

                    auto restoreTx = createRestoreTx(persistentKey);

                    auto updateTx =
                        client.getContract()
                            .prepareInvocation("put_persistent",
                                               {makeSymbolSCVal("key"),
                                                makeU64SCVal(kUpdatedValue)},
                                               spec, false)
                            .withExactNonRefundableResourceFee()
                            .createTx(&acc1);

                    SECTION("same stage")
                    {
                        closeLedger(test.getApp(), {restoreTx, updateTx},
                                    /*strictOrder=*/true);
                        verifyRestoreAndUpdate(
                            kUpdatedValue,
                            /*restoreRequiresDataWrite=*/wasEvicted,
                            /*wasDeletedByFirstTx=*/false,
                            /*wasDeletedBySecondTx=*/false);
                    }

                    if (protocolVersionStartsFrom(
                            cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION,
                            PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION))
                    {
                        SECTION("across stages")
                        {
                            closeLedger(test.getApp(), {restoreTx, updateTx},
                                        {{{0}}, {{1}}});
                            verifyRestoreAndUpdate(
                                kUpdatedValue,
                                /*restoreRequiresDataWrite=*/wasEvicted,
                                /*wasDeletedByFirstTx=*/false,
                                /*wasDeletedBySecondTx=*/false);
                        }
                    }
                }

                SECTION("autorestore with delete")
                {
                    if (protocolVersionStartsFrom(
                            cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION,
                            AUTO_RESTORE_PROTOCOL_VERSION))
                    {
                        auto invocation =
                            client.getContract().prepareInvocation(
                                "del_persistent", {makeSymbolSCVal("key")},
                                spec, /* addContractKeys */ false);
                        REQUIRE(invocation.withExactNonRefundableResourceFee()
                                    .invoke());
                        verifyRestore(/*restoreRequiresDataWrite=*/true,
                                      std::nullopt, /*deleted=*/true);
                    }
                }

                SECTION("autorestore with deleted, then recreate with new TX")
                {
                    if (protocolVersionStartsFrom(
                            cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION,
                            AUTO_RESTORE_PROTOCOL_VERSION))
                    {
                        auto deleteInvocation =
                            client.getContract().prepareInvocation(
                                "del_persistent", {makeSymbolSCVal("key")},
                                spec, /* addContractKeys */ false);
                        auto deleteTx = deleteInvocation.createTx();

                        auto createInvocation =
                            client.getContract().prepareInvocation(
                                "put_persistent",
                                {makeSymbolSCVal("key"),
                                 makeU64SCVal(kUpdatedValue)},
                                spec, /* addContractKeys */ false);
                        auto createTx = createInvocation.createTx(&acc1);

                        SECTION("same stage")
                        {
                            closeLedger(test.getApp(), {deleteTx, createTx},
                                        /*strictOrder=*/true);

                            verifyRestoreAndUpdate(
                                kUpdatedValue,
                                /*restoreRequiresDataWrite=*/true,
                                /*wasDeletedByFirstTx=*/true,
                                /*wasDeletedBySecondTx=*/false);
                        }

                        SECTION("across stages")
                        {
                            closeLedger(test.getApp(), {deleteTx, createTx},
                                        {{{0}}, {{1}}});

                            verifyRestoreAndUpdate(
                                kUpdatedValue,
                                /*restoreRequiresDataWrite=*/true,
                                /*wasDeletedByFirstTx=*/true,
                                /*wasDeletedBySecondTx=*/false);
                        }
                    }
                }

                SECTION("restore, delete value in next tx")
                {
                    auto spec = client.defaultSpecWithoutFootprint()
                                    .setReadWriteFootprint({persistentKey})
                                    .setReadOnlyFootprint(
                                        client.getContract().getKeys())
                                    .setReadBytes(5000)
                                    .setWriteBytes(5000)
                                    .setRefundableResourceFee(1'000'000);

                    auto restoreTx = createRestoreTx(persistentKey);

                    auto deleteTx =
                        client.getContract()
                            .prepareInvocation("del_persistent",
                                               {makeSymbolSCVal("key")}, spec,
                                               false)
                            .withExactNonRefundableResourceFee()
                            .createTx(&acc1);

                    SECTION("same stage")
                    {
                        closeLedger(test.getApp(), {restoreTx, deleteTx},
                                    /*strictOrder=*/true);
                        verifyRestoreAndUpdate(
                            kUpdatedValue,
                            /*restoreRequiresDataWrite=*/wasEvicted,
                            /*wasDeletedByFirstTx=*/false,
                            /*wasDeletedBySecondTx=*/true);
                    }

                    if (protocolVersionStartsFrom(
                            cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION,
                            PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION))
                    {
                        SECTION("across stages")
                        {
                            closeLedger(test.getApp(), {restoreTx, deleteTx},
                                        {{{0}}, {{1}}});
                            verifyRestoreAndUpdate(
                                kUpdatedValue,
                                /*restoreRequiresDataWrite=*/wasEvicted,
                                /*wasDeletedByFirstTx=*/false,
                                /*wasDeletedBySecondTx=*/true);
                        }
                    }
                }

                SECTION("autorestore, delete value in next tx")
                {
                    if (protocolVersionStartsFrom(
                            cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION,
                            AUTO_RESTORE_PROTOCOL_VERSION))
                    {
                        auto autorestoreInvocation =
                            client.getContract().prepareInvocation(
                                "has_persistent", {makeSymbolSCVal("key")},
                                spec, /* addContractKeys */ false);
                        auto autorestoreTx =
                            autorestoreInvocation
                                .withExactNonRefundableResourceFee()
                                .createTx();

                        auto deleteSpec =
                            client.defaultSpecWithoutFootprint()
                                .setReadWriteFootprint({persistentKey})
                                .setReadOnlyFootprint(
                                    client.getContract().getKeys())
                                .setReadBytes(5000)
                                .setWriteBytes(5000)
                                .setRefundableResourceFee(1'000'000);

                        auto deleteInvocation =
                            client.getContract().prepareInvocation(
                                "del_persistent", {makeSymbolSCVal("key")},
                                deleteSpec, /* addContractKeys */ false);
                        auto deleteTx =
                            deleteInvocation.withExactNonRefundableResourceFee()
                                .createTx(&acc1);

                        SECTION("same stage")
                        {
                            closeLedger(test.getApp(),
                                        {autorestoreTx, deleteTx},
                                        /*strictOrder=*/true);

                            verifyRestoreAndUpdate(
                                kUpdatedValue,
                                /*restoreRequiresDataWrite=*/true,
                                /*wasDeletedByFirstTx=*/false,
                                /*wasDeletedBySecondTx=*/true);
                        }

                        SECTION("across stages")
                        {
                            closeLedger(test.getApp(),
                                        {autorestoreTx, deleteTx},
                                        {{{0}}, {{1}}});

                            verifyRestoreAndUpdate(
                                kUpdatedValue,
                                /*restoreRequiresDataWrite=*/true,
                                /*wasDeletedByFirstTx=*/false,
                                /*wasDeletedBySecondTx=*/true);
                        }
                    }
                }
            };

            // First, check restoration meta in non-evicted case. This should
            // be identical to meta in the evicted case.
            SECTION("restore expired but not evicted")
            {
                runRestoreTestCases(/*wasEvicted=*/false);
            }

            SECTION("entry evicted")
            {
                // Close ledgers until entry is evicted
                auto evictionLedger = 33;
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
                else
                {
                    LedgerTxn ltx(test.getApp().getLedgerTxnRoot());
                    REQUIRE(ltx.load(persistentKey));
                }

                SECTION("eviction meta")
                {
                    // Only support persistent eviction meta >= p23
                    if (protocolVersionStartsFrom(
                            cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION,
                            LiveBucket::
                                FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION))
                    {
                        xdr::xvector<LedgerKey> expectedEvictedKeys = {
                            persistentKey, getTTLKey(persistentKey)};
                        verifyEvictions(evictionLedger, expectedEvictedKeys);
                        runRestoreTestCases(/*wasEvicted=*/true);
                    }
                    else
                    {
                        // For older protocols, just verify no evictions
                        // occurred
                        auto frame = getMetaForLedger(evictionLedger);
                        REQUIRE(frame.has_value());
                        REQUIRE(frame->getEvictedKeys().empty());
                    }
                }
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
    auto stateArchivalSettings = test.getNetworkCfg().stateArchivalSettings();

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
            sorobanResourceFee(test.getApp(), restoreResources, 400, 0,
                               std::nullopt, /*isRestoreFootprintOp=*/true);
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
            cfg.mStateArchivalSettings.startingEvictionScanLevel =
                evict ? 1 : 5;
            cfg.mStateArchivalSettings.minPersistentTTL =
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

            SorobanResources extendResources;
            extendResources.footprint.readOnly = {lk};
            extendResources.instructions = 0;
            extendResources.diskReadBytes = 10'000;

            auto bumpLedgers = 10'000;
            auto resourceFee = 300'000 + 40'000;
            auto extendTx =
                test.createExtendOpTx(extendResources, bumpLedgers, 1'000,
                                      resourceFee, &extendSrcAccount);

            // A delete that autorestores
            auto delSrcAccount = test.getRoot().create("del", 500000000);
            auto delSpec =
                client.writeKeySpec("key", ContractDataDurability::PERSISTENT)
                    .setArchivedIndexes({0});
            auto inv = client.getContract().prepareInvocation(
                "del_persistent", {makeSymbolSCVal("key")}, delSpec);
            auto delTx = inv.withExactNonRefundableResourceFee().createTx(
                &delSrcAccount);

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

                SECTION("same stage")
                {
                    // Restore and extend in the same stage
                    auto r = closeLedger(test.getApp(), {restoreTx, extendTx},
                                         /*strictOrder=*/true);
                    REQUIRE(r.results.size() == 2);
                    checkTx(0, r, txSUCCESS);
                    checkTx(1, r, txSUCCESS);

                    // Require that the restore went through and the entry could
                    // be bumped in the subsequent TX
                    REQUIRE(test.getTTL(lk) == bumpLedgers + test.getLCLSeq());
                }
                SECTION("across stages")
                {
                    auto r = closeLedger(test.getApp(), {restoreTx, extendTx},
                                         {{{0}}, {{1}}});
                    REQUIRE(r.results.size() == 2);
                    checkTx(0, r, txSUCCESS);
                    checkTx(1, r, txSUCCESS);

                    // Require that the restore went through and the entry could
                    // be bumped in the subsequent TX
                    REQUIRE(test.getTTL(lk) == bumpLedgers + test.getLCLSeq());
                }
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

                SECTION("same stage")
                {
                    auto r = closeLedger(test.getApp(), {writeTx, extendTx},
                                         /*strictOrder=*/true);
                    REQUIRE(r.results.size() == 2);
                    checkTx(0, r, txSUCCESS);
                    checkTx(1, r, txSUCCESS);

                    // Require that the write TX restored the entry which could
                    // be bumped in the subsequent TX
                    REQUIRE(test.getTTL(lk) == bumpLedgers + test.getLCLSeq());
                }
                SECTION("across stages")
                {
                    auto r = closeLedger(test.getApp(), {writeTx, extendTx},
                                         {{{0}}, {{1}}});
                    REQUIRE(r.results.size() == 2);
                    checkTx(0, r, txSUCCESS);
                    checkTx(1, r, txSUCCESS);
                    // Require that the write TX restored the entry which could
                    // be bumped in the subsequent TX
                    REQUIRE(test.getTTL(lk) == bumpLedgers + test.getLCLSeq());
                }
            }
            SECTION("autorestore then delete")
            {
                auto r = closeLedger(test.getApp(), {delTx});
                REQUIRE(r.results.size() == 1);

                checkTx(0, r, txSUCCESS);

                REQUIRE(client.has("key", ContractDataDurability::PERSISTENT,
                                   false) == INVOKE_HOST_FUNCTION_SUCCESS);
            }

            SECTION("autorestore, delete, then has")
            {
                auto hasInvocation = client.getContract().prepareInvocation(
                    "has_persistent", {makeSymbolSCVal("key")},
                    client.readKeySpec("key",
                                       ContractDataDurability::PERSISTENT));

                auto hasTx = hasInvocation.withExactNonRefundableResourceFee()
                                 .createTx();

                SECTION("same stage")
                {
                    auto r = closeLedger(test.getApp(), {delTx, hasTx},
                                         /*strictOrder=*/true);
                    REQUIRE(r.results.size() == 2);

                    checkTx(0, r, txSUCCESS);
                    checkTx(1, r, txSUCCESS);

                    auto const& txMeta = TransactionMetaFrame(
                        test.getLastLcm().getTransactionMeta(1));
                    REQUIRE(txMeta.getReturnValue().b() == false);

                    REQUIRE(client.has("key",
                                       ContractDataDurability::PERSISTENT,
                                       false) == INVOKE_HOST_FUNCTION_SUCCESS);
                }
                SECTION("across stages")
                {
                    auto r = closeLedger(test.getApp(), {delTx, hasTx},
                                         {{{0}}, {{1}}});
                    REQUIRE(r.results.size() == 2);

                    checkTx(0, r, txSUCCESS);
                    checkTx(1, r, txSUCCESS);

                    auto const& txMeta = TransactionMetaFrame(
                        test.getLastLcm().getTransactionMeta(1));
                    REQUIRE(txMeta.getReturnValue().b() == false);

                    REQUIRE(client.has("key",
                                       ContractDataDurability::PERSISTENT,
                                       false) == INVOKE_HOST_FUNCTION_SUCCESS);
                }
            }

            SECTION("autorestore, delete, then create")
            {
                auto writeInvocation = client.getContract().prepareInvocation(
                    "put_persistent",
                    {makeSymbolSCVal("key"), makeU64SCVal(200)},
                    client.writeKeySpec("key",
                                        ContractDataDurability::PERSISTENT));

                auto writeTx =
                    writeInvocation.withExactNonRefundableResourceFee()
                        .createTx();

                SECTION("same stage")
                {
                    auto r = closeLedger(test.getApp(), {delTx, writeTx},
                                         /*strictOrder=*/true);
                    REQUIRE(r.results.size() == 2);

                    checkTx(0, r, txSUCCESS);
                    checkTx(1, r, txSUCCESS);

                    REQUIRE(client.has("key",
                                       ContractDataDurability::PERSISTENT,
                                       true) == INVOKE_HOST_FUNCTION_SUCCESS);
                }
                SECTION("across stages")
                {
                    auto r = closeLedger(test.getApp(), {delTx, writeTx},
                                         {{{0}}, {{1}}});
                    REQUIRE(r.results.size() == 2);

                    checkTx(0, r, txSUCCESS);
                    checkTx(1, r, txSUCCESS);

                    REQUIRE(client.has("key",
                                       ContractDataDurability::PERSISTENT,
                                       true) == INVOKE_HOST_FUNCTION_SUCCESS);
                }
            }

            SECTION("restore, delete, restore")
            {
                auto restore1SrcAccount =
                    test.getRoot().create("restore1", 500000000);
                auto restore2SrcAccount =
                    test.getRoot().create("restore2", 500000000);

                SorobanResources restoreResources;
                restoreResources.footprint.readWrite = {lk};
                restoreResources.instructions = 0;
                restoreResources.diskReadBytes = 10'000;
                restoreResources.writeBytes = 10'000;

                auto restoreTx1 = test.createRestoreTx(
                    restoreResources, 1'000, 400'000, &restore1SrcAccount);

                auto restoreTx2 = test.createRestoreTx(
                    restoreResources, 1'000, 400'000, &restore2SrcAccount);

                SECTION("same stage")
                {
                    auto r = closeLedger(test.getApp(),
                                         {restoreTx1, delTx, restoreTx2},
                                         /*strictOrder=*/true);
                    REQUIRE(r.results.size() == 3);

                    checkTx(0, r, txSUCCESS);
                    checkTx(1, r, txSUCCESS);
                    checkTx(2, r, txSUCCESS);

                    REQUIRE(client.has("key",
                                       ContractDataDurability::PERSISTENT,
                                       false) == INVOKE_HOST_FUNCTION_SUCCESS);
                }
                SECTION("across stages")
                {
                    auto r = closeLedger(test.getApp(),
                                         {restoreTx1, delTx, restoreTx2},
                                         {{{0}}, {{1}}, {{2}}});
                    REQUIRE(r.results.size() == 3);

                    checkTx(0, r, txSUCCESS);
                    checkTx(1, r, txSUCCESS);
                    checkTx(2, r, txSUCCESS);

                    REQUIRE(client.has("key",
                                       ContractDataDurability::PERSISTENT,
                                       false) == INVOKE_HOST_FUNCTION_SUCCESS);
                }
                SECTION("delete in same stage as first restore")
                {
                    auto r = closeLedger(test.getApp(),
                                         {restoreTx1, delTx, restoreTx2},
                                         {{{0, 1}}, {{2}}});
                    REQUIRE(r.results.size() == 3);

                    checkTx(0, r, txSUCCESS);
                    checkTx(1, r, txSUCCESS);
                    checkTx(2, r, txSUCCESS);

                    REQUIRE(client.has("key",
                                       ContractDataDurability::PERSISTENT,
                                       false) == INVOKE_HOST_FUNCTION_SUCCESS);
                }
                SECTION("delete in same stage as second restore")
                {
                    auto r = closeLedger(test.getApp(),
                                         {restoreTx1, delTx, restoreTx2},
                                         {{{0}}, {{1, 2}}});
                    REQUIRE(r.results.size() == 3);

                    checkTx(0, r, txSUCCESS);
                    checkTx(1, r, txSUCCESS);
                    checkTx(2, r, txSUCCESS);

                    REQUIRE(client.has("key",
                                       ContractDataDurability::PERSISTENT,
                                       false) == INVOKE_HOST_FUNCTION_SUCCESS);
                }
            }

            SECTION("failed auto restores and manual restores across stages")
            {
                auto autoRestoreSrcAccount =
                    test.getRoot().create("autoRestore", 500000000);
                auto manualRestoreSrcAccount =
                    test.getRoot().create("manualRestore", 500000000);
                auto manualRestore2SrcAccount =
                    test.getRoot().create("manualRestore2", 500000000);

                // Create an auto restore transaction with intentionally low
                // refundable fee
                auto autoRestoreSpec =
                    client
                        .writeKeySpec("key", ContractDataDurability::PERSISTENT)
                        .setArchivedIndexes({0})
                        .setRefundableResourceFee(79084); // Too low
                auto autoRestoreInvocation =
                    client.getContract().prepareInvocation(
                        "put_persistent",
                        {makeSymbolSCVal("key"), makeU64SCVal(456)},
                        autoRestoreSpec);
                auto autoRestoreTx =
                    autoRestoreInvocation.createTx(&autoRestoreSrcAccount);

                // Create a manual restore transaction with intentionally low
                // refundable fee
                SorobanResources manualRestoreResources;
                manualRestoreResources.footprint.readWrite = {lk};
                manualRestoreResources.instructions = 0;
                manualRestoreResources.diskReadBytes = 10'000;
                manualRestoreResources.writeBytes = 10'000;

                auto manualRestoreTx = test.createRestoreTx(
                    manualRestoreResources, 69'500, 69'477,
                    &manualRestoreSrcAccount); // resourceFee = 1 (too low)
                auto manualRestoreTx2 = test.createRestoreTx(
                    manualRestoreResources, 200'000, 100'000,
                    &manualRestore2SrcAccount); // resourceFee = 1 (too low)

                SECTION("two failed restores across stages")
                {
                    auto r = closeLedger(test.getApp(),
                                         {autoRestoreTx, manualRestoreTx},
                                         {{{0}}, {{1}}});
                    REQUIRE(r.results.size() == 2);

                    checkTx(0, r, txFAILED);
                    checkTx(1, r, txFAILED);

                    REQUIRE(r.results[0]
                                .result.result.results()[0]
                                .tr()
                                .invokeHostFunctionResult()
                                .code() ==
                            INVOKE_HOST_FUNCTION_INSUFFICIENT_REFUNDABLE_FEE);

                    REQUIRE(r.results[1]
                                .result.result.results()[0]
                                .tr()
                                .restoreFootprintResult()
                                .code() ==
                            RESTORE_FOOTPRINT_INSUFFICIENT_REFUNDABLE_FEE);

                    // Verify the key is still archived after failed restore
                    // attempts
                    REQUIRE(client.has("key",
                                       ContractDataDurability::PERSISTENT,
                                       std::nullopt) ==
                            INVOKE_HOST_FUNCTION_ENTRY_ARCHIVED);
                }
                SECTION("two failed restores and a successful restore in the "
                        "same stage")
                {
                    // Test failed transactions across stages
                    auto r = closeLedger(
                        test.getApp(),
                        {autoRestoreTx, manualRestoreTx, manualRestoreTx2},
                        {{{0, 1, 2}}});
                    REQUIRE(r.results.size() == 3);

                    // Both transactions should fail due to insufficient
                    // refundable fee
                    checkTx(0, r, txFAILED);
                    checkTx(1, r, txFAILED);
                    checkTx(2, r, txSUCCESS);

                    REQUIRE(r.results[0]
                                .result.result.results()[0]
                                .tr()
                                .invokeHostFunctionResult()
                                .code() ==
                            INVOKE_HOST_FUNCTION_INSUFFICIENT_REFUNDABLE_FEE);

                    REQUIRE(r.results[1]
                                .result.result.results()[0]
                                .tr()
                                .restoreFootprintResult()
                                .code() ==
                            RESTORE_FOOTPRINT_INSUFFICIENT_REFUNDABLE_FEE);

                    REQUIRE(client.has("key",
                                       ContractDataDurability::PERSISTENT,
                                       true) == INVOKE_HOST_FUNCTION_SUCCESS);
                }
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
    cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS = true;
    SorobanTest test(cfg, true, [](SorobanNetworkConfig& cfg) {
        cfg.mStateArchivalSettings.minPersistentTTL =
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
    auto const refundableRestoreCost = 60'711;
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

        auto result = test.invokeTx(autorestoreTx);
        REQUIRE(isSuccessResult(result));

        // Check that the refundable fee charged matches what we expect for
        // restoration
        auto const eventSize = 4;
        test.checkRefundableFee(initialBalance, autorestoreTx,
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
            test.invokeRestoreOp(contractKeys, 40'546);

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
    cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS = true;
    SorobanTest test(cfg, true, [](SorobanNetworkConfig& cfg) {
        cfg.mStateArchivalSettings.minPersistentTTL =
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
    // These costs are based on the exact amount of bump ledgers above. They
    // should match between the manual restore and autorestore cases.
    auto const costToRestore1KB = 20'342;
    auto const costToBump = 17'702'779;
    auto const costToBumpAfterResize = 50'624'589;

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

        // The restored entry has been already bumped by
        // `MINIMUM_PERSISTENT_ENTRY_LIFETIME` during the restoration, so in
        // order to extend it by exactly `bumpLedgers` we add the current entry
        // TTL to the bump amount.
        // Notice the subtle (LCL + 1) here - the TTL is going to be extended
        // in the next ledger, not the last one.
        auto const manualRestoreBumpLedgers =
            bumpLedgers + (test.getTTL(lk) - (test.getLCLSeq() + 1));
        SECTION("extend only")
        {
            test.invokeExtendOp({lk}, manualRestoreBumpLedgers, costToBump);
        }

        SECTION("extend after resize")
        {
            REQUIRE(isSuccess(
                client.resizeStorageAndExtend("key", resizeKb, 0, 0)));
            // We've just closed another ledger above, so need to decrease the
            // bump amount respectively.
            test.invokeExtendOp({lk}, manualRestoreBumpLedgers - 1,
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
                               .setRefundableResourceFee(100'000'000);

    auto invokeAndCheck = [&](auto invocation,
                              auto expectedRefundableFeeCharged, auto eventSize,
                              auto expectedLiveUntilLedger) {
        auto autorestoreTx =
            invocation.withExactNonRefundableResourceFee().createTx();

        auto initialBalance = test.getAccountBalance();

        auto result = test.invokeTx(autorestoreTx);
        REQUIRE(isSuccessResult(result));

        // Check that the refundable fee charged matches what we expect
        // for restoration
        test.checkRefundableFee(initialBalance, autorestoreTx,
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
    // When we autorestore an entry, we consider it to just have been created
    // and thus the current ledger is included in the rent payment. So we
    // subtract it from the bump ledgers in order to pay rent for exactly
    // `bumpLedgers`.
    auto const autoRestoreBumpLedgers = bumpLedgers - 1;
    auto const eventsSize = 4;
    SECTION("autorestore with rent bump")
    {
        // autorestore and extend in the same tx. Rent fees should be
        // identical to a an extension of bumpLedgers
        auto invocation = client.getContract().prepareInvocation(
            "replace_with_bytes_and_extend",
            {makeSymbolSCVal("key"), makeU32(1),
             makeU32(autoRestoreBumpLedgers), makeU32(autoRestoreBumpLedgers)},
            autorestoreSpec);
        invokeAndCheck(invocation, costToBump, eventsSize,
                       test.getLCLSeq() + autoRestoreBumpLedgers);
    }

    SECTION("autorestore with rent bump and resize")
    {
        // autorestore and extend in the same tx
        auto invocation = client.getContract().prepareInvocation(
            "replace_with_bytes_and_extend",
            {makeSymbolSCVal("key"), makeU32(resizeKb),
             makeU32(autoRestoreBumpLedgers), makeU32(autoRestoreBumpLedgers)},
            autorestoreSpec);

        invokeAndCheck(invocation, costToBumpAfterResize, eventsSize,
                       test.getLCLSeq() + autoRestoreBumpLedgers);
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
    // mAverageSorobanStateSize
    modifySorobanNetworkConfig(*app, [](SorobanNetworkConfig& cfg) {
        cfg.mStateArchivalSettings.liveSorobanStateSizeWindowSamplePeriod = 1;
        // These are required to allow for an upgrade of all settings at once.
        cfg.mMaxContractDataEntrySizeBytes = 5000;
        cfg.mLedgerMaxDiskReadBytes = 5000;
        cfg.mLedgerMaxWriteBytes = 5000;
        cfg.mTxMaxDiskReadBytes = 5000;
        cfg.mTxMaxWriteBytes = 5000;
    });

    const int64_t startingBalance =
        app->getLedgerManager().getLastMinBalance(50);

    auto a1 = root->create("A", startingBalance);

    std::vector<TransactionEnvelope> txsToSign;

    auto uploadRes =
        getUploadTx(a1.getPublicKey(), a1.getLastSequenceNumber() + 1, 0);
    txsToSign.emplace_back(uploadRes.first);
    auto const& contractCodeLedgerKey = uploadRes.second;

    auto createRes =
        getCreateTx(a1.getPublicKey(), contractCodeLedgerKey,
                    cfg.NETWORK_PASSPHRASE, a1.getLastSequenceNumber() + 2, 0);
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
                                 upgradeSet, a1.getLastSequenceNumber() + 3, 0);
    txsToSign.emplace_back(invokeRes.first);
    auto const& upgradeSetKey = invokeRes.second;

    closeLedger(*app);

    for (auto& txEnv : txsToSign)
    {
        txEnv.v1().signatures.emplace_back(SignatureUtils::sign(
            a1.getSecretKey(),
            sha256(xdr::xdr_to_opaque(app->getNetworkID(), ENVELOPE_TYPE_TX,
                                      txEnv.v1().tx))));

        auto const& rawTx = TransactionFrameBase::makeTransactionFromWire(
            app->getNetworkID(), txEnv);
        auto tx = TransactionTestFrame::fromTxFrame(rawTx);
        closeLedger(*app, {tx});
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
                        a1.getLastSequenceNumber() + 4, 0);

        auto const& upgradeSetKey2 = invokeRes2.second;

        invokeRes2.first.v1().signatures.emplace_back(SignatureUtils::sign(
            a1.getSecretKey(),
            sha256(xdr::xdr_to_opaque(app->getNetworkID(), ENVELOPE_TYPE_TX,
                                      invokeRes2.first.v1().tx))));

        auto const& txRaw = TransactionFrameBase::makeTransactionFromWire(
            app->getNetworkID(), invokeRes2.first);
        auto txRevertSettings = TransactionTestFrame::fromTxFrame(txRaw);
        closeLedger(*app, {txRevertSettings});

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
                            a1.getLastSequenceNumber() + 3, 0),
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

TEST_CASE("multiple version of same key in a single eviction scan",
          "[archival][soroban]")
{
    // This tests an edge case where an entry is expired, and multiple versions
    // of that same entry will be scanned in the eviction scan. We want to make
    // sure we don't "double evict" the key that gets scanned twice.
    auto cfg = getTestConfig(0);
    cfg.TESTING_SOROBAN_HIGH_LIMIT_OVERRIDE = true;
    cfg.TESTING_MINIMUM_PERSISTENT_ENTRY_LIFETIME = 10;
    cfg.OVERRIDE_EVICTION_PARAMS_FOR_TESTING = true;
    cfg.TESTING_STARTING_EVICTION_SCAN_LEVEL = 1;
    cfg.TESTING_MAX_ENTRIES_TO_ARCHIVE = 100;

    SorobanTest test(cfg, false);
    ContractStorageTestClient client(test);

    // WASM and instance should not expire
    test.invokeExtendOp(client.getContract().getKeys(), 10'000);

    auto lk = client.getContract().getDataKey(
        makeSymbolSCVal("key"), ContractDataDurability::PERSISTENT);
    client.put("key", ContractDataDurability::PERSISTENT, 1);

    // Close ledgers until entry is evicted
    auto evictEntry = [&]() {
        auto evictionLedger =
            test.getLCLSeq() +
            MinimumSorobanNetworkConfig::MINIMUM_PERSISTENT_ENTRY_LIFETIME;
        for (uint32_t ledgerSeq = test.getLCLSeq() + 1;
             ledgerSeq <= evictionLedger; ++ledgerSeq)
        {
            closeLedgerOn(test.getApp(), ledgerSeq, 2, 1, 2016);
        }

        auto hotArchive = test.getApp()
                              .getBucketManager()
                              .getBucketSnapshotManager()
                              .copySearchableHotArchiveBucketListSnapshot();
        REQUIRE(hotArchive->load(lk));
    };

    evictEntry();

    // Restore entry. Two versions of the entry will exist in the BucketList,
    // the restored version and the original, evicted version. Their data is the
    // same, but they have different lastModifiedLedgerSeq and live in different
    // levels of the BucketList.
    test.invokeRestoreOp({lk}, 20166);

    auto bl = test.getApp()
                  .getBucketManager()
                  .getBucketSnapshotManager()
                  .copySearchableLiveBucketListSnapshot();
    auto loadRes = bl->load(lk);
    REQUIRE(loadRes);

    // Evict the entry again. If we "double evict" we'll throw during ledger
    // close.
    REQUIRE_NOTHROW(evictEntry());
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
UnorderedMap<uint32_t /*ledgerSeq*/, LedgerCloseMeta>
readParallelMeta(std::string const& metaPath)
{
    UnorderedMap<uint32_t, LedgerCloseMeta> res;

    XDRInputFileStream in;
    in.open(metaPath);
    LedgerCloseMeta lcm;
    while (in.readOne(lcm))
    {
        // We make the assumption this is only used for parallel soroban, so
        // meta version should be v2
        REQUIRE(lcm.v() == 2);
        auto ledgerSeq = lcm.v2().ledgerHeader.header.ledgerSeq;

        res.emplace(ledgerSeq, lcm);
    }

    return res;
}

TEST_CASE_VERSIONS("source account of first tx is in second txs footprint",
                   "[tx][soroban][parallelapply]")
{
    Config cfg = getTestConfig();
    cfg.EMIT_CLASSIC_EVENTS = true;
    cfg.BACKFILL_STELLAR_ASSET_EVENTS = true;

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);

    for_versions_from(20, *app, [&] {
        SorobanTest test(app);
        AssetContractTestClient assetClient(test, txtest::makeNativeAsset());

        auto ledgerVersion = getLclProtocolVersion(test.getApp());

        const int64_t startingBalance =
            test.getApp().getLedgerManager().getLastMinBalance(50);

        auto a1 = test.getRoot().create("A", startingBalance);
        auto b1 = test.getRoot().create("B", startingBalance);
        auto c1 = test.getRoot().create("C", startingBalance);

        auto b1StartingSeq = b1.loadSequenceNumber();

        auto classicTx = c1.tx({payment(b1, 10)});

        auto b1Addr = makeAccountAddress(b1.getPublicKey());
        auto sorobanTx1 = assetClient.getTransferTx(a1, b1Addr, 50);

        auto wasm = rust_bridge::get_test_wasm_add_i32();
        auto resources =
            defaultUploadWasmResourcesWithoutFootprint(wasm, ledgerVersion);
        auto sorobanTx2 =
            makeSorobanWasmUploadTx(test.getApp(), b1, wasm, resources, 1000);

        std::vector<TransactionFrameBasePtr> txs = {classicTx, sorobanTx1,
                                                    sorobanTx2};
        auto r = closeLedger(test.getApp(), txs, true);
        REQUIRE(r.results.size() == 3);
        checkTx(0, r, txSUCCESS);
        checkTx(1, r, txSUCCESS);
        checkTx(2, r, txSUCCESS);

        REQUIRE(b1.loadSequenceNumber() == b1StartingSeq + 1);
        REQUIRE(b1.getBalance() ==
                startingBalance + 10 + 50 - r.results.at(2).result.feeCharged);
    });
}

TEST_CASE_VERSIONS("non-fee source account is recipient of payment in both "
                   "classic and soroban",
                   "[tx][soroban][parallelapply]")
{
    Config cfg = getTestConfig();
    cfg.EMIT_CLASSIC_EVENTS = true;
    cfg.BACKFILL_STELLAR_ASSET_EVENTS = true;

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);

    for_versions_from(20, *app, [&] {
        SorobanTest test(app);
        AssetContractTestClient assetClient(test, txtest::makeNativeAsset());

        const int64_t startingBalance =
            test.getApp().getLedgerManager().getLastMinBalance(50);

        auto a1 = test.getRoot().create("A", startingBalance);
        auto b1 = test.getRoot().create("B", startingBalance);
        auto c1 = test.getRoot().create("C", startingBalance);

        auto b1Addr = makeAccountAddress(b1.getPublicKey());
        auto sacTx = assetClient.getTransferTx(c1, b1Addr, 50);

        auto classicTx = a1.tx({payment(b1, 10)});

        std::vector<TransactionFrameBasePtr> txs = {classicTx, sacTx};
        auto r = closeLedger(test.getApp(), txs);
        REQUIRE(r.results.size() == 2);
        checkTx(0, r, txSUCCESS);
        checkTx(1, r, txSUCCESS);

        // Make sure both payments are reflected in the balance
        REQUIRE(b1.getBalance() == startingBalance + 10 + 50);
    });
}

TEST_CASE("parallel txs", "[tx][soroban][parallelapply]")
{
    auto cfg = getTestConfig();
    cfg.LEDGER_PROTOCOL_VERSION =
        static_cast<uint32_t>(PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION);
    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
        static_cast<uint32_t>(PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION);
    cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS = true;

    TmpDirManager tdm(std::string("metatest-soroban-") +
                      binToHex(randomBytes(8)));
    TmpDir td = tdm.tmpDir("meta-ok");
    std::string metaPath = td.getName() + "/stream.xdr";

    cfg.METADATA_OUTPUT_STREAM = metaPath;

    SorobanTest test(cfg, true, [](SorobanNetworkConfig& cfg) {
        cfg.mLedgerMaxInstructions = 25'000'000;
        cfg.mTxMaxInstructions = 25'000'000;
        cfg.mLedgerMaxDependentTxClusters = 2;
    });
    ContractStorageTestClient client(test);

    auto& app = test.getApp();
    auto issuerKey = getAccount("issuer");
    Asset idr = makeAsset(issuerKey, "IDR");
    AssetContractTestClient assetClient(test, idr);

    // Wasm and instance should not expire during test
    test.invokeExtendOp(client.getContract().getKeys(), 10'000);
    test.invokeExtendOp(assetClient.getContract().getKeys(), 10'000);

    auto& lm = app.getLedgerManager();

    const int64_t startingBalance = lm.getLastMinBalance(50);

    auto& root = test.getRoot();
    auto a0 = root.create("a0", startingBalance);
    auto a1 = root.create("a1", startingBalance);
    auto a2 = root.create("a2", startingBalance);
    auto a3 = root.create("a3", startingBalance);
    auto a4 = root.create("a4", startingBalance);

    auto issuer = root.create(issuerKey, startingBalance);
    auto a5 = root.create("a5", startingBalance);
    auto a6 = root.create("a6", startingBalance);

    auto a7 = root.create("a7", startingBalance);
    auto a8 = root.create("a8", startingBalance);
    a8.changeTrust(idr, 200);
    a7.changeTrust(idr, 75);
    issuer.pay(a8, idr, 200);

    auto a9 = root.create("a9", startingBalance);
    auto a10 = root.create("a10", startingBalance);
    auto a11 = root.create("a11", startingBalance);

    REQUIRE(client.put("key2", ContractDataDurability::TEMPORARY, 0) ==
            INVOKE_HOST_FUNCTION_SUCCESS);

    REQUIRE(test.getTTL(client.getContract().getDataKey(
                makeSymbolSCVal("key2"), ContractDataDurability::TEMPORARY)) ==
            46);
    REQUIRE(test.getLCLSeq() == 31);

    REQUIRE(client.put("key4", ContractDataDurability::PERSISTENT, 0) ==
            INVOKE_HOST_FUNCTION_SUCCESS);

    REQUIRE(client.put("extendDelete", ContractDataDurability::TEMPORARY, 0) ==
            INVOKE_HOST_FUNCTION_SUCCESS);

    auto& hostFnSuccessMeter =
        app.getMetrics().NewMeter({"soroban", "host-fn-op", "success"}, "call");
    auto& hostFnFailureMeter =
        app.getMetrics().NewMeter({"soroban", "host-fn-op", "failure"}, "call");

    auto successesBefore = hostFnSuccessMeter.count();

    SECTION("basic test")
    {
        auto i1Spec =
            client.writeKeySpec("key1", ContractDataDurability::TEMPORARY);
        auto i1 = client.getContract().prepareInvocation(
            "put_temporary", {makeSymbolSCVal("key1"), makeU64SCVal(123)},
            i1Spec.setInclusionFee(i1Spec.getInclusionFee() + 1));
        auto tx1 = i1.withExactNonRefundableResourceFee().createTx(&a1);

        TransactionEnvelope fbTx1(ENVELOPE_TYPE_TX_FEE_BUMP);
        fbTx1.feeBump().tx.feeSource = toMuxedAccount(a1);
        fbTx1.feeBump().tx.fee = tx1->getEnvelope().v1().tx.fee * 5;

        fbTx1.feeBump().tx.innerTx.type(ENVELOPE_TYPE_TX);
        fbTx1.feeBump().tx.innerTx.v1() = tx1->getEnvelope().v1();

        fbTx1.feeBump().signatures.emplace_back(SignatureUtils::sign(
            a1, sha256(xdr::xdr_to_opaque(test.getApp().getNetworkID(),
                                          ENVELOPE_TYPE_TX_FEE_BUMP,
                                          fbTx1.feeBump().tx))));
        auto feeBumpTx1Frame = TransactionFrameBase::makeTransactionFromWire(
            test.getApp().getNetworkID(), fbTx1);

        // extend key2 using ExtendFootprintTTLOp
        SorobanResources extendResources;
        extendResources.footprint.readOnly = {client.getContract().getDataKey(
            makeSymbolSCVal("key2"), ContractDataDurability::TEMPORARY)};
        extendResources.diskReadBytes = 9'000;
        auto tx2 =
            test.createExtendOpTx(extendResources, 400, 30'000, 500'000, &a2);

        // Rewrite key1 value
        auto i3 = client.getContract().prepareInvocation(
            "put_temporary", {makeSymbolSCVal("key1"), makeU64SCVal(8)},
            client.writeKeySpec("key1", ContractDataDurability::TEMPORARY)
                .setInclusionFee(i1Spec.getInclusionFee() + 3));
        auto tx3 = i3.withExactNonRefundableResourceFee().createTx(&a3);

        // Tx4 should fail due to low refundable fee.
        auto i4Spec =
            client.writeKeySpec("key3", ContractDataDurability::TEMPORARY)
                .setRefundableResourceFee(1)
                .setInclusionFee(i1Spec.getInclusionFee() + 4);
        auto i4 = client.getContract().prepareInvocation(
            "put_temporary", {makeSymbolSCVal("key3"), makeU64SCVal(1)},
            i4Spec);
        auto tx4 = i4.withExactNonRefundableResourceFee().createTx(&a4);

        auto key4Spec =
            client.readKeySpec("key4", ContractDataDurability::PERSISTENT);

        auto i5 = client.getContract().prepareInvocation(
            "extend_persistent",
            {makeSymbolSCVal("key4"), makeU32SCVal(1000), makeU32SCVal(1000)},
            key4Spec.setInclusionFee(i1Spec.getInclusionFee() + 5));
        auto tx5 = i5.withExactNonRefundableResourceFee().createTx(&a5);

        auto i6 = client.getContract().prepareInvocation(
            "extend_persistent",
            {makeSymbolSCVal("key4"), makeU32SCVal(1000), makeU32SCVal(1000)},
            key4Spec.setInclusionFee(i1Spec.getInclusionFee() + 6));
        auto tx6 = i6.withExactNonRefundableResourceFee().createTx(&a6);

        auto i7 = client.getContract().prepareInvocation(
            "extend_temporary",
            {makeSymbolSCVal("key2"), makeU32SCVal(200), makeU32SCVal(200)},
            client.readKeySpec("key2", ContractDataDurability::TEMPORARY)
                .setInclusionFee(i1Spec.getInclusionFee() + 7));
        auto tx7 = i7.withExactNonRefundableResourceFee().createTx(&a9);

        auto i8 = client.getContract().prepareInvocation(
            "extend_temporary",
            {makeSymbolSCVal("key2"), makeU32SCVal(1000), makeU32SCVal(1000)},
            client.readKeySpec("key2", ContractDataDurability::TEMPORARY)
                .setInclusionFee(i1Spec.getInclusionFee() + 8));
        auto tx8 = i8.withExactNonRefundableResourceFee().createTx(&a10);

        auto i9 = client.getContract().prepareInvocation(
            "del_temporary", {makeSymbolSCVal("extendDelete")},
            client
                .writeKeySpec("extendDelete", ContractDataDurability::TEMPORARY)
                .setInclusionFee(i1Spec.getInclusionFee() + 9));
        auto tx9 = i9.withExactNonRefundableResourceFee().createTx(&a11);

        auto a7Addr = makeAccountAddress(a7.getPublicKey());
        auto transferTx1 = assetClient.getTransferTx(a8, a7Addr, 50);

        auto issuerAddr = makeAccountAddress(issuer.getPublicKey());
        auto transferTx2 = assetClient.getTransferTx(issuer, a7Addr, 25);

        std::vector<TransactionFrameBaseConstPtr> sorobanTxs;
        sorobanTxs.emplace_back(feeBumpTx1Frame);
        sorobanTxs.emplace_back(tx2);
        sorobanTxs.emplace_back(tx3);
        sorobanTxs.emplace_back(tx4);
        sorobanTxs.emplace_back(tx5);
        sorobanTxs.emplace_back(tx6);
        sorobanTxs.emplace_back(tx7);
        sorobanTxs.emplace_back(tx8);
        sorobanTxs.emplace_back(tx9);
        sorobanTxs.emplace_back(transferTx1);
        sorobanTxs.emplace_back(transferTx2);
        ParallelSorobanOrder order;
        order.emplace_back();
        order.back().emplace_back();
        auto& clusterOrder = order.back().back();
        clusterOrder.resize(sorobanTxs.size());
        std::iota(clusterOrder.begin(), clusterOrder.end(), 0);
        auto r = closeLedger(test.getApp(), sorobanTxs, order);
        REQUIRE(r.results.size() == sorobanTxs.size());

        // Do a sanity check on tx meta
        auto metaMap = readParallelMeta(metaPath);
        REQUIRE(metaMap.count(test.getLCLSeq()));

        auto const& lcm = metaMap[test.getLCLSeq()];
        REQUIRE(lcm.v2().txProcessing.size() == 11);
        for (auto const& txResultMeta : lcm.v2().txProcessing)
        {
            REQUIRE(txResultMeta.txApplyProcessing.v4().txChangesAfter.empty());

            LedgerCloseMetaFrame lcmFrame(lcm);
            auto const& refundChanges = lcmFrame.getPostTxApplyFeeProcessing(0);
            // Just verify that a refund happened
            REQUIRE(refundChanges.size() == 2);
            REQUIRE(refundChanges[1].updated().data.account().balance >
                    refundChanges[0].state().data.account().balance);
        }

        // One tx should fail due to low fee.
        checkResults(r, sorobanTxs.size() - 1, 1);

        REQUIRE(hostFnSuccessMeter.count() - successesBefore ==
                sorobanTxs.size() -
                    2); // -2 because one tx is expected to fail, and the other
                        // is a extend op not covered by the hostFnSuccessMeter
        REQUIRE(hostFnFailureMeter.count() == 1);

        REQUIRE(r.results[3]
                    .result.result.results()[0]
                    .tr()
                    .invokeHostFunctionResult()
                    .code() ==
                INVOKE_HOST_FUNCTION_INSUFFICIENT_REFUNDABLE_FEE);

        // Make sure key4's bumps were accumulated
        REQUIRE(
            test.getTTL(client.getContract().getDataKey(
                makeSymbolSCVal("key4"), ContractDataDurability::PERSISTENT)) ==
            1'000 + test.getLCLSeq());

        REQUIRE(
            test.getTTL(client.getContract().getDataKey(
                makeSymbolSCVal("key2"), ContractDataDurability::TEMPORARY)) ==
            1000 + test.getLCLSeq());

        REQUIRE(client.get("key1", ContractDataDurability::TEMPORARY, 8) ==
                INVOKE_HOST_FUNCTION_SUCCESS);
        REQUIRE(client.get("key2", ContractDataDurability::TEMPORARY, 0) ==
                INVOKE_HOST_FUNCTION_SUCCESS);
        REQUIRE(client.has("key3", ContractDataDurability::TEMPORARY, false) ==
                INVOKE_HOST_FUNCTION_SUCCESS);
        REQUIRE(client.has("extendDelete", ContractDataDurability::TEMPORARY,
                           false) == INVOKE_HOST_FUNCTION_SUCCESS);

        REQUIRE(a7.getTrustlineBalance(idr) == 75);
        REQUIRE(a8.getTrustlineBalance(idr) == 150);
    }
    SECTION("internal error")
    {
        auto i1Spec =
            client.writeKeySpec("key2", ContractDataDurability::TEMPORARY);
        auto i1 = client.getContract().prepareInvocation(
            "extend_temporary",
            {makeSymbolSCVal("key2"), makeU32SCVal(400), makeU32SCVal(400)},
            i1Spec.setInclusionFee(i1Spec.getInclusionFee() + 1));
        auto tx1 = i1.withExactNonRefundableResourceFee().createTx(&a1);

        // This extension will not be applied due to the internal error
        // triggered through the memo.
        auto i2 = client.getContract().prepareInvocation(
            "extend_temporary",
            {makeSymbolSCVal("key2"), makeU32SCVal(1000), makeU32SCVal(1000)},
            client.readKeySpec("key2", ContractDataDurability::TEMPORARY)
                .setInclusionFee(i1Spec.getInclusionFee() + 2));
        auto tx2 = i2.withExactNonRefundableResourceFee().createTx(
            &a2, "txINTERNAL_ERROR");

        auto r = closeLedger(test.getApp(), {tx1, tx2});
        checkResults(r, 1, 1);
        bool hasInternalError =
            r.results[0].result.result.code() == txINTERNAL_ERROR ||
            r.results[1].result.result.code() == txINTERNAL_ERROR;
        REQUIRE(hasInternalError);

        REQUIRE(
            test.getTTL(client.getContract().getDataKey(
                makeSymbolSCVal("key2"), ContractDataDurability::TEMPORARY)) ==
            400 + test.getLCLSeq());
    }

    auto recreateLowerTtlTest = [&](ContractDataDurability durability) {
        // serialize all transactions
        modifySorobanNetworkConfig(app, [](SorobanNetworkConfig& cfg) {
            cfg.mLedgerMaxInstructions = 100'000'000;
            cfg.mLedgerMaxDependentTxClusters = 1;
        });

        bool isPersistent = durability == ContractDataDurability::PERSISTENT;

        REQUIRE(client.put("recreate", durability, 0) ==
                INVOKE_HOST_FUNCTION_SUCCESS);
        REQUIRE(client.extend("recreate", durability, 10'000, 10'000) ==
                INVOKE_HOST_FUNCTION_SUCCESS);
        REQUIRE(test.getTTL(client.getContract().getDataKey(
                    makeSymbolSCVal("recreate"), durability)) ==
                10'000 + test.getLCLSeq());

        auto writeSpec = client.writeKeySpec("recreate", durability);

        auto i1 = client.getContract().prepareInvocation(
            isPersistent ? "del_persistent" : "del_temporary",
            {makeSymbolSCVal("recreate")},
            writeSpec.setInclusionFee(writeSpec.getInclusionFee() + 1));
        auto tx1 = i1.withExactNonRefundableResourceFee().createTx(&a2);

        auto i2 = client.getContract().prepareInvocation(
            isPersistent ? "put_persistent" : "put_temporary",
            {makeSymbolSCVal("recreate"), makeU64SCVal(5)}, writeSpec);
        auto tx2 = i2.withExactNonRefundableResourceFee().createTx(&a3);

        auto check = [&](auto const& r) {
            REQUIRE(r.results.size() == 2);

            // Make sure the del_temporary tx is first
            REQUIRE(r.results.at(0).transactionHash == tx1->getContentsHash());

            auto metaMap = readParallelMeta(metaPath);
            REQUIRE(metaMap.count(test.getLCLSeq()));

            auto& lcm = metaMap[test.getLCLSeq()];
            normalizeMeta(lcm);
            REQUIRE(lcm.v2().txProcessing.size() == 2);

            // Make sure meta looks looks correct
            // tx1 removes two entries (contract data and ttl), and then tx2
            // creates them.
            auto tx1OpChanges = lcm.v2()
                                    .txProcessing.at(0)
                                    .txApplyProcessing.v4()
                                    .operations.at(0)
                                    .changes;
            REQUIRE(tx1OpChanges.at(0).type() == LEDGER_ENTRY_STATE);
            REQUIRE(tx1OpChanges.at(1).type() == LEDGER_ENTRY_REMOVED);
            REQUIRE(tx1OpChanges.at(2).type() == LEDGER_ENTRY_STATE);
            REQUIRE(tx1OpChanges.at(3).type() == LEDGER_ENTRY_REMOVED);

            auto tx2OpChanges = lcm.v2()
                                    .txProcessing.at(1)
                                    .txApplyProcessing.v4()
                                    .operations.at(0)
                                    .changes;
            REQUIRE(tx2OpChanges.at(0).type() == LEDGER_ENTRY_CREATED);
            REQUIRE(tx2OpChanges.at(0).type() == LEDGER_ENTRY_CREATED);

            auto minTTL = isPersistent ? test.getNetworkCfg()
                                             .stateArchivalSettings()
                                             .minPersistentTTL
                                       : test.getNetworkCfg()
                                             .stateArchivalSettings()
                                             .minTemporaryTTL;
            auto recreatedTTL = minTTL + test.getLCLSeq() - 1;
            REQUIRE(
                tx2OpChanges.at(1).created().data.ttl().liveUntilLedgerSeq ==
                recreatedTTL);

            REQUIRE(test.getTTL(client.getContract().getDataKey(
                        makeSymbolSCVal("recreate"), durability)) ==
                    recreatedTTL);
        };

        SECTION("same stage")
        {
            auto r = closeLedger(test.getApp(), {tx1, tx2}, true);
            check(r);
        }

        SECTION("across stages")
        {
            auto r = closeLedger(test.getApp(), {tx1, tx2}, {{{0}}, {{1}}});
            check(r);
        }
    };

    SECTION("delete and re-create persistent entry with lower TTL")
    {
        recreateLowerTtlTest(ContractDataDurability::PERSISTENT);
    }

    SECTION("delete and re-create temporary entry with lower TTL")
    {
        recreateLowerTtlTest(ContractDataDurability::TEMPORARY);
    }

    auto singleRWMultiROExtensionTest = [&]() {
        // Test increase and then decrease of clusters
        modifySorobanNetworkConfig(app, [](SorobanNetworkConfig& cfg) {
            cfg.mLedgerMaxInstructions = 100'000'000;
            cfg.mLedgerMaxDependentTxClusters = 4;
        });

        modifySorobanNetworkConfig(app, [](SorobanNetworkConfig& cfg) {
            cfg.mLedgerMaxInstructions = 100'000'000;
            cfg.mLedgerMaxDependentTxClusters = 1;
        });

        stellar::uniform_int_distribution<uint32_t> dist(50, 1000);
        std::vector<uint32_t> extendTos;

        std::vector<TransactionFrameBaseConstPtr> sorobanTxs;

        auto inclusionFee =
            client.writeKeySpec("key2", ContractDataDurability::TEMPORARY)
                .getInclusionFee();
        for (size_t i = 0; i < 6; i++)
        {
            auto extendTo = dist(getGlobalRandomEngine());
            extendTos.emplace_back(extendTo);

            // The first tx will be a rw, the rest are ro
            auto spec = i == 0 ? client.writeKeySpec(
                                     "key2", ContractDataDurability::TEMPORARY)
                               : client.readKeySpec(
                                     "key2", ContractDataDurability::TEMPORARY);
            spec = spec.setInclusionFee(inclusionFee++);

            auto invocation = client.getContract().prepareInvocation(
                "extend_temporary",
                {makeSymbolSCVal("key2"), makeU32SCVal(extendTo),
                 makeU32SCVal(extendTo)},
                spec);

            auto account =
                TestAccount(app, getAccount(("a" + std::to_string(i)).c_str()));
            auto tx = invocation.withExactNonRefundableResourceFee().createTx(
                &account);
            sorobanTxs.emplace_back(tx);
        }

        // Save a map of tx hashs to extensions so we can do a lookup against
        // the ordering of tx results to determine where the readWrite
        // transaction was applied.
        UnorderedMap<Hash, uint32_t> txHashToExtendTO;
        for (size_t i = 0; i < sorobanTxs.size(); ++i)
        {
            txHashToExtendTO.emplace(sorobanTxs.at(i)->getContentsHash(),
                                     extendTos.at(i));
        }

        auto observedTtl = test.getTTL(client.getContract().getDataKey(
            makeSymbolSCVal("key2"), ContractDataDurability::TEMPORARY));

        successesBefore = hostFnSuccessMeter.count();
        auto r = closeLedger(test.getApp(), sorobanTxs);

        REQUIRE(r.results.size() == sorobanTxs.size());

        // We're testing to make sure the write tx observed any ttl extensions
        // before it, so save the largest extension seen before the write tx.
        uint32_t maxLiveUntilSeenBeforeWrite = observedTtl;
        for (auto const& res : r.results)
        {
            // The first tx in sorobanTxs is the readWrite tx
            if (res.transactionHash == sorobanTxs.at(0)->getContentsHash())
            {
                break;
            }

            auto it = txHashToExtendTO.find(res.transactionHash);
            REQUIRE(it != txHashToExtendTO.end());

            maxLiveUntilSeenBeforeWrite = std::max(
                it->second + test.getLCLSeq(), maxLiveUntilSeenBeforeWrite);
        }

        REQUIRE(hostFnSuccessMeter.count() - successesBefore ==
                sorobanTxs.size());
        REQUIRE(hostFnFailureMeter.count() == 0);

        checkResults(r, sorobanTxs.size(), 0);

        auto metaMap = readParallelMeta(metaPath);
        REQUIRE(metaMap.count(test.getLCLSeq()));

        auto const& lcm = metaMap[test.getLCLSeq()];
        REQUIRE(lcm.v2().txProcessing.size() == 6);
        for (auto const& txResultMeta : lcm.v2().txProcessing)
        {
            bool isWrite = sorobanTxs.at(0)->getContentsHash() ==
                           txResultMeta.result.transactionHash;

            auto const& changes =
                txResultMeta.txApplyProcessing.v4().operations.at(0).changes;

            if (isWrite)
            {
                observedTtl = maxLiveUntilSeenBeforeWrite;
            }

            // Verify meta makes sense
            for (auto const& change : changes)
            {
                switch (change.type())
                {
                case LedgerEntryChangeType::LEDGER_ENTRY_STATE:
                {
                    if (change.state().data.type() != TTL)
                    {
                        continue;
                    }

                    REQUIRE(change.state().data.ttl().liveUntilLedgerSeq ==
                            observedTtl);
                }
                break;
                case LedgerEntryChangeType::LEDGER_ENTRY_UPDATED:
                {
                    if (change.updated().data.type() != TTL)
                    {
                        continue;
                    }

                    auto writeTtl = extendTos.at(0) + test.getLCLSeq();
                    auto thisTxTtl =
                        txHashToExtendTO
                            .find(txResultMeta.result.transactionHash)
                            ->second +
                        test.getLCLSeq();
                    if (isWrite)
                    {
                        observedTtl = writeTtl;
                    }

                    auto expected = isWrite ? writeTtl : thisTxTtl;
                    REQUIRE(change.updated().data.ttl().liveUntilLedgerSeq ==
                            expected);
                }
                break;

                default:
                    REQUIRE(false);
                }
            }
        }

        auto maxExtendTo =
            *std::max_element(extendTos.begin(), extendTos.end());
        auto maxTtl = maxExtendTo + test.getLCLSeq();
        REQUIRE(test.getTTL(client.getContract().getDataKey(
                    makeSymbolSCVal("key2"),
                    ContractDataDurability::TEMPORARY)) == maxTtl);
    };

    for (size_t i = 0; i < 25; ++i)
    {
        SECTION("multi RO extensions with a single RW extension in a single "
                "stage and cluster. Run " +
                std::to_string(i))
        {
            REQUIRE(client.has("key2", ContractDataDurability::TEMPORARY,
                               true) == INVOKE_HOST_FUNCTION_SUCCESS);

            singleRWMultiROExtensionTest();
        }
    }
}

TEST_CASE("Failed write still causes ttl observation",
          "[tx][soroban][parallelapply]")
{
    auto cfg = getTestConfig();
    cfg.LEDGER_PROTOCOL_VERSION =
        static_cast<uint32_t>(PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION);
    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
        static_cast<uint32_t>(PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION);
    cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS = true;

    SorobanTest test(cfg);
    ContractStorageTestClient client(test);

    auto& app = test.getApp();
    modifySorobanNetworkConfig(app, [](SorobanNetworkConfig& cfg) {
        cfg.mLedgerMaxInstructions = 100'000'000;
        cfg.mLedgerMaxDependentTxClusters = 1;
    });

    auto& lm = app.getLedgerManager();

    const int64_t startingBalance = lm.getLastMinBalance(50);

    auto& root = test.getRoot();
    auto a1 = root.create("a1", startingBalance);
    auto a2 = root.create("a2", startingBalance);
    auto a3 = root.create("a3", startingBalance);

    REQUIRE(client.put("key", ContractDataDurability::PERSISTENT, 0) ==
            INVOKE_HOST_FUNCTION_SUCCESS);

    auto const& stateArchivalSettings =
        test.getNetworkCfg().stateArchivalSettings();

    auto startingTTL =
        test.getLCLSeq() + stateArchivalSettings.minPersistentTTL - 1;
    REQUIRE(client.getTTL("key", ContractDataDurability::PERSISTENT) ==
            startingTTL);

    auto key1ReadSpec =
        client.readKeySpec("key", ContractDataDurability::PERSISTENT);

    // Tx2 will fail
    auto key1WriteSpec =
        client.writeKeySpec("key", ContractDataDurability::PERSISTENT)
            .setRefundableResourceFee(1);

    auto i1 = client.getContract().prepareInvocation(
        "extend_persistent",
        {makeSymbolSCVal("key"), makeU32SCVal(1000), makeU32SCVal(1000)},
        key1ReadSpec);
    auto tx1 = i1.withExactNonRefundableResourceFee().createTx(&a1);

    auto i2 = client.getContract().prepareInvocation(
        "extend_persistent",
        {makeSymbolSCVal("key"), makeU32SCVal(2000), makeU32SCVal(2000)},
        key1WriteSpec);
    auto tx2 = i2.withExactNonRefundableResourceFee().createTx(&a2);

    auto i3 = client.getContract().prepareInvocation(
        "extend_persistent",
        {makeSymbolSCVal("key"), makeU32SCVal(1100), makeU32SCVal(1100)},
        key1ReadSpec);
    auto tx3 = i3.withExactNonRefundableResourceFee().createTx(&a3);

    auto r = closeLedger(test.getApp(), {tx1, tx2, tx3}, /*strictOrder=*/true);
    REQUIRE(r.results.size() == 3);

    checkTx(0, r, txSUCCESS);
    checkTx(1, r, txFAILED);
    checkTx(2, r, txSUCCESS);

    REQUIRE(client.getTTL("key", ContractDataDurability::PERSISTENT) ==
            test.getLCLSeq() + 1100);

    auto const& lcm = lm.getLastClosedLedgerTxMeta();
    REQUIRE(lcm.size() == 3);
    auto opMeta0 =
        lm.getLastClosedLedgerTxMeta().at(0).getLedgerEntryChangesAtOp(0);
    REQUIRE(opMeta0.at(0).state().data.ttl().liveUntilLedgerSeq == startingTTL);
    REQUIRE(opMeta0.at(1).updated().data.ttl().liveUntilLedgerSeq ==
            test.getLCLSeq() + 1000);

    REQUIRE(
        lm.getLastClosedLedgerTxMeta().at(1).getXDR().v4().operations.empty());

    auto opMeta2 =
        lm.getLastClosedLedgerTxMeta().at(2).getLedgerEntryChangesAtOp(0);
    REQUIRE(opMeta2.at(0).state().data.ttl().liveUntilLedgerSeq ==
            test.getLCLSeq() + 1000);
    REQUIRE(opMeta2.at(1).updated().data.ttl().liveUntilLedgerSeq ==
            test.getLCLSeq() + 1100);
}

TEST_CASE("parallel txs hit declared readBytes", "[tx][soroban][parallelapply]")
{
    auto cfg = getTestConfig();
    cfg.LEDGER_PROTOCOL_VERSION =
        static_cast<uint32_t>(PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION);
    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
        static_cast<uint32_t>(PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION);
    cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS = true;

    SorobanTest test(cfg);
    ContractStorageTestClient client(test);

    auto contractExpirationLedger =
        test.getLCLSeq() +
        test.getNetworkCfg().stateArchivalSettings().minPersistentTTL;

    auto& app = test.getApp();
    modifySorobanNetworkConfig(app, [](SorobanNetworkConfig& cfg) {
        cfg.mLedgerMaxInstructions = 5'000'000;
        cfg.mTxMaxInstructions = 5'000'000;
        cfg.mLedgerMaxDependentTxClusters = 2;
    });

    auto& lm = app.getLedgerManager();

    const int64_t startingBalance = lm.getLastMinBalance(50);

    auto& root = test.getRoot();
    auto a1 = root.create("a1", startingBalance);
    auto a2 = root.create("a2", startingBalance);
    auto a3 = root.create("a3", startingBalance);

    REQUIRE(client.put("key2", ContractDataDurability::TEMPORARY, 0) ==
            INVOKE_HOST_FUNCTION_SUCCESS);

    SECTION("invoke")
    {
        // tx1 will fast fail due to hitting the readBytes limit when loading
        // the classic account entry
        auto i1Spec =
            client.writeKeySpec("key1", ContractDataDurability::TEMPORARY)
                .extendReadOnlyFootprint({accountKey(a1.getPublicKey())})
                .setReadBytes(5);
        auto i1 = client.getContract().prepareInvocation(
            "put_temporary", {makeSymbolSCVal("key1"), makeU64SCVal(123)},
            i1Spec.setInclusionFee(i1Spec.getInclusionFee() + 1));
        auto tx1 = i1.withExactNonRefundableResourceFee().createTx(&a1);

        // extend key2
        auto i2 = client.getContract().prepareInvocation(
            "extend_temporary",
            {makeSymbolSCVal("key2"), makeU32SCVal(400), makeU32SCVal(400)},
            client.readKeySpec("key2", ContractDataDurability::TEMPORARY)
                .setInclusionFee(i1Spec.getInclusionFee() + 2));
        auto tx2 = i2.withExactNonRefundableResourceFee().createTx(&a2);

        auto r = closeLedger(test.getApp(), {tx1, tx2}, /*strictOrder=*/true);
        REQUIRE(r.results.size() == 2);

        checkTx(0, r, txFAILED);
        checkTx(1, r, txSUCCESS);
        REQUIRE(r.results[0]
                    .result.result.results()[0]
                    .tr()
                    .invokeHostFunctionResult()
                    .code() == INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
    }
    SECTION("restore")
    {
        for (uint32_t i = test.getLCLSeq() + 1; i <= contractExpirationLedger;
             ++i)
        {
            closeLedgerOn(test.getApp(), i, 2, 1, 2016);
        }

        auto const& contractKeys = client.getContract().getKeys();

        SorobanResources restoreResources;
        restoreResources.footprint.readWrite = contractKeys;
        restoreResources.diskReadBytes = 3'028 + 104;
        restoreResources.writeBytes = restoreResources.diskReadBytes;
        auto txSufficientReadBytes =
            test.createRestoreTx(restoreResources, 30'000, 500'000, &a1);

        --restoreResources.diskReadBytes;
        auto txInsufficientReadBytes =
            test.createRestoreTx(restoreResources, 30'000, 500'000, &a2);

        auto r = closeLedger(test.getApp(),
                             {txInsufficientReadBytes, txSufficientReadBytes},
                             /*strictOrder=*/true);
        REQUIRE(r.results.size() == 2);

        checkTx(0, r, txFAILED);
        checkTx(1, r, txSUCCESS);

        REQUIRE(r.results[0]
                    .result.result.results()[0]
                    .tr()
                    .restoreFootprintResult()
                    .code() == RESTORE_FOOTPRINT_RESOURCE_LIMIT_EXCEEDED);
    }
}

TEST_CASE("delete non existent entry with parallel apply",
          "[tx][soroban][parallelapply]")
{
    auto cfg = getTestConfig();
    cfg.LEDGER_PROTOCOL_VERSION =
        static_cast<uint32_t>(PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION);
    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
        static_cast<uint32_t>(PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION);

    SorobanTest test(cfg);
    ContractStorageTestClient client(test);

    auto& app = test.getApp();
    auto& lm = app.getLedgerManager();

    const int64_t startingBalance = lm.getLastMinBalance(50);

    auto& root = test.getRoot();
    auto a1 = root.create("a1", startingBalance);

    auto i1Spec =
        client.writeKeySpec("key1", ContractDataDurability::TEMPORARY);
    auto i1 = client.getContract().prepareInvocation(
        "del_temporary", {makeSymbolSCVal("key1")},
        i1Spec.setInclusionFee(i1Spec.getInclusionFee() + 1));
    auto tx = i1.withExactNonRefundableResourceFee().createTx(&a1);

    auto r = closeLedger(test.getApp(), {tx});
    checkTx(0, r, txSUCCESS);

    REQUIRE(client.has("key1", ContractDataDurability::TEMPORARY, false) ==
            INVOKE_HOST_FUNCTION_SUCCESS);
}

TEST_CASE_VERSIONS("merge account then SAC payment scenarios",
                   "[tx][soroban][parallelapply]")
{
    Config cfg = getTestConfig();

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);

    for_versions_from(20, *app, [&] {
        SorobanTest test(app);
        AssetContractTestClient assetClient(test, txtest::makeNativeAsset());

        const int64_t startingBalance =
            test.getApp().getLedgerManager().getLastMinBalance(50);

        auto a1 = test.getRoot().create("A", startingBalance);
        auto b1 = test.getRoot().create("B", startingBalance);
        auto c1 = test.getRoot().create("C", startingBalance);

        SECTION("SAC payment to the merged account")
        {
            // Classic transaction: merge account a1 into b1
            auto classicTx = a1.tx({accountMerge(b1.getPublicKey())});

            // Soroban transaction: SAC payment to the merged account (a1) -
            // this should fail
            auto a1Addr = makeAccountAddress(a1.getPublicKey());
            auto sacTx = assetClient.getTransferTx(c1, a1Addr, 50,
                                                   true /*sourceIsRoot*/);

            std::vector<TransactionFrameBasePtr> txs = {classicTx, sacTx};
            auto r = closeLedger(test.getApp(), txs);
            REQUIRE(r.results.size() == 2);

            checkTx(0, r, txSUCCESS);
            checkTx(1, r, txFAILED);

            // Verify that a1 no longer exists after the merge
            LedgerSnapshot ls(test.getApp());
            REQUIRE(!ls.getAccount(a1.getPublicKey()));

            // Verify that b1 received a1's balance (minus merge fee)
            auto expectedBalance =
                startingBalance +
                (startingBalance - r.results.at(0).result.feeCharged);
            REQUIRE(b1.getBalance() == expectedBalance);

            // Verify that c1's balance remains unchanged
            REQUIRE(c1.getBalance() == startingBalance);
        }

        SECTION("SAC payment from the merged account")
        {
            // Classic transaction: merge account a1 into b1
            auto classicTx = a1.tx({accountMerge(b1.getPublicKey())});

            // Soroban transaction: SAC payment from the merged account (a1) to
            // c1 with root as source - this should fail
            auto c1Addr = makeAccountAddress(c1.getPublicKey());
            auto sacTx = assetClient.getTransferTx(a1, c1Addr, 50,
                                                   true /*sourceIsRoot*/);

            std::vector<TransactionFrameBasePtr> txs = {classicTx, sacTx};
            auto r = closeLedger(test.getApp(), txs);
            REQUIRE(r.results.size() == 2);

            checkTx(0, r, txSUCCESS);
            checkTx(1, r, txFAILED);

            // Verify that a1 no longer exists after the merge
            LedgerSnapshot ls(test.getApp());
            REQUIRE(!ls.getAccount(a1.getPublicKey()));

            // Verify that b1 received a1's balance (minus merge fee)
            auto expectedBalance =
                startingBalance +
                (startingBalance - r.results.at(0).result.feeCharged);
            REQUIRE(b1.getBalance() == expectedBalance);

            // Verify that c1's balance remains unchanged (no payment occurred)
            REQUIRE(c1.getBalance() == startingBalance);
        }

        SECTION("SAC payment with merged account as source")
        {
            // Classic transaction: merge account a1 into b1 (using a1 as
            // source)
            auto mergeOp = accountMerge(b1.getPublicKey());
            mergeOp.sourceAccount.activate() =
                toMuxedAccount(a1.getPublicKey());

            auto classicMergeTx = c1.tx({mergeOp});
            classicMergeTx->addSignature(a1.getSecretKey());

            // Soroban transaction: SAC payment from the merged account (a1) to
            // c1 with a1 as source - this should fail with txNO_ACCOUNT
            auto c1Addr = makeAccountAddress(c1.getPublicKey());
            auto sacTx = assetClient.getTransferTx(a1, c1Addr, 50);

            std::vector<TransactionFrameBasePtr> txs = {classicMergeTx, sacTx};
            auto r = closeLedger(test.getApp(), txs);
            REQUIRE(r.results.size() == 2);

            checkTx(0, r, txSUCCESS);
            checkTx(1, r, txNO_ACCOUNT);

            // Verify that a1 no longer exists after the merge
            LedgerSnapshot ls(test.getApp());
            REQUIRE(!ls.getAccount(a1.getPublicKey()));

            // Verify that b1 received a1's balance (minus the soroban
            // transactions fee which a1 paid before it was merged).
            auto expectedBalance =
                startingBalance +
                (startingBalance - r.results.at(1).result.feeCharged);
            REQUIRE(b1.getBalance() == expectedBalance);

            // Verify that c1's balance remains unchanged other than the fee
            REQUIRE(c1.getBalance() ==
                    startingBalance - r.results.at(0).result.feeCharged);
        }
    });
}

TEST_CASE_VERSIONS("trustline deletion then SAC payment",
                   "[tx][soroban][parallelapply]")
{
    Config cfg = getTestConfig();

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);

    for_versions_from(20, *app, [&] {
        SorobanTest test(app);

        // Create non-native asset
        auto issuerKey = getAccount("issuer");
        Asset idr = makeAsset(issuerKey, "IDR");
        AssetContractTestClient assetClient(test, idr);

        const int64_t startingBalance =
            test.getApp().getLedgerManager().getLastMinBalance(50);

        auto issuer = test.getRoot().create("issuer", startingBalance);
        auto holder = test.getRoot().create("holder", startingBalance);
        auto otherAccount = test.getRoot().create("other", startingBalance);
        auto recipient = test.getRoot().create("recipient", startingBalance);

        // Create trustline and fund it
        holder.changeTrust(idr, 1000);
        issuer.pay(holder, idr, 500);
        recipient.changeTrust(idr, 1000);

        REQUIRE(holder.getTrustlineBalance(idr) == 500);

        auto issuerAddr = makeAccountAddress(issuer.getPublicKey());
        auto holderAddr = makeAccountAddress(holder.getPublicKey());
        auto recipientAddr = makeAccountAddress(recipient.getPublicKey());

        // First transaction: send balance back to issuer
        auto payTx = holder.tx({payment(issuer, idr, 500)});

        // Second transaction: delete trustline (using different source
        // account)
        auto deleteTrustlineOp = changeTrust(idr, 0);
        deleteTrustlineOp.sourceAccount.activate() =
            toMuxedAccount(holder.getPublicKey());
        auto deleteTx = otherAccount.tx({deleteTrustlineOp});
        deleteTx->addSignature(holder.getSecretKey());

        // Third transaction: Soroban SAC payment attempt from deleted
        // trustline
        auto sacTx = assetClient.getTransferTx(holder, recipientAddr, 50,
                                               true /*sourceIsRoot*/);

        std::vector<TransactionFrameBasePtr> txs = {payTx, deleteTx, sacTx};
        auto r = closeLedger(test.getApp(), txs, /*strictOrder=*/true);
        REQUIRE(r.results.size() == 3);

        checkTx(0, r, txSUCCESS); // payment succeeds
        checkTx(1, r, txSUCCESS); // trustline deletion succeeds
        checkTx(2, r, txFAILED);  // SAC payment fails (no trustline)

        REQUIRE(!holder.hasTrustLine(idr));
        REQUIRE(recipient.getTrustlineBalance(idr) == 0);
    });
}

TEST_CASE("create in first stage delete in second stage",
          "[tx][soroban][parallelapply]")
{
    auto cfg = getTestConfig();

    SorobanTest test(cfg);
    ContractStorageTestClient client(test);

    auto& app = test.getApp();

    auto& lm = app.getLedgerManager();
    const int64_t startingBalance = lm.getLastMinBalance(50);

    auto& root = test.getRoot();
    auto a1 = root.create("a1", startingBalance);
    auto b1 = root.create("b", startingBalance);

    auto i1Spec =
        client.writeKeySpec("key1", ContractDataDurability::PERSISTENT);
    auto i1 = client.getContract().prepareInvocation(
        "put_persistent", {makeSymbolSCVal("key1"), makeU64SCVal(1)},
        i1Spec.setInclusionFee(i1Spec.getInclusionFee() + 1));
    auto tx1 = i1.withExactNonRefundableResourceFee().createTx(&a1);

    auto i2Spec =
        client.writeKeySpec("key1", ContractDataDurability::PERSISTENT);
    auto i2 = client.getContract().prepareInvocation(
        "del_persistent", {makeSymbolSCVal("key1")},
        i2Spec.setInclusionFee(i2Spec.getInclusionFee() + 2));
    auto tx2 = i2.withExactNonRefundableResourceFee().createTx(&b1);

    auto r = closeLedger(test.getApp(), {tx1, tx2}, {{{0}}, {{1}}});
    REQUIRE(r.results.size() == 2);
    checkTx(0, r, txSUCCESS);
    checkTx(1, r, txSUCCESS);

    REQUIRE(client.has("key1", ContractDataDurability::PERSISTENT, false) ==
            INVOKE_HOST_FUNCTION_SUCCESS);
}

TEST_CASE("delete in first stage extend in second stage",
          "[tx][soroban][parallelapply]")
{
    auto cfg = getTestConfig();

    SorobanTest test(cfg);
    ContractStorageTestClient client(test);

    auto& app = test.getApp();

    auto& lm = app.getLedgerManager();
    const int64_t startingBalance = lm.getLastMinBalance(50);

    auto& root = test.getRoot();
    auto a1 = root.create("a1", startingBalance);
    auto b1 = root.create("b", startingBalance);

    // First create the entry so we can delete it
    REQUIRE(client.put("key", ContractDataDurability::PERSISTENT, 1) ==
            INVOKE_HOST_FUNCTION_SUCCESS);

    // First stage: delete the entry
    auto i1Spec =
        client.writeKeySpec("key", ContractDataDurability::PERSISTENT);
    auto i1 = client.getContract().prepareInvocation(
        "del_persistent", {makeSymbolSCVal("key")},
        i1Spec.setInclusionFee(i1Spec.getInclusionFee() + 1));
    auto tx1 = i1.withExactNonRefundableResourceFee().createTx(&a1);

    // Second stage: try to extend the deleted entry - this should fail
    auto i2Spec = client.readKeySpec("key", ContractDataDurability::PERSISTENT);
    auto i2 = client.getContract().prepareInvocation(
        "extend_persistent",
        {makeSymbolSCVal("key"), makeU32SCVal(1000), makeU32SCVal(1000)},
        i2Spec.setInclusionFee(i2Spec.getInclusionFee() + 2));
    auto tx2 = i2.withExactNonRefundableResourceFee().createTx(&b1);

    auto r = closeLedger(test.getApp(), {tx1, tx2}, {{{0}}, {{1}}});
    REQUIRE(r.results.size() == 2);

    checkTx(0, r, txSUCCESS);
    checkTx(1, r, txFAILED);

    // Verify the entry is indeed deleted
    REQUIRE(client.has("key", ContractDataDurability::PERSISTENT, false) ==
            INVOKE_HOST_FUNCTION_SUCCESS);
}

TEST_CASE("put in first stage and then update value in second stage",
          "[tx][soroban][parallelapply]")
{
    auto cfg = getTestConfig();

    SorobanTest test(cfg);
    ContractStorageTestClient client(test);

    auto& app = test.getApp();

    auto& lm = app.getLedgerManager();
    const int64_t startingBalance = lm.getLastMinBalance(50);

    auto& root = test.getRoot();
    auto a1 = root.create("a1", startingBalance);
    auto b1 = root.create("b", startingBalance);

    // First stage: put initial value
    auto i1Spec =
        client.writeKeySpec("key", ContractDataDurability::PERSISTENT);
    auto i1 = client.getContract().prepareInvocation(
        "put_persistent", {makeSymbolSCVal("key"), makeU64SCVal(100)}, i1Spec);
    auto tx1 = i1.withExactNonRefundableResourceFee().createTx(&a1);

    // Second stage: put updated value
    auto i2Spec =
        client.writeKeySpec("key", ContractDataDurability::PERSISTENT);
    auto i2 = client.getContract().prepareInvocation(
        "put_persistent", {makeSymbolSCVal("key"), makeU64SCVal(200)}, i2Spec);
    auto tx2 = i2.withExactNonRefundableResourceFee().createTx(&b1);

    auto r = closeLedger(test.getApp(), {tx1, tx2}, {{{0}}, {{1}}});
    REQUIRE(r.results.size() == 2);

    checkTx(0, r, txSUCCESS);
    checkTx(1, r, txSUCCESS);

    // Verify the entry has the updated value from the second stage
    REQUIRE(client.get("key", ContractDataDurability::PERSISTENT, 200) ==
            INVOKE_HOST_FUNCTION_SUCCESS);
}

TEST_CASE("apply generated parallel tx sets", "[soroban][parallelapply]")
{
    uint32 const MAX_TRANSACTIONS_PER_LEDGER = 500;
    auto cfg = getTestConfig();
    cfg.LEDGER_PROTOCOL_VERSION =
        static_cast<uint32_t>(PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION);
    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
        static_cast<uint32_t>(PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION);
    cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS = true;

    // This is required because we're setting the min stage count above one,
    // which will reduce the per-stage instruction count too low to run the
    // settings upgrade transactions.
    cfg.TESTING_SOROBAN_HIGH_LIMIT_OVERRIDE = true;
    cfg.SOROBAN_PHASE_MIN_STAGE_COUNT = 3;
    cfg.SOROBAN_PHASE_MAX_STAGE_COUNT = 4;
    cfg.GENESIS_TEST_ACCOUNT_COUNT = 1000;

    std::vector<std::string> keys = {"key1", "key2", "key3", "key4",
                                     "key5", "key6", "key7"};
    std::vector<std::string> actions = {"put", "extend", "del"};
    std::vector<ContractDataDurability> durabilities = {
        ContractDataDurability::TEMPORARY, ContractDataDurability::PERSISTENT};

    SorobanTest test(cfg);
    ContractStorageTestClient client(test);

    auto& app = test.getApp();

    modifySorobanNetworkConfig(app, [&](SorobanNetworkConfig& cfg) {
        cfg.mLedgerMaxInstructions = 400'000'000;
        cfg.mLedgerMaxTxCount = MAX_TRANSACTIONS_PER_LEDGER;
        cfg.mLedgerMaxDependentTxClusters = 2;
    });

    test.invokeExtendOp(client.getContract().getKeys(), 10'000);

    auto& lm = app.getLedgerManager();
    auto& root = test.getRoot();
    const int64_t startingBalance = lm.getLastMinBalance(50);

    stellar::uniform_int_distribution<uint32_t> keyDist(0, keys.size() - 1);
    stellar::uniform_int_distribution<uint32_t> actionDist(0,
                                                           actions.size() - 1);
    stellar::uniform_int_distribution<uint32_t> durabilityDist(
        0, durabilities.size() - 1);
    stellar::uniform_int_distribution<uint32_t> ttlDist(0, 10'000);

    uint32_t accountId = 0;

    for (int i = 0; i < 2; ++i)
    {
        std::vector<TransactionFrameBaseConstPtr> sorobanTxs;
        auto resources = lm.maxLedgerResources(true);
        LedgerSnapshot ls(app);
        for (int txId = 0; txId < MAX_TRANSACTIONS_PER_LEDGER; ++txId)
        {
            auto account = txtest::getGenesisAccount(app, accountId++);
            auto const& key = keys.at(keyDist(getGlobalRandomEngine()));
            auto const& action =
                actions.at(actionDist(getGlobalRandomEngine()));
            auto const& durability =
                durabilities.at(durabilityDist(getGlobalRandomEngine()));

            SorobanInvocationSpec spec;
            std::vector<SCVal> args;
            if (action == "put")
            {
                spec = client.writeKeySpec(key, durability);
                args = {makeSymbolSCVal(key), makeU64SCVal(123)};
            }
            else if (action == "extend")
            {
                spec = client.readKeySpec(key, durability);
                auto ttl = ttlDist(getGlobalRandomEngine());
                args = {makeSymbolSCVal(key), makeU32SCVal(ttl),
                        makeU32SCVal(ttl)};
            }
            else
            {
                spec = client.writeKeySpec(key, durability);
                REQUIRE(action == "del");
                args = {makeSymbolSCVal(key)};
            }

            auto durabilityString =
                durability == ContractDataDurability::PERSISTENT ? "persistent"
                                                                 : "temporary";

            auto invocation = client.getContract().prepareInvocation(
                action + "_" + durabilityString, args, spec);

            auto tx = invocation.withExactNonRefundableResourceFee().createTx(
                &account);

            REQUIRE(tx->checkValid(app.getAppConnector(), ls, 0, 0, 0)
                        ->isSuccess());
            if (!anyGreater(tx->getResources(false, test.getLedgerVersion()),
                            resources))
            {
                resources -= tx->getResources(false, test.getLedgerVersion());
            }
            else
            {
                break;
            }

            sorobanTxs.emplace_back(tx);
        }

        auto r = closeLedger(test.getApp(), sorobanTxs);

        // This test just checks to make sure we don't trigger an internal
        // error. Some transactions can still fail due to missing keys.
        for (auto const& txRes : r.results)
        {
            REQUIRE(txRes.result.result.code() != txINTERNAL_ERROR);
        }
        REQUIRE(r.results.size() <= sorobanTxs.size());
        // It's not always possible to perfectly pack all the transactions into
        // two clusters, so a transaction may be left out from time to time.
        REQUIRE(sorobanTxs.size() - r.results.size() <= 2);
    }
}

TEST_CASE("parallel restore and extend op", "[tx][soroban][parallelapply]")
{
    auto cfg = getTestConfig();
    cfg.LEDGER_PROTOCOL_VERSION =
        static_cast<uint32_t>(PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION);
    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
        static_cast<uint32_t>(PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION);

    SorobanTest test(cfg);
    ContractStorageTestClient client(test);

    auto& app = test.getApp();

    auto& lm = app.getLedgerManager();

    const int64_t startingBalance = lm.getLastMinBalance(50);

    // Advance ledger until the contract expires
    auto expirationLedger =
        test.getLCLSeq() +
        test.getNetworkCfg().stateArchivalSettings().minPersistentTTL;
    for (uint32_t i = test.getLCLSeq() + 1; i <= expirationLedger; ++i)
    {
        closeLedgerOn(test.getApp(), i, 2, 1, 2016);
    }

    auto const& contractKeys = client.getContract().getKeys();
    REQUIRE(!test.isEntryLive(contractKeys[0], test.getLCLSeq()));
    REQUIRE(!test.isEntryLive(contractKeys[1], test.getLCLSeq()));

    auto& root = test.getRoot();
    auto a1 = root.create("a1", startingBalance);
    auto a2 = root.create("a2", startingBalance);

    SorobanResources restoreResources;
    restoreResources.footprint.readWrite = contractKeys;
    restoreResources.diskReadBytes = 9'000;
    restoreResources.writeBytes = 9'000;

    auto const resourceFee = 300'000 + 40'000 * contractKeys.size();
    auto tx1 = test.createRestoreTx(restoreResources, 1'000, resourceFee, &a1);

    SorobanResources extendResources;
    extendResources.footprint.readOnly = contractKeys;
    extendResources.diskReadBytes = 9'000;
    auto tx2 =
        test.createExtendOpTx(extendResources, 10'000, 30'000, 500'000, &a2);

    std::vector<TransactionFrameBaseConstPtr> sorobanTxs;
    sorobanTxs.emplace_back(tx1);
    sorobanTxs.emplace_back(tx2);

    auto r = closeLedger(test.getApp(), sorobanTxs);
    REQUIRE(r.results.size() == sorobanTxs.size());

    checkTx(0, r, txSUCCESS);
    checkTx(1, r, txSUCCESS);
}

TEST_CASE("read-only bumps across threads", "[tx][soroban][parallelapply]")
{
    auto cfg = getTestConfig();

    SorobanTest test(cfg, true, [](SorobanNetworkConfig& cfg) {
        cfg.mLedgerMaxInstructions = 20'000'000;
        cfg.mTxMaxInstructions = 20'000'000;
        cfg.mLedgerMaxDependentTxClusters = 2;
    });

    ContractStorageTestClient client(test);
    test.invokeExtendOp(client.getContract().getKeys(), 10'000);

    auto& lm = test.getApp().getLedgerManager();
    const int64_t startingBalance = lm.getLastMinBalance(50);

    auto& root = test.getRoot();

    REQUIRE(client.put("key", ContractDataDurability::TEMPORARY, 0) ==
            INVOKE_HOST_FUNCTION_SUCCESS);

    auto keySpec = client.readKeySpec("key", ContractDataDurability::TEMPORARY);

    // Each tx is 4M instructions, so we can fit 5 txs in a single cluster.
    // 10 txs will be split across 2 clusters.
    std::vector<TestAccount> accounts;
    for (size_t i = 0; i < 10; ++i)
    {
        accounts.emplace_back(
            root.create("a" + std::to_string(i), startingBalance));
    }

    uint32_t maxExtension = 0;
    stellar::uniform_int_distribution<uint32_t> dist(100, 1000);

    std::vector<TransactionFrameBaseConstPtr> sorobanTxs;
    for (auto& account : accounts)
    {
        auto extendTo = dist(getGlobalRandomEngine());
        maxExtension = std::max(maxExtension, extendTo);

        auto inv = client.getContract().prepareInvocation(
            "extend_temporary",
            {makeSymbolSCVal("key"), makeU32SCVal(extendTo),
             makeU32SCVal(extendTo)},
            keySpec);
        auto tx = inv.withExactNonRefundableResourceFee().createTx(&account);
        sorobanTxs.emplace_back(tx);
    }

    auto r = closeLedger(test.getApp(), sorobanTxs);
    REQUIRE(r.results.size() == sorobanTxs.size());
    checkResults(r, sorobanTxs.size(), 0);

    REQUIRE(test.getTTL(client.getContract().getDataKey(
                makeSymbolSCVal("key"), ContractDataDurability::TEMPORARY)) ==
            maxExtension + test.getLCLSeq());
}

TEST_CASE("parallel restore and update", "[tx][soroban][parallelapply]")
{
    auto cfg = getTestConfig();
    cfg.LEDGER_PROTOCOL_VERSION =
        static_cast<uint32_t>(PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION);
    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
        static_cast<uint32_t>(PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION);

    SorobanTest test(cfg);
    ContractStorageTestClient client(test);

    auto& app = test.getApp();

    auto& lm = app.getLedgerManager();

    const int64_t startingBalance = lm.getLastMinBalance(50);

    REQUIRE(client.put("key", ContractDataDurability::PERSISTENT, 1) ==
            INVOKE_HOST_FUNCTION_SUCCESS);

    // Advance ledger until the entry created above expires
    auto expirationLedger =
        test.getLCLSeq() +
        test.getNetworkCfg().stateArchivalSettings().minPersistentTTL;
    for (uint32_t i = test.getLCLSeq() + 1; i <= expirationLedger; ++i)
    {
        closeLedgerOn(test.getApp(), i, 2, 1, 2016);
    }

    auto persistentKey = client.getContract().getDataKey(
        makeSymbolSCVal("key"), ContractDataDurability::PERSISTENT);

    auto& root = test.getRoot();
    auto a1 = root.create("a1", startingBalance);
    auto a2 = root.create("a2", startingBalance);

    auto contractKeys = client.getContract().getKeys();
    contractKeys.emplace_back(persistentKey);
    REQUIRE(contractKeys.size() == 3);

    for (auto const& key : contractKeys)
    {
        REQUIRE(!test.isEntryLive(key, test.getLCLSeq()));
    }

    SorobanResources restoreResources;
    restoreResources.footprint.readWrite = contractKeys;
    restoreResources.diskReadBytes = 9'000;
    restoreResources.writeBytes = 9'000;

    auto const resourceFee = 1'000'000 + 40'000 * contractKeys.size();
    auto tx1 = test.createRestoreTx(restoreResources, 1'000, resourceFee, &a1);

    auto i1Spec =
        client.writeKeySpec("key", ContractDataDurability::PERSISTENT);
    auto i1 = client.getContract().prepareInvocation(
        "put_persistent", {makeSymbolSCVal("key"), makeU64SCVal(3)},
        i1Spec.setInclusionFee(i1Spec.getInclusionFee() + 2));
    auto tx2 = i1.withExactNonRefundableResourceFee().createTx(&a2);

    auto r = closeLedger(test.getApp(), {tx1, tx2}, /*strictOrder=*/true);
    REQUIRE(r.results.size() == 2);

    checkTx(0, r, txSUCCESS);
    checkTx(1, r, txSUCCESS);

    for (auto const& key : contractKeys)
    {
        REQUIRE(
            test.getTTL(key) ==
            test.getLCLSeq() +
                test.getNetworkCfg().stateArchivalSettings().minPersistentTTL -
                1);
    }

    REQUIRE(client.get("key", ContractDataDurability::PERSISTENT, 3) ==
            INVOKE_HOST_FUNCTION_SUCCESS);
}

TEST_CASE_VERSIONS(
    "refund to source in tx1 and SAC transfer from same account in tx2",
    "[tx][soroban][parallelapply]")
{
    Config cfg = getTestConfig();
    cfg.EMIT_CLASSIC_EVENTS = true;
    cfg.BACKFILL_STELLAR_ASSET_EVENTS = true;

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);

    for_versions_from(20, *app, [&] {
        SorobanTest test(app);
        AssetContractTestClient assetClient(test, txtest::makeNativeAsset());

        auto ledgerVersion = getLclProtocolVersion(test.getApp());

        const int64_t startingBalance =
            test.getApp().getLedgerManager().getLastMinBalance(50);

        auto a1 = test.getRoot().create("A", startingBalance);
        auto b1 = test.getRoot().create("B", startingBalance);

        auto wasm = rust_bridge::get_test_wasm_add_i32();
        auto resources =
            defaultUploadWasmResourcesWithoutFootprint(wasm, ledgerVersion);
        auto tx =
            makeSorobanWasmUploadTx(test.getApp(), a1, wasm, resources, 1000);

        auto lclHeader =
            app->getLedgerManager().getLastClosedLedgerHeader().header;

        auto a1PreRefundFeeCharged =
            tx->getFee(lclHeader, lclHeader.baseFee, true);

        // Pre v23, refunds are done after each tx. From v23, refunds are done
        // after all txs are applied.
        SECTION("success")
        {
            auto a1MaxSend = a1.getAvailableBalance() - a1PreRefundFeeCharged;

            auto b1Addr = makeAccountAddress(b1.getPublicKey());
            auto sacTx = assetClient.getTransferTx(a1, b1Addr, a1MaxSend, true);

            auto r = closeLedger(test.getApp(), {tx, sacTx}, true);
            REQUIRE(r.results.size() == 2);
            checkTx(0, r, txSUCCESS);
            checkTx(1, r, txSUCCESS);

            REQUIRE(a1.getAvailableBalance() ==
                    a1PreRefundFeeCharged - r.results.at(0).result.feeCharged);
        }

        SECTION("failure")
        {
            auto preTxA1Balance = a1.getAvailableBalance();
            auto a1OverMax =
                a1.getAvailableBalance() - a1PreRefundFeeCharged + 1;

            if (protocolVersionIsBefore(test.getLedgerVersion(),
                                        ProtocolVersion::V_23))
            {
                // Pre v23 the refund is done after each tx, so just set this
                // to a value that will cause the transfer to fail.
                a1OverMax = a1.getAvailableBalance() + 1;
            }

            auto b1Addr = makeAccountAddress(b1.getPublicKey());
            auto sacTx = assetClient.getTransferTx(a1, b1Addr, a1OverMax, true);

            auto r = closeLedger(test.getApp(), {tx, sacTx}, true);
            REQUIRE(r.results.size() == 2);
            checkTx(0, r, txSUCCESS);
            checkTx(1, r, txFAILED);

            REQUIRE(a1.getAvailableBalance() ==
                    preTxA1Balance - r.results.at(0).result.feeCharged);
        }
    });
}

TEST_CASE("in-memory state size tracking", "[soroban]")
{
    int const MIN_PERSISTENT_TTL = 100;
    auto const keyCount = 20;
    SorobanTest test(
        getTestConfig(), /*useTestLimits=*/false,
        [&](SorobanNetworkConfig& cfg) {
            cfg.mStateArchivalSettings.minPersistentTTL = MIN_PERSISTENT_TTL;
            // Set low eviction scan level in order to get the
            // entries evicted faster.
            // The state size is only reduced on evictions.
            cfg.mStateArchivalSettings.startingEvictionScanLevel = 1;
            // Modify the config to let the transactions run only in parallel.
            cfg.mLedgerMaxDependentTxClusters = 5;
            cfg.mLedgerMaxInstructions =
                keyCount *
                ContractStorageTestClient::defaultSpecWithoutFootprint()
                    .getResources()
                    .instructions /
                cfg.mLedgerMaxDependentTxClusters;
            cfg.mTxMaxInstructions = cfg.mLedgerMaxInstructions;

            // Use the test limit overrides here instead of using
            // `useTestLimits` in order to have just a single config upgrade
            // that's easy to account for (all the upgrade data entries will
            // live for 4096 entries, which is more than this test goes
            // through).
            cfg.mMaxContractSizeBytes = 64 * 1024;
            cfg.mMaxContractDataEntrySizeBytes = 64 * 1024;

            cfg.mTxMaxSizeBytes = 100 * 1024;
            cfg.mLedgerMaxTransactionsSizeBytes = cfg.mTxMaxSizeBytes * 10;

            cfg.mTxMaxDiskReadEntries = 40;
            cfg.mTxMaxDiskReadBytes = 200 * 1024;

            cfg.mTxMaxWriteLedgerEntries = 20;
            cfg.mTxMaxWriteBytes = 100 * 1024;

            cfg.mLedgerMaxDiskReadEntries = cfg.mTxMaxDiskReadEntries * 10;
            cfg.mLedgerMaxDiskReadBytes = cfg.mTxMaxDiskReadBytes * 10;
            cfg.mLedgerMaxWriteLedgerEntries =
                cfg.mTxMaxWriteLedgerEntries * 10;
            cfg.mLedgerMaxWriteBytes = cfg.mTxMaxWriteBytes * 10;
            cfg.mLedgerMaxTxCount = 100;
        });
    // Initial network config has a higher persistent TTL, so we don't expect
    // this state to expire.
    auto const stateSizeAfterUpgrade =
        test.getApp()
            .getLedgerManager()
            .getSorobanInMemoryStateSizeForTesting();

    std::vector<TestAccount> accounts;

    for (size_t i = 0; i < keyCount; ++i)
    {
        accounts.emplace_back(test.getRoot().create(
            "account" + std::to_string(i),
            test.getApp().getLedgerManager().getLastMinBalance(1000)));
    }
    ContractStorageTestClient client(test);

    auto const initialContractsCreatedLedgerSeq = test.getLCLSeq();
    auto baseStateSize = test.getApp()
                             .getLedgerManager()
                             .getSorobanInMemoryStateSizeForTesting();

    std::vector<std::pair<std::string, ContractDataDurability>> dataKeys;

    for (size_t i = 0; i < keyCount; ++i)
    {
        auto key = "key" + std::to_string(i);
        dataKeys.emplace_back(key, ContractDataDurability::PERSISTENT);
    }

    auto verifyInMemorySize = [&]() {
        auto expectedSize = baseStateSize;
        for (auto const& [key, durability] : dataKeys)
        {
            auto ledgerKey = client.getContract().getDataKey(
                makeSymbolSCVal(key), durability);
            LedgerSnapshot ls(test.getApp());
            auto le = ls.load(ledgerKey);
            if (le)
            {
                // We only deal with the data entries here, so no need to use
                // ledgerEntrySizeForRent.
                expectedSize += xdr::xdr_size(le.current());
            }
        }
        auto actualSize = test.getApp()
                              .getLedgerManager()
                              .getSorobanInMemoryStateSizeForTesting();
        REQUIRE(actualSize == expectedSize);
        // Make sure there actually were some live contract data entries.
        REQUIRE(actualSize > baseStateSize);
    };

    auto closeLedgerAndVerify =
        [&](std::vector<TransactionFrameBaseConstPtr> const& txs) {
            auto results = closeLedger(test.getApp(), txs);
            REQUIRE(results.results.size() == txs.size());
            for (auto const& res : results.results)
            {
                REQUIRE(isSuccessResult(res.result));
            }
            verifyInMemorySize();
        };

    {
        INFO("put initial entries");
        std::vector<TransactionFrameBaseConstPtr> txs;

        for (int i = 0; i < keyCount; ++i)
        {
            auto invocation =
                client.putInvocation(dataKeys[i].first, dataKeys[i].second, i);
            txs.emplace_back(
                invocation.withExactNonRefundableResourceFee().createTx(
                    &accounts[i]));
        }

        closeLedgerAndVerify(txs);
    }

    {
        INFO("modify entries (increase size)");
        std::vector<TransactionFrameBaseConstPtr> txs;

        for (int i = 0; i < keyCount; ++i)
        {
            auto invocation = client.resizeStorageAndExtendInvocation(
                dataKeys[i].first, 5, 0, 0);
            txs.emplace_back(
                invocation.withExactNonRefundableResourceFee().createTx(
                    &accounts[i]));
        }

        closeLedgerAndVerify(txs);
    }

    {
        INFO("modify entries (increase and decrease size)");
        std::vector<TransactionFrameBaseConstPtr> txs;

        for (int i = 0; i < keyCount; ++i)
        {
            auto invocation = client.resizeStorageAndExtendInvocation(
                dataKeys[i].first, i % 2 == 0 ? 1 : 10, 0, 0);
            txs.emplace_back(
                invocation.withExactNonRefundableResourceFee().createTx(
                    &accounts[i]));
        }

        closeLedgerAndVerify(txs);
    }

    {
        INFO("delete some entries");
        std::vector<TransactionFrameBaseConstPtr> txs;

        for (int i = 0; i < keyCount / 4; ++i)
        {
            auto invocation =
                client.delInvocation(dataKeys[i].first, dataKeys[i].second);
            txs.emplace_back(
                invocation.withExactNonRefundableResourceFee().createTx(
                    &accounts[i]));
        }

        closeLedgerAndVerify(txs);
    }
    {
        INFO("create, update and delete entries");
        std::vector<TransactionFrameBaseConstPtr> txs;

        for (int i = 0; i < keyCount; ++i)
        {
            std::unique_ptr<TestContract::Invocation> invocation;
            if (i < keyCount / 4)
            {
                invocation = std::make_unique<TestContract::Invocation>(
                    client.putInvocation(dataKeys[i].first, dataKeys[i].second,
                                         i));
            }
            else
            {
                if (i % 3 == 0)
                {
                    invocation = std::make_unique<TestContract::Invocation>(
                        client.delInvocation(dataKeys[i].first,
                                             dataKeys[i].second));
                }
                else
                {
                    invocation = std::make_unique<TestContract::Invocation>(
                        client.resizeStorageAndExtendInvocation(
                            dataKeys[i].first, i % 3 == 1 ? 2 : 7, 0, 0));
                }
            }
            txs.emplace_back(
                invocation->withExactNonRefundableResourceFee().createTx(
                    &accounts[i]));
        }

        closeLedgerAndVerify(txs);
    }
    {
        INFO("evict contract entries");
        while (test.getLCLSeq() <
               initialContractsCreatedLedgerSeq + MIN_PERSISTENT_TTL)
        {
            closeLedger(test.getApp());
        }
        // The 'initial' state has been evicted now, so only data entries
        // should remain.
        baseStateSize = stateSizeAfterUpgrade;
        verifyInMemorySize();
    }
    {
        INFO("evict all entries");
        // Close a few more ledgers to let the entries created above to expire.
        for (int i = 0; i < 5; ++i)
        {
            closeLedger(test.getApp());
        }
        REQUIRE(test.getApp()
                    .getLedgerManager()
                    .getSorobanInMemoryStateSizeForTesting() ==
                stateSizeAfterUpgrade);
    }
}

TEST_CASE("readonly ttl bumps across threads and stages",
          "[tx][soroban][archival]")
{
    auto cfg = getTestConfig();
    SorobanTest test(cfg, true, [](SorobanNetworkConfig& sorobanCfg) {
        sorobanCfg.mStateArchivalSettings.minPersistentTTL = 100;
        sorobanCfg.mStateArchivalSettings.maxEntriesToArchive = 100;
    });

    ContractStorageTestClient client(test);

    // Extend contract keys to ensure they don't expire during test
    test.invokeExtendOp(client.getContract().getKeys(), 10'000);

    // Create a single persistent entry that all operations will target
    client.put("key", ContractDataDurability::PERSISTENT, 100);

    auto lk = client.getContract().getDataKey(
        makeSymbolSCVal("key"), ContractDataDurability::PERSISTENT);

    auto verifyTTLMeta = [&](int txIndex, uint32_t expectedStateTTL,
                             uint32_t expectedUpdatedTTL,
                             uint32_t expectedStateLastModified) {
        auto& lm = test.getApp().getLedgerManager();
        auto opMeta = lm.getLastClosedLedgerTxMeta()
                          .at(txIndex)
                          .getLedgerEntryChangesAtOp(0);
        REQUIRE(opMeta.at(0).state().data.ttl().liveUntilLedgerSeq ==
                expectedStateTTL);
        REQUIRE(opMeta.at(0).state().lastModifiedLedgerSeq ==
                expectedStateLastModified);
        REQUIRE(opMeta.at(1).updated().data.ttl().liveUntilLedgerSeq ==
                expectedUpdatedTTL);
        REQUIRE(opMeta.at(1).updated().lastModifiedLedgerSeq ==
                test.getLCLSeq());
    };

    SECTION("multiple readonly ttl bumps same stage")
    {
        // Create multiple extend operations targeting the same key with
        // readonly footprints
        auto a1 = test.getRoot().create("extend1", 500000000);
        auto a2 = test.getRoot().create("extend2", 500000000);
        auto a3 = test.getRoot().create("extend3", 500000000);

        SorobanResources extendResources;
        extendResources.footprint.readOnly = {lk};
        extendResources.instructions = 0;
        extendResources.diskReadBytes = 1000;

        auto extendTx1 =
            test.createExtendOpTx(extendResources, 500, 1'000, 50'000, &a1);
        auto extendTx2 =
            test.createExtendOpTx(extendResources, 600, 1'000, 50'000, &a2);
        auto extendTx3 =
            test.createExtendOpTx(extendResources, 700, 1'000, 50'000, &a3);

        auto startingTTL = test.getTTL(lk);

        // Capture the TTL entry's lastModifiedLedgerSeq before tx execution
        LedgerSnapshot ls(test.getApp());
        auto ttlKey = getTTLKey(lk);
        auto ttlEntry = ls.load(ttlKey);
        REQUIRE(ttlEntry);
        uint32_t ttlLastModifiedBeforeTx =
            ttlEntry.current().lastModifiedLedgerSeq;

        // Execute all extends in the same stage
        auto r = closeLedger(test.getApp(), {extendTx1, extendTx2, extendTx3},
                             /*strictOrder=*/true);
        REQUIRE(r.results.size() == 3);
        checkTx(0, r, txSUCCESS);
        checkTx(1, r, txSUCCESS);
        checkTx(2, r, txSUCCESS);

        // Verify TTL meta for each transaction
        // All transactions in same stage see the same starting state
        verifyTTLMeta(0, startingTTL, test.getLCLSeq() + 500,
                      ttlLastModifiedBeforeTx);
        verifyTTLMeta(1, startingTTL, test.getLCLSeq() + 600,
                      ttlLastModifiedBeforeTx);
        verifyTTLMeta(2, startingTTL, test.getLCLSeq() + 700,
                      ttlLastModifiedBeforeTx);

        // The final TTL should be based on the last extend operation
        REQUIRE(test.getTTL(lk) == test.getLCLSeq() + 700);
    }

    SECTION("multiple readonly ttl bumps across stages")
    {
        auto a1 = test.getRoot().create("extend1", 500000000);
        auto a2 = test.getRoot().create("extend2", 500000000);
        auto a3 = test.getRoot().create("extend3", 500000000);

        SorobanResources extendResources;
        extendResources.footprint.readOnly = {lk};
        extendResources.instructions = 0;
        extendResources.diskReadBytes = 1000;

        auto extendTx1 =
            test.createExtendOpTx(extendResources, 800, 1'000, 50'000, &a1);
        auto extendTx2 =
            test.createExtendOpTx(extendResources, 900, 1'000, 50'000, &a2);
        auto extendTx3 =
            test.createExtendOpTx(extendResources, 1000, 1'000, 50'000, &a3);

        auto startingTTL = test.getTTL(lk);

        // Capture the TTL entry's lastModifiedLedgerSeq before tx execution
        LedgerSnapshot ls(test.getApp());
        auto ttlKey = getTTLKey(lk);
        auto ttlEntry = ls.load(ttlKey);
        REQUIRE(ttlEntry);
        uint32_t ttlLastModifiedBeforeTx =
            ttlEntry.current().lastModifiedLedgerSeq;

        // Execute extends across different stages
        auto r = closeLedger(test.getApp(), {extendTx1, extendTx2, extendTx3},
                             {{{0}}, {{1}}, {{2}}});
        REQUIRE(r.results.size() == 3);
        checkTx(0, r, txSUCCESS);
        checkTx(1, r, txSUCCESS);
        checkTx(2, r, txSUCCESS);

        // Verify TTL meta for each stage
        // Stage 0: sees original state
        verifyTTLMeta(0, startingTTL, test.getLCLSeq() + 800,
                      ttlLastModifiedBeforeTx);
        // Stage 1: sees state modified by stage 0
        verifyTTLMeta(1, test.getLCLSeq() + 800, test.getLCLSeq() + 900,
                      test.getLCLSeq());
        // Stage 2: sees state modified by stage 1
        verifyTTLMeta(2, test.getLCLSeq() + 900, test.getLCLSeq() + 1000,
                      test.getLCLSeq());

        // The final TTL should be based on the last extend operation
        REQUIRE(test.getTTL(lk) == test.getLCLSeq() + 1000);
    }

    SECTION("readonly ttl bumps with mixed stages")
    {
        auto a1 = test.getRoot().create("extend1", 500000000);
        auto a2 = test.getRoot().create("extend2", 500000000);
        auto a3 = test.getRoot().create("extend3", 500000000);
        auto a4 = test.getRoot().create("extend4", 500000000);

        SorobanResources extendResources;
        extendResources.footprint.readOnly = {lk};
        extendResources.instructions = 0;
        extendResources.diskReadBytes = 1000;

        auto extendTx1 =
            test.createExtendOpTx(extendResources, 200, 1'000, 50'000, &a1);
        auto extendTx2 =
            test.createExtendOpTx(extendResources, 250, 1'000, 50'000, &a2);
        auto extendTx3 =
            test.createExtendOpTx(extendResources, 300, 1'000, 50'000, &a3);
        auto extendTx4 =
            test.createExtendOpTx(extendResources, 350, 1'000, 50'000, &a4);

        auto startingTTL = test.getTTL(lk);

        // Capture the TTL entry's lastModifiedLedgerSeq before tx execution
        LedgerSnapshot ls(test.getApp());
        auto ttlKey = getTTLKey(lk);
        auto ttlEntry = ls.load(ttlKey);
        REQUIRE(ttlEntry);
        uint32_t ttlLastModifiedBeforeTx =
            ttlEntry.current().lastModifiedLedgerSeq;

        // Execute with mixed staging: some in same stage, some across stages
        auto r = closeLedger(test.getApp(),
                             {extendTx1, extendTx2, extendTx3, extendTx4},
                             {{{0, 1}}, {{2}}, {{3}}});
        REQUIRE(r.results.size() == 4);
        checkTx(0, r, txSUCCESS);
        checkTx(1, r, txSUCCESS);
        checkTx(2, r, txSUCCESS);
        checkTx(3, r, txSUCCESS);

        // Verify TTL meta for mixed stages
        // First stage: tx0 and tx1 in same stage - both see original state
        verifyTTLMeta(0, startingTTL, test.getLCLSeq() + 200,
                      ttlLastModifiedBeforeTx);
        verifyTTLMeta(1, startingTTL, test.getLCLSeq() + 250,
                      ttlLastModifiedBeforeTx);

        // Second stage: tx2 alone - sees state from stage 0 (tx1's update)
        verifyTTLMeta(2, test.getLCLSeq() + 250, test.getLCLSeq() + 300,
                      test.getLCLSeq());

        // Third stage: tx3 alone - sees state from stage 1
        verifyTTLMeta(3, test.getLCLSeq() + 300, test.getLCLSeq() + 350,
                      test.getLCLSeq());

        // The final TTL should be based on the last extend operation
        REQUIRE(test.getTTL(lk) == test.getLCLSeq() + 350);
    }

    SECTION("extend_persistent contract function across stages")
    {
        auto a1 = test.getRoot().create("extend1", 500000000);
        auto a2 = test.getRoot().create("extend2", 500000000);
        auto a3 = test.getRoot().create("extend3", 500000000);

        // Use contract's extend_persistent function on the same key
        auto extendSpec =
            client.readKeySpec("key", ContractDataDurability::PERSISTENT);

        auto extendInvocation1 = client.getContract().prepareInvocation(
            "extend_persistent",
            {makeSymbolSCVal("key"), makeU32SCVal(150), makeU32SCVal(150)},
            extendSpec);
        auto extendTx1 =
            extendInvocation1.withExactNonRefundableResourceFee().createTx(&a1);

        auto extendInvocation2 = client.getContract().prepareInvocation(
            "extend_persistent",
            {makeSymbolSCVal("key"), makeU32SCVal(200), makeU32SCVal(200)},
            extendSpec);
        auto extendTx2 =
            extendInvocation2.withExactNonRefundableResourceFee().createTx(&a2);

        auto extendInvocation3 = client.getContract().prepareInvocation(
            "extend_persistent",
            {makeSymbolSCVal("key"), makeU32SCVal(250), makeU32SCVal(250)},
            extendSpec);
        auto extendTx3 =
            extendInvocation3.withExactNonRefundableResourceFee().createTx(&a3);

        auto startingTTL = test.getTTL(lk);

        // Execute across stages
        auto r = closeLedger(test.getApp(), {extendTx1, extendTx2, extendTx3},
                             {{{0}}, {{1}}, {{2}}});
        REQUIRE(r.results.size() == 3);
        checkTx(0, r, txSUCCESS);
        checkTx(1, r, txSUCCESS);
        checkTx(2, r, txSUCCESS);

        // Verify liveUntilLedgerSeq in meta for each stage
        auto& lm = test.getApp().getLedgerManager();
        auto opMeta0 =
            lm.getLastClosedLedgerTxMeta().at(0).getLedgerEntryChangesAtOp(0);
        REQUIRE(opMeta0.at(0).state().data.ttl().liveUntilLedgerSeq ==
                startingTTL);
        REQUIRE(opMeta0.at(1).updated().data.ttl().liveUntilLedgerSeq ==
                test.getLCLSeq() + 150);

        auto opMeta1 =
            lm.getLastClosedLedgerTxMeta().at(1).getLedgerEntryChangesAtOp(0);
        REQUIRE(opMeta1.at(0).state().data.ttl().liveUntilLedgerSeq ==
                test.getLCLSeq() + 150);
        REQUIRE(opMeta1.at(1).updated().data.ttl().liveUntilLedgerSeq ==
                test.getLCLSeq() + 200);

        auto opMeta2 =
            lm.getLastClosedLedgerTxMeta().at(2).getLedgerEntryChangesAtOp(0);
        REQUIRE(opMeta2.at(0).state().data.ttl().liveUntilLedgerSeq ==
                test.getLCLSeq() + 200);
        REQUIRE(opMeta2.at(1).updated().data.ttl().liveUntilLedgerSeq ==
                test.getLCLSeq() + 250);

        // The final TTL should be based on the last extend operation
        REQUIRE(test.getTTL(lk) == test.getLCLSeq() + 250);
    }
}

TEST_CASE_VERSIONS("validate return values", "[tx][soroban][parallelapply]")
{
    Config cfg = getTestConfig();
    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);

    for_versions_from(20, *app, [&] {
        SorobanTest test(app);
        ContractStorageTestClient client(test);

        auto& lm = test.getApp().getLedgerManager();
        const int64_t startingBalance = lm.getLastMinBalance(50);
        auto& root = test.getRoot();

        // Create test accounts
        auto a1 = root.create("a1", startingBalance);
        auto a2 = root.create("a2", startingBalance);
        auto a3 = root.create("a3", startingBalance);
        auto a4 = root.create("a4", startingBalance);
        auto a5 = root.create("a5", startingBalance);
        auto a6 = root.create("a6", startingBalance);
        auto a7 = root.create("a7", startingBalance);

        REQUIRE(client.put("key2", ContractDataDurability::PERSISTENT, 50) ==
                INVOKE_HOST_FUNCTION_SUCCESS);

        // Stage 0
        auto putTx1 = client.getContract()
                          .prepareInvocation(
                              "put_persistent",
                              {makeSymbolSCVal("key1"), makeU64SCVal(100)},
                              client.writeKeySpec(
                                  "key1", ContractDataDurability::PERSISTENT))
                          .withExactNonRefundableResourceFee()
                          .createTx(&a1);

        auto putTx2 = client.getContract()
                          .prepareInvocation(
                              "put_persistent",
                              {makeSymbolSCVal("key2"), makeU64SCVal(200)},
                              client.writeKeySpec(
                                  "key2", ContractDataDurability::PERSISTENT))
                          .withExactNonRefundableResourceFee()
                          .createTx(&a2);

        auto getTx1 = client.getContract()
                          .prepareInvocation(
                              "get_persistent", {makeSymbolSCVal("key2")},
                              client.readKeySpec(
                                  "key2", ContractDataDurability::PERSISTENT))
                          .withExactNonRefundableResourceFee()
                          .createTx(&a3);

        // Stage 1
        auto getTx2 = client.getContract()
                          .prepareInvocation(
                              "get_persistent", {makeSymbolSCVal("key1")},
                              client.readKeySpec(
                                  "key1", ContractDataDurability::PERSISTENT))
                          .withExactNonRefundableResourceFee()
                          .createTx(&a4);

        auto getTx3 = client.getContract()
                          .prepareInvocation(
                              "get_persistent", {makeSymbolSCVal("key2")},
                              client.readKeySpec(
                                  "key2", ContractDataDurability::PERSISTENT))
                          .withExactNonRefundableResourceFee()
                          .createTx(&a5);

        // Stage 2
        auto updateTx = client.getContract()
                            .prepareInvocation(
                                "put_persistent",
                                {makeSymbolSCVal("key1"), makeU64SCVal(300)},
                                client.writeKeySpec(
                                    "key1", ContractDataDurability::PERSISTENT))
                            .withExactNonRefundableResourceFee()
                            .createTx(&a6);

        auto getTx4 = client.getContract()
                          .prepareInvocation(
                              "get_persistent", {makeSymbolSCVal("key1")},
                              client.readKeySpec(
                                  "key1", ContractDataDurability::PERSISTENT))
                          .withExactNonRefundableResourceFee()
                          .createTx(&a7);

        auto r = closeLedger(
            test.getApp(),
            {getTx1, putTx1, putTx2, getTx2, getTx3, updateTx, getTx4},
            {{{0, 2}, {1}}, {{3}, {4}}, {{5, 6}}});

        REQUIRE(r.results.size() == 7);
        checkResults(r, 7, 0);

        // Verify return values through metadata
        auto const& txMetas = lm.getLastClosedLedgerTxMeta();

        // getTx1 should return the state from the last ledger for key2
        auto getTx1Meta = txMetas.at(0);
        auto getTx1ReturnValue = getTx1Meta.getReturnValue();
        REQUIRE(getTx1ReturnValue.u64() == 50);

        // getTx2 should return the key1 value after it was updated
        auto getTx2Meta = txMetas.at(3);
        auto getTx2ReturnValue = getTx2Meta.getReturnValue();
        REQUIRE(getTx2ReturnValue.u64() == 100);

        // getTx3 should return the key2 value after it was updated
        auto getTx3Meta = txMetas.at(4);
        auto getTx3ReturnValue = getTx3Meta.getReturnValue();
        REQUIRE(getTx3ReturnValue.u64() == 200);

        // getTx4 should return 300 (updated key1 value)
        auto getTx4Meta = txMetas.at(6);
        auto getUpdatedReturnValue = getTx4Meta.getReturnValue();
        REQUIRE(getUpdatedReturnValue.u64() == 300);

        // Additional verification: Check that the storage actually contains the
        // expected values
        REQUIRE(client.get("key1", ContractDataDurability::PERSISTENT, 300) ==
                INVOKE_HOST_FUNCTION_SUCCESS);
        REQUIRE(client.get("key2", ContractDataDurability::PERSISTENT, 200) ==
                INVOKE_HOST_FUNCTION_SUCCESS);
    });
}

// Test that autorestore works when keys aren't explicitly written and belong to
// another uncalled contractID.
TEST_CASE("autorestore from another contract", "[tx][soroban][archival]")
{
    auto cfg = getTestConfig();
    cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS = true;
    SorobanTest test(cfg, true, [](SorobanNetworkConfig& sorobanCfg) {
        sorobanCfg.mStateArchivalSettings.minPersistentTTL =
            MinimumSorobanNetworkConfig::MINIMUM_PERSISTENT_ENTRY_LIFETIME;
        sorobanCfg.mStateArchivalSettings.startingEvictionScanLevel = 1;
        sorobanCfg.mStateArchivalSettings.evictionScanSize = 1'000'000;

        // Never snapshot bucket list so we have stable rent fees
        sorobanCfg.mStateArchivalSettings
            .liveSorobanStateSizeWindowSamplePeriod = 10'000;
    });

    // Deploy two separate contracts
    ContractStorageTestClient client1(test);
    ContractStorageTestClient client2(test);
    client1.put("key1", ContractDataDurability::PERSISTENT, 111);
    client2.put("key2", ContractDataDurability::PERSISTENT, 222);

    // Extend the contract instances so they don't get archived
    auto extendLedgers =
        MinimumSorobanNetworkConfig::MINIMUM_PERSISTENT_ENTRY_LIFETIME + 10000;
    test.invokeExtendOp(client1.getContract().getKeys(), extendLedgers);
    test.invokeExtendOp(client2.getContract().getKeys(), extendLedgers);

    auto expirationLedger =
        test.getLCLSeq() +
        MinimumSorobanNetworkConfig::MINIMUM_PERSISTENT_ENTRY_LIFETIME;

    // Close ledgers until all entries are expired and evicted
    for (uint32_t ledgerSeq = test.getLCLSeq() + 1;
         ledgerSeq <= expirationLedger + 1; ++ledgerSeq)
    {
        closeLedgerOn(test.getApp(), ledgerSeq, 2, 1, 2016);
    }

    auto lk1 = client1.getContract().getDataKey(
        makeSymbolSCVal("key1"), ContractDataDurability::PERSISTENT);
    auto lk2 = client2.getContract().getDataKey(
        makeSymbolSCVal("key2"), ContractDataDurability::PERSISTENT);

    // Verify entries are archived
    REQUIRE(client1.get("key1", ContractDataDurability::PERSISTENT,
                        std::nullopt) == INVOKE_HOST_FUNCTION_ENTRY_ARCHIVED);
    REQUIRE(client2.get("key2", ContractDataDurability::PERSISTENT,
                        std::nullopt) == INVOKE_HOST_FUNCTION_ENTRY_ARCHIVED);

    auto hotArchiveSnapshot = test.getApp()
                                  .getBucketManager()
                                  .getBucketSnapshotManager()
                                  .copySearchableHotArchiveBucketListSnapshot();
    auto liveSnapshot = test.getApp()
                            .getBucketManager()
                            .getBucketSnapshotManager()
                            .copySearchableLiveBucketListSnapshot();

    REQUIRE(hotArchiveSnapshot->loadKeys({lk1, lk2}).size() == 2);
    REQUIRE(liveSnapshot->loadKeys({lk1, lk2}, "load").size() == 0);

    // Now, invoke contract2, but also autorestore state from contract1.
    auto keysToRestore = client2.getContract().getKeys();
    keysToRestore.push_back(lk2);
    keysToRestore.push_back(lk1);

    std::vector<uint32_t> archivedIndexes;
    for (size_t i = 0; i < keysToRestore.size(); ++i)
    {
        archivedIndexes.push_back(i);
    }

    auto spec = client2.defaultSpecWithoutFootprint()
                    .setReadWriteFootprint(keysToRestore)
                    .setArchivedIndexes(archivedIndexes)
                    .setWriteBytes(10000)
                    .setRefundableResourceFee(100'000);

    // Make sure both keys are properly restored
    auto invocation = client2.getContract().prepareInvocation(
        "get_persistent", {makeSymbolSCVal("key2")}, spec,
        /*addContractKeys=*/false);
    REQUIRE(invocation.withExactNonRefundableResourceFee().invoke());

    liveSnapshot = test.getApp()
                       .getBucketManager()
                       .getBucketSnapshotManager()
                       .copySearchableLiveBucketListSnapshot();
    hotArchiveSnapshot = test.getApp()
                             .getBucketManager()
                             .getBucketSnapshotManager()
                             .copySearchableHotArchiveBucketListSnapshot();

    REQUIRE(liveSnapshot->loadKeys({lk1, lk2}, "load").size() == 2);
    REQUIRE(hotArchiveSnapshot->loadKeys({lk1, lk2}).size() == 0);

    // Verify that the correct values were restored
    REQUIRE(client1.get("key1", ContractDataDurability::PERSISTENT, 111) ==
            INVOKE_HOST_FUNCTION_SUCCESS);
    REQUIRE(client2.get("key2", ContractDataDurability::PERSISTENT, 222) ==
            INVOKE_HOST_FUNCTION_SUCCESS);
}

TEST_CASE_VERSIONS("fee bump inner account merged then used as inner account "
                   "on soroban fee bump",
                   "[tx][soroban][merge][feebump]")
{
    Config cfg = getTestConfig();

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);

    // From 21 so we don't have to handle the feeCharged issues from v20 in this
    // test.
    for_versions_from(21, *app, [&] {
        SorobanTest test(app);

        const int64_t startingBalance =
            test.getApp().getLedgerManager().getLastMinBalance(50);

        auto innerAccount = test.getRoot().create("inner", startingBalance);
        auto feeBumper = test.getRoot().create("feeBumper", startingBalance);
        auto destination =
            test.getRoot().create("destination", startingBalance);
        auto secondFeeBumper =
            test.getRoot().create("feeBumper2", startingBalance);

        auto wasm = rust_bridge::get_test_wasm_add_i32();
        auto resources = defaultUploadWasmResourcesWithoutFootprint(
            wasm, test.getLedgerVersion());

        // Create classic transaction that merges innerAccount into destination
        auto mergeOp = accountMerge(destination);
        mergeOp.sourceAccount.activate() = toMuxedAccount(innerAccount);
        auto classicMergeTx = destination.tx({mergeOp});
        classicMergeTx->addSignature(innerAccount.getSecretKey());

        // Fee bump the merge transaction with feeBumper
        int64_t mergeFee = classicMergeTx->getEnvelope().v1().tx.fee * 5;
        auto feeBumpMergeTx =
            feeBump(test.getApp(), feeBumper, classicMergeTx, mergeFee,
                    /*useInclusionAsFullFee=*/true);

        // Create soroban transaction using the account that will be merged
        auto sorobanTx = makeSorobanWasmUploadTx(test.getApp(), innerAccount,
                                                 wasm, resources, 100);

        // Create fee bump transaction wrapping the soroban tx with the merged
        // account as inner
        int64_t feeBumpFullFee = sorobanTx->getEnvelope().v1().tx.fee * 5;
        auto feeBumpSorobanTx =
            feeBump(test.getApp(), secondFeeBumper, sorobanTx, feeBumpFullFee,
                    /*useInclusionAsFullFee=*/true);

        std::vector<TransactionFrameBasePtr> txs = {feeBumpMergeTx,
                                                    feeBumpSorobanTx};
        auto r = closeLedger(test.getApp(), txs);
        REQUIRE(r.results.size() == 2);

        checkTx(0, r, txFEE_BUMP_INNER_SUCCESS);
        checkTx(1, r, txFEE_BUMP_INNER_FAILED);

        auto const& innerRes =
            r.results[1].result.result.innerResultPair().result;
        REQUIRE(innerRes.result.code() == txNO_ACCOUNT);

        // Verify that innerAccount no longer exists after the merge
        LedgerSnapshot ls(test.getApp());
        REQUIRE(!ls.getAccount(innerAccount.getPublicKey()));

        auto expectedDestinationBalance = startingBalance + startingBalance;
        REQUIRE(destination.getBalance() == expectedDestinationBalance);

        // Verify feeBumper paid the fee for the merge transaction
        REQUIRE(feeBumper.getBalance() ==
                startingBalance - r.results.at(0).result.feeCharged);

        // Verify secondFeeBumper paid the fee for the failed soroban
        // transaction
        REQUIRE(secondFeeBumper.getBalance() ==
                startingBalance - r.results.at(1).result.feeCharged);
    });
}

TEST_CASE_VERSIONS("classic payment to soroban fee bump account",
                   "[tx][soroban][feebump]")
{
    Config cfg = getTestConfig();
    cfg.EMIT_CLASSIC_EVENTS = true;
    cfg.BACKFILL_STELLAR_ASSET_EVENTS = true;

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);

    for_versions_from(21, *app, [&] {
        SorobanTest test(app);
        auto ledgerVersion = test.getLedgerVersion();

        const int64_t startingBalance =
            test.getApp().getLedgerManager().getLastMinBalance(50);

        // Create accounts
        auto root = test.getRoot();
        auto paymentSender = root.create("paymentSender", startingBalance);
        auto feeBumpAccount = root.create("feeBumpAccount", startingBalance);
        auto sorobanAccount = root.create("sorobanAccount", startingBalance);

        auto feeBumpAccountStartingBalance = feeBumpAccount.getBalance();
        auto sorobanAccountStartingBalance = sorobanAccount.getBalance();

        // Record initial sequence numbers
        auto paymentSenderStartingSeq = paymentSender.loadSequenceNumber();
        auto feeBumpAccountStartingSeq = feeBumpAccount.loadSequenceNumber();
        auto sorobanAccountStartingSeq = sorobanAccount.loadSequenceNumber();

        // Step 1: Create classic payment to the fee bump account
        auto classicPaymentTx =
            paymentSender.tx({payment(feeBumpAccount, 1000000)});

        // Step 2: Create a Soroban transaction with the soroban account as
        // source
        auto wasm = rust_bridge::get_test_wasm_add_i32();
        auto resources =
            defaultUploadWasmResourcesWithoutFootprint(wasm, ledgerVersion);
        auto sorobanTx = makeSorobanWasmUploadTx(test.getApp(), sorobanAccount,
                                                 wasm, resources, 1000);

        // Step 3: Use the fee bump account (that will receive the payment) as
        // the fee bump source
        int64_t feeBumpFullFee = sorobanTx->getEnvelope().v1().tx.fee * 5;
        auto feeBumpTx =
            feeBump(test.getApp(), feeBumpAccount, sorobanTx, feeBumpFullFee,
                    /*useInclusionAsFullFee=*/true);

        // Execute both transactions in the same ledger
        std::vector<TransactionFrameBasePtr> txs = {classicPaymentTx,
                                                    feeBumpTx};
        auto result = closeLedger(test.getApp(), txs);
        REQUIRE(result.results.size() == 2);
        checkTx(0, result, txSUCCESS);                // Classic payment
        checkTx(1, result, txFEE_BUMP_INNER_SUCCESS); // Fee bump soroban

        // Verify sequence numbers
        REQUIRE(paymentSender.loadSequenceNumber() ==
                paymentSenderStartingSeq + 1); // Payment sender seq incremented
        REQUIRE(sorobanAccount.loadSequenceNumber() ==
                sorobanAccountStartingSeq +
                    1); // Soroban account seq incremented
        REQUIRE(feeBumpAccount.loadSequenceNumber() ==
                feeBumpAccountStartingSeq); // Fee bump account seq unchanged

        // Verify the soroban account balance is unchanged (didn't pay fee)
        REQUIRE(sorobanAccount.getBalance() == sorobanAccountStartingBalance);

        // The fee bump account should have received payment and paid the
        // soroban fee
        auto finalFeeBumpBalance = feeBumpAccount.getBalance();
        auto expectedBalance = feeBumpAccountStartingBalance + 1000000 -
                               result.results[1].result.feeCharged;
        REQUIRE(finalFeeBumpBalance == expectedBalance);
    });
}

TEST_CASE_VERSIONS("classic payment source same as soroban fee bump source",
                   "[tx][soroban][feebump]")
{
    Config cfg = getTestConfig();
    cfg.EMIT_CLASSIC_EVENTS = true;
    cfg.BACKFILL_STELLAR_ASSET_EVENTS = true;

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);

    for_versions_from(21, *app, [&] {
        SorobanTest test(app);
        auto ledgerVersion = test.getLedgerVersion();

        const int64_t startingBalance =
            test.getApp().getLedgerManager().getLastMinBalance(50);

        // Create accounts
        auto root = test.getRoot();
        auto sharedAccount = root.create("sharedAccount", startingBalance);
        auto paymentRecipient =
            root.create("paymentRecipient", startingBalance);
        auto sorobanInnerAccount = root.create("sorobanInner", startingBalance);

        auto sharedAccountStartingBalance = sharedAccount.getBalance();
        auto paymentRecipientStartingBalance = paymentRecipient.getBalance();
        auto sorobanInnerAccountStartingBalance =
            sorobanInnerAccount.getBalance();

        // Record initial sequence numbers
        auto sharedAccountStartingSeq = sharedAccount.loadSequenceNumber();
        auto paymentRecipientStartingSeq =
            paymentRecipient.loadSequenceNumber();
        auto sorobanInnerAccountStartingSeq =
            sorobanInnerAccount.loadSequenceNumber();

        // Step 1: Create classic payment from shared account
        auto classicPaymentTx =
            sharedAccount.tx({payment(paymentRecipient, 750000)});

        // Step 2: Create Soroban transaction with a different inner source
        // account
        auto wasm = rust_bridge::get_test_wasm_add_i32();
        auto resources =
            defaultUploadWasmResourcesWithoutFootprint(wasm, ledgerVersion);
        auto sorobanTx = makeSorobanWasmUploadTx(
            test.getApp(), sorobanInnerAccount, wasm, resources, 1000);

        // Step 3: Fee bump the Soroban transaction with the same shared account
        // (that also did the payment)
        int64_t feeBumpFullFee = sorobanTx->getEnvelope().v1().tx.fee * 5;
        auto feeBumpTx =
            feeBump(test.getApp(), sharedAccount, sorobanTx, feeBumpFullFee,
                    /*useInclusionAsFullFee=*/true);

        // Execute both transactions in the same ledger
        std::vector<TransactionFrameBasePtr> txs = {classicPaymentTx,
                                                    feeBumpTx};
        auto result = closeLedger(test.getApp(), txs);
        REQUIRE(result.results.size() == 2);
        checkTx(0, result, txSUCCESS);                // Classic payment
        checkTx(1, result, txFEE_BUMP_INNER_SUCCESS); // Fee bump soroban

        // Verify sequence numbers
        REQUIRE(sharedAccount.loadSequenceNumber() ==
                sharedAccountStartingSeq +
                    1); // Only classic payment increments seq
        REQUIRE(paymentRecipient.loadSequenceNumber() ==
                paymentRecipientStartingSeq); // Recipient seq unchanged
        REQUIRE(sorobanInnerAccount.loadSequenceNumber() ==
                sorobanInnerAccountStartingSeq +
                    1); // Inner soroban account seq incremented

        // Verify balances
        // Payment recipient should have received the payment
        REQUIRE(paymentRecipient.getBalance() ==
                paymentRecipientStartingBalance + 750000);

        // Soroban inner account balance should be unchanged (fee bump pays the
        // fee)
        REQUIRE(sorobanInnerAccount.getBalance() ==
                sorobanInnerAccountStartingBalance);

        // Shared account should have paid for payment amount, classic payment
        // fee, and soroban fee
        auto expectedSharedBalance =
            sharedAccountStartingBalance - 750000 - // Payment amount
            result.results[0].result.feeCharged -   // Classic payment fee
            result.results[1].result.feeCharged;    // Soroban fee
        REQUIRE(sharedAccount.getBalance() == expectedSharedBalance);
    });
}

TEST_CASE_VERSIONS(
    "classic phase sets master weight of soroban source account to 0",
    "[tx][soroban][signer]")
{
    Config cfg = getTestConfig();
    cfg.EMIT_CLASSIC_EVENTS = true;
    cfg.BACKFILL_STELLAR_ASSET_EVENTS = true;

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);

    for_versions_from(20, *app, [&] {
        SorobanTest test(app);
        auto ledgerVersion = test.getLedgerVersion();

        const int64_t startingBalance =
            test.getApp().getLedgerManager().getLastMinBalance(50);

        // Create accounts
        auto root = test.getRoot();
        auto sorobanSourceAccount =
            root.create("sorobanSource", startingBalance);
        auto signerAdmin = root.create("signerAdmin", startingBalance);

        auto sorobanSourceStartingBalance = sorobanSourceAccount.getBalance();
        auto signerAdminStartingBalance = signerAdmin.getBalance();

        // Record initial sequence numbers
        auto sorobanSourceStartingSeq =
            sorobanSourceAccount.loadSequenceNumber();
        auto signerAdminStartingSeq = signerAdmin.loadSequenceNumber();

        // Step 1: Classic transaction to add signer to the soroban source
        // account
        auto setOptionsOp = txtest::setOptions(setMasterWeight(0));
        setOptionsOp.sourceAccount.activate() =
            toMuxedAccount(sorobanSourceAccount);
        auto classicSetOptionsTx = signerAdmin.tx({setOptionsOp});
        classicSetOptionsTx->addSignature(sorobanSourceAccount.getSecretKey());

        // Step 2: Create Soroban transaction with the account that just got a
        // new signer
        auto wasm = rust_bridge::get_test_wasm_add_i32();
        auto resources =
            defaultUploadWasmResourcesWithoutFootprint(wasm, ledgerVersion);
        auto sorobanTx = makeSorobanWasmUploadTx(
            test.getApp(), sorobanSourceAccount, wasm, resources, 1000);

        // Execute both transactions in the same ledger
        std::vector<TransactionFrameBasePtr> txs = {classicSetOptionsTx,
                                                    sorobanTx};
        auto result = closeLedger(test.getApp(), txs);
        REQUIRE(result.results.size() == 2);
        checkTx(0, result, txSUCCESS);  // Classic set options
        checkTx(1, result, txBAD_AUTH); // Soroban transaction

        // Verify sequence numbers
        REQUIRE(signerAdmin.loadSequenceNumber() ==
                signerAdminStartingSeq + 1); // Set options tx increments seq
        REQUIRE(sorobanSourceAccount.loadSequenceNumber() ==
                sorobanSourceStartingSeq + 1); // Soroban tx increments seq

        // Verify balances
        // Signer admin should have paid the set options fee
        REQUIRE(signerAdmin.getBalance() ==
                signerAdminStartingBalance -
                    result.results[0].result.feeCharged);

        // Soroban source account should have paid the soroban fee
        auto expectedSorobanBalance =
            sorobanSourceStartingBalance - result.results[1].result.feeCharged;
        REQUIRE(sorobanSourceAccount.getBalance() == expectedSorobanBalance);

        auto txm =
            TransactionMetaFrame(test.getLastLcm().getTransactionMeta(1));
        // Verify refund amount in transaction meta
        auto refundChanges =
            protocolVersionIsBefore(test.getLedgerVersion(),
                                    PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION)
                ? txm.getChangesAfter()
                : test.getLastLcm().getPostTxApplyFeeProcessing(
                      1); // Index 1 for soroban tx

        REQUIRE(refundChanges.size() == 2);
        auto refund = refundChanges[1].updated().data.account().balance -
                      refundChanges[0].state().data.account().balance;
        REQUIRE(refund > 0);

        auto initialFeeChanges =
            test.getLastLcm().getPreTxApplyFeeProcessing(1);
        auto initialFee = initialFeeChanges[0].state().data.account().balance -
                          initialFeeChanges[1].updated().data.account().balance;

        auto const& txEvents = txm.getTxEvents();
        REQUIRE(txEvents.size() == 2);
        validateFeeEvent(txEvents[0], sorobanSourceAccount.getPublicKey(),
                         initialFee, ledgerVersion, false);
        validateFeeEvent(txEvents[1], sorobanSourceAccount.getPublicKey(),
                         -refund, ledgerVersion, true);
    });
}

TEST_CASE_VERSIONS("classic phase bumps sequence of soroban source account",
                   "[tx][soroban][bumpsequence]")
{
    Config cfg = getTestConfig();
    cfg.EMIT_CLASSIC_EVENTS = true;
    cfg.BACKFILL_STELLAR_ASSET_EVENTS = true;

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);

    for_versions_from(20, *app, [&] {
        SorobanTest test(app);
        auto ledgerVersion = test.getLedgerVersion();

        const int64_t startingBalance =
            test.getApp().getLedgerManager().getLastMinBalance(50);

        // Create accounts
        auto root = test.getRoot();
        auto sorobanSourceAccount =
            root.create("sorobanSource", startingBalance);
        auto bumpAdmin = root.create("bumpAdmin", startingBalance);

        auto sorobanSourceStartingBalance = sorobanSourceAccount.getBalance();
        auto bumpAdminStartingBalance = bumpAdmin.getBalance();

        // Record initial sequence numbers
        auto sorobanSourceStartingSeq =
            sorobanSourceAccount.loadSequenceNumber();
        auto bumpAdminStartingSeq = bumpAdmin.loadSequenceNumber();

        // Step 1: Classic transaction to bump sequence of the soroban source
        // account
        auto targetSequence = sorobanSourceStartingSeq + 10;
        auto bumpSequenceOp = txtest::bumpSequence(targetSequence);
        bumpSequenceOp.sourceAccount.activate() =
            toMuxedAccount(sorobanSourceAccount);
        auto classicBumpTx = bumpAdmin.tx({bumpSequenceOp});
        classicBumpTx->addSignature(sorobanSourceAccount.getSecretKey());

        // Step 2: Create Soroban transaction with the account that just had its
        // sequence bumped
        auto wasm = rust_bridge::get_test_wasm_add_i32();
        auto resources =
            defaultUploadWasmResourcesWithoutFootprint(wasm, ledgerVersion);
        auto sorobanTx = makeSorobanWasmUploadTx(
            test.getApp(), sorobanSourceAccount, wasm, resources, 1000);

        // Execute both transactions in the same ledger
        std::vector<TransactionFrameBasePtr> txs = {classicBumpTx, sorobanTx};
        auto result = closeLedger(test.getApp(), txs);
        REQUIRE(result.results.size() == 2);
        checkTx(0, result, txSUCCESS); // Classic bump sequence
        checkTx(1, result, txBAD_SEQ); // Soroban transaction

        // Verify sequence numbers
        REQUIRE(bumpAdmin.loadSequenceNumber() == bumpAdminStartingSeq + 1);
        REQUIRE(sorobanSourceAccount.loadSequenceNumber() == targetSequence);

        // Verify balances
        // Bump admin should have paid the bump sequence fee
        REQUIRE(bumpAdmin.getBalance() ==
                bumpAdminStartingBalance - result.results[0].result.feeCharged);

        // Soroban source account should have paid the soroban fee
        auto expectedSorobanBalance =
            sorobanSourceStartingBalance - result.results[1].result.feeCharged;
        REQUIRE(sorobanSourceAccount.getBalance() == expectedSorobanBalance);

        auto txm =
            TransactionMetaFrame(test.getLastLcm().getTransactionMeta(1));
        // Verify refund amount in transaction meta
        auto refundChanges =
            protocolVersionIsBefore(test.getLedgerVersion(),
                                    PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION)
                ? txm.getChangesAfter()
                : test.getLastLcm().getPostTxApplyFeeProcessing(
                      1); // Index 1 for soroban tx

        REQUIRE(refundChanges.size() == 2);
        auto refund = refundChanges[1].updated().data.account().balance -
                      refundChanges[0].state().data.account().balance;
        REQUIRE(refund > 0);

        auto initialFeeChanges =
            test.getLastLcm().getPreTxApplyFeeProcessing(1);
        auto initialFee = initialFeeChanges[0].state().data.account().balance -
                          initialFeeChanges[1].updated().data.account().balance;

        auto const& txEvents = txm.getTxEvents();
        REQUIRE(txEvents.size() == 2);
        validateFeeEvent(txEvents[0], sorobanSourceAccount.getPublicKey(),
                         initialFee, ledgerVersion, false);
        validateFeeEvent(txEvents[1], sorobanSourceAccount.getPublicKey(),
                         -refund, ledgerVersion, true);
    });
}