// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/test/BucketTestUtils.h"
#include "invariant/ConservationOfLumens.h"
#include "invariant/InvariantDoesNotHold.h"
#include "invariant/InvariantManager.h"
#include "invariant/test/InvariantTestUtils.h"
#include "ledger/LedgerStateSnapshot.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnHeader.h"
#include "ledger/test/LedgerTestUtils.h"
#include "lib/util/stdrandom.h"
#include "main/Application.h"
#include "test/Catch2.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/test/SorobanTxTestUtils.h"
#include "util/Math.h"
#include <numeric>
#include <xdrpp/autocheck.h>

using namespace stellar;
using namespace stellar::InvariantTestUtils;
using namespace stellar::txtest;

namespace
{
int64_t
getTotalBalance(std::vector<LedgerEntry> const& entries)
{
    return std::accumulate(entries.begin(), entries.end(),
                           static_cast<int64_t>(0),
                           [](int64_t lhs, LedgerEntry const& rhs) {
                               return lhs + rhs.data.account().balance;
                           });
}

int64_t
getCoinsAboveReserve(std::vector<LedgerEntry> const& entries, Application& app)
{
    return std::accumulate(
        entries.begin(), entries.end(), static_cast<int64_t>(0),
        [&app](int64_t lhs, LedgerEntry const& rhs) {
            auto& account = rhs.data.account();
            return lhs + account.balance - getMinBalance(app, account);
        });
}

std::vector<LedgerEntry>
updateBalances(std::vector<LedgerEntry> entries, Application& app,
               int64_t netChange, bool updateTotalCoins)
{
    int64_t initialCoins = getTotalBalance(entries);
    int64_t pool = netChange + getCoinsAboveReserve(entries, app);
    int64_t totalDelta = 0;
    for (auto iter = entries.begin(); iter != entries.end(); ++iter)
    {
        auto& account = iter->data.account();
        auto minBalance = getMinBalance(app, account);
        pool -= account.balance - minBalance;

        int64_t delta = 0;
        if (iter + 1 != entries.end())
        {
            int64_t maxDecrease = minBalance - account.balance;
            int64_t maxIncrease = pool;
            REQUIRE(maxIncrease >= maxDecrease);
            stellar::uniform_int_distribution<int64_t> dist(maxDecrease,
                                                            maxIncrease);
            delta = dist(getGlobalRandomEngine());
        }
        else
        {
            delta = netChange - totalDelta;
        }

        pool -= delta;
        account.balance += delta;
        totalDelta += delta;
        REQUIRE(account.balance >= minBalance);
    }
    REQUIRE(totalDelta == netChange);

    auto finalCoins = getTotalBalance(entries);
    REQUIRE(initialCoins + netChange == finalCoins);

    if (updateTotalCoins)
    {
        LedgerTxn ltx(app.getLedgerTxnRoot());
        auto& current = ltx.loadHeader().current();
        REQUIRE(current.totalCoins >= 0);
        if (netChange > 0)
        {
            REQUIRE(current.totalCoins <= INT64_MAX - netChange);
        }
        current.totalCoins += netChange;
        ltx.commit();
    }
    return entries;
}

std::vector<LedgerEntry>
updateBalances(std::vector<LedgerEntry> const& entries, Application& app)
{
    int64_t coinsAboveReserve = getCoinsAboveReserve(entries, app);

    int64_t totalCoins = 0;
    {
        LedgerTxn ltx(app.getLedgerTxnRoot());
        totalCoins = ltx.loadHeader().current().totalCoins;
    }

    stellar::uniform_int_distribution<int64_t> dist(
        totalCoins - coinsAboveReserve, INT64_MAX);
    int64_t newTotalCoins = dist(getGlobalRandomEngine());
    return updateBalances(entries, app, newTotalCoins - totalCoins, true);
}
} // namespace

TEST_CASE("Total coins change without inflation",
          "[invariant][conservationoflumens]")
{
    Config cfg = getTestConfig(0);
    cfg.INVARIANT_CHECKS = {"ConservationOfLumens"};

    stellar::uniform_int_distribution<int64_t> dist(0, INT64_MAX);

    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);

    LedgerTxn ltx(app->getLedgerTxnRoot());
    ltx.loadHeader().current().totalCoins = dist(getGlobalRandomEngine());
    OperationResult res;
    REQUIRE_THROWS_AS(app->getInvariantManager().checkOnOperationApply(
                          {}, res, ltx.getDelta(), {}, app->getAppConnector()),
                      InvariantDoesNotHold);
}

