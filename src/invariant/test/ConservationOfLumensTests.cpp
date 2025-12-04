// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/ConservationOfLumens.h"
#include "invariant/InvariantDoesNotHold.h"
#include "invariant/InvariantManager.h"
#include "invariant/test/InvariantTestUtils.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnHeader.h"
#include "lib/util/stdrandom.h"
#include "main/Application.h"
#include "test/Catch2.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/test/SorobanTxTestUtils.h"
#include "util/Math.h"
#include <numeric>
#include <random>
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

TEST_CASE("Snapshot invariant validates total lumens",
          "[invariant][conservationoflumens]")
{
    auto cfg = getTestConfig();
    cfg.INVARIANT_CHECKS = {"ConservationOfLumens"};
    cfg.INVARIANT_EXTRA_CHECKS = true;

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

    // Note: We should figure out how to force the eviction of an SAC contract
    // balance. The SAC auto extends balances, so simply waiting for TTL to
    // expire is not practical. We need to update the in memory ttl to a lower
    // value for testing.

    // Verify the snapshot invariant passes
    {
        auto ledgerState =
            app.getLedgerManager().getLastClosedLedgerStateForTesting();
        auto& inMemoryState =
            app.getLedgerManager().getInMemorySorobanStateForTesting();

        REQUIRE_NOTHROW(app.getInvariantManager().runStateSnapshotInvariant(
            ledgerState, inMemoryState, []() { return false; }));
    }

    // Now, manually modify totalCoins to be inconsistent. The invariant should
    // trigger.
    {
        {
            LedgerTxn ltx(app.getLedgerTxnRoot());
            ltx.loadHeader().current().totalCoins += 1;
            ltx.commit();
        }

        closeLedger(test.getApp());

        auto ledgerState =
            app.getLedgerManager().getLastClosedLedgerStateForTesting();
        auto& inMemoryState =
            app.getLedgerManager().getInMemorySorobanStateForTesting();

        REQUIRE_THROWS_AS(
            app.getInvariantManager().runStateSnapshotInvariant(
                ledgerState, inMemoryState, []() { return false; }),
            InvariantDoesNotHold);
    }
}