TEST_CASE("Fee pool change without inflation",
          "[invariant][conservationoflumens]")
{
    Config cfg = getTestConfig(0);
    cfg.INVARIANT_CHECKS = {"ConservationOfLumens"};

    stellar::uniform_int_distribution<int64_t> dist(0, INT64_MAX);

    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);

    LedgerTxn ltx(app->getLedgerTxnRoot());
    ltx.loadHeader().current().feePool = dist(getGlobalRandomEngine());
    OperationResult res;
    REQUIRE_THROWS_AS(app->getInvariantManager().checkOnOperationApply(
                          {}, res, ltx.getDelta(), {}, app->getAppConnector()),
                      InvariantDoesNotHold);
}

TEST_CASE("Account balances changed without inflation",
          "[invariant][conservationoflumens]")
{
    Config cfg = getTestConfig(0, Config::TESTDB_IN_MEMORY);
    cfg.INVARIANT_CHECKS = {"ConservationOfLumens"};

    uint32_t const N = 10;
    for (uint32_t i = 0; i < 100; ++i)
    {
        VirtualClock clock;
        Application::pointer app = createTestApplication(clock, cfg);

        std::vector<LedgerEntry> entries1;
        std::generate_n(std::back_inserter(entries1), N,
                        std::bind(generateRandomAccount, 2));
        entries1 = updateBalances(entries1, *app);
        {
            auto updates = makeUpdateList(entries1, nullptr);
            REQUIRE(!store(*app, updates));
        }

        auto entries2 = updateBalances(entries1, *app);
        {
            auto updates = makeUpdateList(entries2, entries1);
            REQUIRE(!store(*app, updates));
        }

        {
            auto updates = makeUpdateList(nullptr, entries1);
            REQUIRE(!store(*app, updates));
        }
    }
}

TEST_CASE("Account balances unchanged without inflation",
          "[invariant][conservationoflumens]")
{
    Config cfg = getTestConfig(0, Config::TESTDB_IN_MEMORY);
    cfg.INVARIANT_CHECKS = {"ConservationOfLumens"};

    uint32_t const N = 10;
    for (uint32_t i = 0; i < 100; ++i)
    {
        VirtualClock clock;
        Application::pointer app = createTestApplication(clock, cfg);

        std::vector<LedgerEntry> entries1;
        std::generate_n(std::back_inserter(entries1), N,
                        std::bind(generateRandomAccount, 2));
        entries1 = updateBalances(entries1, *app);
        {
            auto updates = makeUpdateList(entries1, nullptr);
            REQUIRE(!store(*app, updates));
        }

        auto entries2 = updateBalances(entries1, *app, 0, false);
        {
            auto updates = makeUpdateList(entries2, entries1);
            REQUIRE(store(*app, updates));
        }

        auto keepEnd = entries2.begin() + N / 2;
        std::vector<LedgerEntry> entries3(entries2.begin(), keepEnd);
        std::vector<LedgerEntry> toDelete(keepEnd, entries2.end());
        int64_t balanceToDelete = getTotalBalance(toDelete);
        auto entries4 = updateBalances(entries3, *app, balanceToDelete, false);
        {
            auto updates = makeUpdateList(entries4, entries3);
            auto updates2 = makeUpdateList(nullptr, toDelete);
            updates.insert(updates.end(), updates2.begin(), updates2.end());
            REQUIRE(store(*app, updates));
        }
    }
}

TEST_CASE("Inflation changes are consistent",
          "[invariant][conservationoflumens]")
{
    Config cfg = getTestConfig(0, Config::TESTDB_IN_MEMORY);
    cfg.INVARIANT_CHECKS = {"ConservationOfLumens"};
    stellar::uniform_int_distribution<uint32_t> payoutsDist(1, 100);
    stellar::uniform_int_distribution<int64_t> amountDist(1, 100000);

    uint32_t const N = 10;
    for (uint32_t i = 0; i < 100; ++i)
    {
        VirtualClock clock;
        Application::pointer app = createTestApplication(clock, cfg);

        std::vector<LedgerEntry> entries1;
        std::generate_n(std::back_inserter(entries1), N,
                        std::bind(generateRandomAccount, 2));
        entries1 = updateBalances(entries1, *app);
        {
            auto updates = makeUpdateList(entries1, nullptr);
            REQUIRE(!store(*app, updates));
        }

        OperationResult opRes;
        opRes.tr().type(INFLATION);
        opRes.tr().inflationResult().code(INFLATION_SUCCESS);
        int64_t inflationAmount = 0;
        auto& payouts = opRes.tr().inflationResult().payouts();
        std::generate_n(std::back_inserter(payouts),
                        payoutsDist(getGlobalRandomEngine()),
                        [&amountDist, &inflationAmount]() {
                            InflationPayout ip;
                            ip.amount = amountDist(getGlobalRandomEngine());
                            inflationAmount += ip.amount;
                            return ip;
                        });

        stellar::uniform_int_distribution<int64_t> deltaFeePoolDist(
            0, 2 * inflationAmount);
        auto deltaFeePool = deltaFeePoolDist(getGlobalRandomEngine());

        {
            LedgerTxn ltx(app->getLedgerTxnRoot());
            ltx.loadHeader().current().feePool += deltaFeePool;
            REQUIRE_THROWS_AS(
                app->getInvariantManager().checkOnOperationApply(
                    {}, opRes, ltx.getDelta(), {}, app->getAppConnector()),
                InvariantDoesNotHold);
        }

        {
            LedgerTxn ltx(app->getLedgerTxnRoot());
            ltx.loadHeader().current().feePool += deltaFeePool;
            ltx.loadHeader().current().totalCoins +=
                deltaFeePool + inflationAmount;
            REQUIRE_THROWS_AS(
                app->getInvariantManager().checkOnOperationApply(
                    {}, opRes, ltx.getDelta(), {}, app->getAppConnector()),
                InvariantDoesNotHold);
        }

        auto entries2 = updateBalances(entries1, *app, inflationAmount, true);
        {
            LedgerTxn ltx(app->getLedgerTxnRoot());
            ltx.loadHeader().current().feePool += deltaFeePool;
            ltx.loadHeader().current().totalCoins +=
                deltaFeePool + inflationAmount;

            auto updates = makeUpdateList(entries2, entries1);
            REQUIRE(store(*app, updates, &ltx, &opRes));
        }
    }
}

// This test validates that the ConservationOfLumens invariant correctly detects
// when totalCoins doesn't match the actual balance in buckets.
TEST_CASE(
    "ConservationOfLumens snapshot invariant validates corrupted total lumens",
    "[invariant][conservationoflumens]")
{
    auto cfg = getTestConfig();
    cfg.INVARIANT_CHECKS = {"ConservationOfLumens"};
    // This test directly modifies LedgerTxnRoot header (totalCoins), which
    // creates a hash mismatch between the DB header and what SCP externalized.
    // This is incompatible with background apply where the cross-check runs on
    // a thread that doesn't have access to the cached LCL header.
    cfg.PARALLEL_LEDGER_APPLY = false;

    SorobanTest test(cfg);

    auto& app = test.getApp();
    auto& root = test.getRoot();

    // Create an account with some native lumens
    auto const minBalance = app.getLedgerManager().getLastMinBalance(2);
    auto a1 = root.create("a1", minBalance);

    // Transfer native lumens to a contract address
    AssetContractTestClient client(test, makeNativeAsset());
    auto contractAddr = makeContractAddress(sha256("contract"));
    REQUIRE(client.transfer(a1, contractAddr, 100));

    // Verify the snapshot invariant passes
    {
        auto snap = app.getLedgerManager().copyApplyLedgerStateSnapshot();
        auto& inMemoryState =
            app.getLedgerManager().getInMemorySorobanStateForTesting();

        REQUIRE_NOTHROW(app.getInvariantManager().runStateSnapshotInvariant(
            snap, inMemoryState, []() { return false; }));
    }

    // Now, manually modify totalCoins to be inconsistent. The invariant should
    // detect the mismatch.
    {
        {
            LedgerTxn ltx(app.getLedgerTxnRoot());
            ltx.loadHeader().current().totalCoins += 1;
            ltx.commit();
        }

        closeLedger(test.getApp());

        auto snap = app.getLedgerManager().copyApplyLedgerStateSnapshot();
        auto& inMemoryState =
            app.getLedgerManager().getInMemorySorobanStateForTesting();

        Asset native(ASSET_TYPE_NATIVE);
        auto lumenInfo = getAssetContractInfo(native, app.getNetworkID());
        ConservationOfLumens invariant(lumenInfo);
        auto result = invariant.checkSnapshot(snap, inMemoryState,
                                              []() { return false; });
        REQUIRE_FALSE(result.empty());
        REQUIRE(result.find("Total native asset supply mismatch") !=
                std::string::npos);
    }
}

TEST_CASE("ConservationOfLumens snapshot invariant detects bucket corruption",
          "[invariant][conservationoflumens]")
{
    auto cfg = getTestConfig();
    cfg.INVARIANT_CHECKS = {}; // Disable automatic invariant checks because we
                               // will invoke it manually
    // This test directly modifies LedgerTxnRoot header (totalCoins), which is
    // incompatible with background apply (see comment in the test above).
    cfg.PARALLEL_LEDGER_APPLY = false;

    VirtualClock clock;
    auto app = createTestApplication<BucketTestUtils::BucketTestApplication>(
        clock, cfg);

    BucketTestUtils::LedgerManagerForBucketTests& lm = app->getLedgerManager();

    // Create test accounts with native balances
    auto acc1 = LedgerTestUtils::generateValidLedgerEntryOfType(ACCOUNT);
    acc1.data.account().accountID = txtest::getAccount("acc1").getPublicKey();
    acc1.data.account().balance = 1000;

    auto acc2 = LedgerTestUtils::generateValidLedgerEntryOfType(ACCOUNT);
    acc2.data.account().accountID = txtest::getAccount("acc2").getPublicKey();
    acc2.data.account().balance = 2000;

    SECTION("Invariant passes with correct bucket state")
    {
        // Get initial totalCoins before adding test accounts
        int64_t initialTotal = app->getLedgerManager()
                                   .getLastClosedLedgerHeader()
                                   .header.totalCoins;

        // Add entries to live bucket
        lm.setNextLedgerEntryBatchForBucketTesting({acc1, acc2}, // init entries
                                                   {},           // live entries
                                                   {});          // dead entries
        BucketTestUtils::closeLedger(*app);

        // Calculate expected totalCoins: initial + acc1 + acc2
        int64_t expectedTotal = initialTotal + acc1.data.account().balance +
                                acc2.data.account().balance;

        // Update header to match
        {
            LedgerTxn ltx(app->getLedgerTxnRoot());
            ltx.loadHeader().current().totalCoins = expectedTotal;
            ltx.commit();
        }

        BucketTestUtils::closeLedger(*app);

        app->getInvariantManager().enableInvariant("ConservationOfLumens");

        auto snap = app->getLedgerManager().copyApplyLedgerStateSnapshot();
        auto& inMemoryState =
            app->getLedgerManager().getInMemorySorobanStateForTesting();

        REQUIRE_NOTHROW(app->getInvariantManager().runStateSnapshotInvariant(
            snap, inMemoryState, []() { return false; }));
    }

    SECTION("Invariant fails when bucket balance doesn't match totalCoins")
    {
        // Get initial totalCoins before adding test accounts
        int64_t initialTotal = app->getLedgerManager()
                                   .getLastClosedLedgerHeader()
                                   .header.totalCoins;

        // Add entries to live bucket
        lm.setNextLedgerEntryBatchForBucketTesting({acc1, acc2}, // init entries
                                                   {},           // live entries
                                                   {});          // dead entries
        BucketTestUtils::closeLedger(*app);

        // Set totalCoins to incorrect value (off by 9999)
        int64_t incorrectTotal = initialTotal + acc1.data.account().balance +
                                 acc2.data.account().balance + 9999;

        {
            LedgerTxn ltx(app->getLedgerTxnRoot());
            ltx.loadHeader().current().totalCoins = incorrectTotal;
            ltx.commit();
        }

        BucketTestUtils::closeLedger(*app);

        auto snap = app->getLedgerManager().copyApplyLedgerStateSnapshot();
        auto& inMemoryState =
            app->getLedgerManager().getInMemorySorobanStateForTesting();

        Asset native(ASSET_TYPE_NATIVE);
        auto lumenInfo = getAssetContractInfo(native, app->getNetworkID());
        ConservationOfLumens invariant(lumenInfo);
        auto result = invariant.checkSnapshot(snap, inMemoryState,
                                              []() { return false; });
        REQUIRE_FALSE(result.empty());
        REQUIRE(result.find("Total native asset supply mismatch") !=
                std::string::npos);
    }

    SECTION("Invariant handles shadowing correctly")
    {
        // Get initial totalCoins
        int64_t initialTotal = app->getLedgerManager()
                                   .getLastClosedLedgerHeader()
                                   .header.totalCoins;

        // Add acc1 as INIT entry in older level
        lm.setNextLedgerEntryBatchForBucketTesting({acc1}, // init entries
                                                   {},     // live entries
                                                   {});    // dead entries

        // Close enough ledgers so acc1 is pushed down to a different bucket.
        // We'll add a acc1 update to curr after this.
        for (size_t i = 0; i < 8; ++i)
        {
            BucketTestUtils::closeLedger(*app);
        }

        // Create modified version of acc1 with different balance
        auto acc1Modified = acc1;
        acc1Modified.data.account().balance = 5000;

        // Add modified acc1 as LIVE entry (shadows the INIT entry)
        lm.setNextLedgerEntryBatchForBucketTesting(
            {},             // init entries
            {acc1Modified}, // live entries (shadows acc1 INIT)
            {});            // dead entries

        BucketTestUtils::closeLedger(*app);

        // The invariant should only count acc1Modified's balance (5000), not
        // the original (1000)
        int64_t expectedTotal =
            initialTotal + acc1Modified.data.account().balance;

        {
            LedgerTxn ltx(app->getLedgerTxnRoot());
            ltx.loadHeader().current().totalCoins = expectedTotal;
            ltx.commit();
        }

        BucketTestUtils::closeLedger(*app);

        app->getInvariantManager().enableInvariant("ConservationOfLumens");

        auto snap = app->getLedgerManager().copyApplyLedgerStateSnapshot();
        auto& inMemoryState =
            app->getLedgerManager().getInMemorySorobanStateForTesting();

        REQUIRE_NOTHROW(app->getInvariantManager().runStateSnapshotInvariant(
            snap, inMemoryState, []() { return false; }));
    }

    SECTION("Invariant detects corrupted native balance in hot archive")
    {
        // Wrap the existing app in SorobanTest to use contract utilities
        SorobanTest sorobanTest(app, cfg, true, [](SorobanNetworkConfig&) {});

        auto& root = sorobanTest.getRoot();
        auto minBalance = app->getLedgerManager().getLastMinBalance(2);
        auto acc1 = root.create("acc1", minBalance + 10000);

        // Deploy native asset contract and create a balance
        AssetContractTestClient client(sorobanTest, makeNativeAsset());
        auto contractAddr1 = makeContractAddress(sha256("test_contract1"));
        auto contractAddr2 = makeContractAddress(sha256("test_contract2"));

        // Transfer creates the native balance CONTRACT_DATA entry
        REQUIRE(client.transfer(acc1, contractAddr1, 5000));
        REQUIRE(client.transfer(acc1, contractAddr2, 1));
        BucketTestUtils::closeLedger(*app);

        // Extract the balance entry from live state
        LedgerEntry balanceEntry1;
        LedgerEntry balanceEntry2;
        LedgerKey balanceKey1;
        LedgerKey balanceKey2;
        {
            LedgerTxn ltx(app->getLedgerTxnRoot());
            balanceKey1 = client.makeBalanceKey(contractAddr1);
            balanceKey2 = client.makeBalanceKey(contractAddr2);

            auto ltxe1 = ltx.load(balanceKey1);
            auto ltxe2 = ltx.load(balanceKey2);

            REQUIRE(ltxe1);
            REQUIRE(ltxe2);

            balanceEntry1 = ltxe1.current();
            balanceEntry2 = ltxe2.current();
        }

        app->getInvariantManager().enableInvariant("ConservationOfLumens");

        // Remove from live buckets and inject valid entry into hot archive
        lm.setNextLedgerEntryBatchForBucketTesting(
            {}, {}, {balanceKey1, getTTLKey(balanceKey1)});
        lm.setNextArchiveBatchForBucketTesting({balanceEntry1}, {});
        BucketTestUtils::closeLedger(*app);

        {
            auto snap = app->getLedgerManager().copyApplyLedgerStateSnapshot();
            auto& inMemoryState =
                app->getLedgerManager().getInMemorySorobanStateForTesting();

            REQUIRE_NOTHROW(
                app->getInvariantManager().runStateSnapshotInvariant(
                    snap, inMemoryState, []() { return false; }));
        }

        // Corrupt the other live balance by adding 123 stroops to the balance
        // value
        balanceEntry2.data.contractData().val.map()->at(0).val.i128().lo += 123;

        // Remove from live buckets and inject corrupted entry into hot archive
        lm.setNextLedgerEntryBatchForBucketTesting(
            {}, {}, {balanceKey2, getTTLKey(balanceKey2)});
        lm.setNextArchiveBatchForBucketTesting({balanceEntry2}, {});
        BucketTestUtils::closeLedger(*app);

        {
            auto snap = app->getLedgerManager().copyApplyLedgerStateSnapshot();
            auto& inMemoryState =
                app->getLedgerManager().getInMemorySorobanStateForTesting();

            Asset native(ASSET_TYPE_NATIVE);
            auto lumenInfo = getAssetContractInfo(native, app->getNetworkID());
            ConservationOfLumens invariant(lumenInfo);
            auto result = invariant.checkSnapshot(snap, inMemoryState,
                                                  []() { return false; });
            REQUIRE_FALSE(result.empty());
            REQUIRE(result.find("Total native asset supply mismatch") !=
                    std::string::npos);
        }
    }
}
