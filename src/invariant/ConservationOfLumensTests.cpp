// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/ConservationOfLumens.h"
#include "invariant/InvariantDoesNotHold.h"
#include "invariant/InvariantManager.h"
#include "invariant/InvariantTestUtils.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include <numeric>
#include <random>
#include <xdrpp/autocheck.h>

using namespace stellar;
using namespace stellar::InvariantTestUtils;

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
    return std::accumulate(entries.begin(), entries.end(),
                           static_cast<int64_t>(0),
                           [&app](int64_t lhs, LedgerEntry const& rhs) {
                               auto& lm = app.getLedgerManager();
                               auto& account = rhs.data.account();
                               return lhs + account.balance -
                                      lm.getMinBalance(account.numSubEntries);
                           });
}

std::vector<LedgerEntry>
updateBalances(std::vector<LedgerEntry> entries, Application& app,
               std::default_random_engine& gen, int64_t netChange)
{
    int64_t initialCoins = getTotalBalance(entries);
    int64_t pool = netChange + getCoinsAboveReserve(entries, app);
    int64_t totalDelta = 0;
    for (auto iter = entries.begin(); iter != entries.end(); ++iter)
    {
        auto& account = iter->data.account();
        auto minBalance =
            app.getLedgerManager().getMinBalance(account.numSubEntries);
        pool -= account.balance - minBalance;

        int64_t delta = 0;
        if (iter + 1 != entries.end())
        {
            int64_t maxDecrease = minBalance - account.balance;
            int64_t maxIncrease = pool;
            REQUIRE(maxIncrease >= maxDecrease);
            std::uniform_int_distribution<int64_t> dist(maxDecrease,
                                                        maxIncrease);
            delta = dist(gen);
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

    auto& lh = app.getLedgerManager().getCurrentLedgerHeader();
    lh.totalCoins += netChange;
    return entries;
}

std::vector<LedgerEntry>
updateBalances(std::vector<LedgerEntry> const& entries, Application& app,
               std::default_random_engine& gen)
{
    int64_t coinsAboveReserve = getCoinsAboveReserve(entries, app);
    auto& lh = app.getLedgerManager().getCurrentLedgerHeader();
    std::uniform_int_distribution<int64_t> dist(
        lh.totalCoins - coinsAboveReserve, INT64_MAX);
    int64_t newTotalCoins = dist(gen);
    return updateBalances(entries, app, gen, newTotalCoins - lh.totalCoins);
}

std::vector<EntryFrame::pointer>
generateEntryFrames(std::vector<LedgerEntry> const& entries)
{
    std::vector<EntryFrame::pointer> result;
    std::transform(
        entries.begin(), entries.end(), std::back_inserter(result),
        [](LedgerEntry const& le) { return EntryFrame::FromXDR(le); });
    return result;
}

UpdateList
generateUpdateList(std::vector<EntryFrame::pointer> const& current,
                   std::vector<EntryFrame::pointer> const& previous)
{
    assert(current.size() == previous.size());
    UpdateList updates;
    std::transform(
        current.begin(), current.end(), previous.begin(),
        std::back_inserter(updates),
        [](EntryFrame::pointer const& curr, EntryFrame::pointer const& prev) {
            return UpdateList::value_type{curr, prev};
        });
    return updates;
}

TEST_CASE("Total coins change without inflation",
          "[invariant][conservationoflumens]")
{
    std::default_random_engine gen;
    Config cfg = getTestConfig(0);
    cfg.INVARIANT_CHECKS = {"ConservationOfLumens"};

    std::uniform_int_distribution<int64_t> dist(0, INT64_MAX);

    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);

    LedgerHeader lh(app->getLedgerManager().getCurrentLedgerHeader());
    LedgerDelta ld(lh, app->getDatabase(), false);
    ld.getHeader().totalCoins = dist(gen);
    OperationResult res;
    REQUIRE_THROWS_AS(
        app->getInvariantManager().checkOnOperationApply({}, res, ld),
        InvariantDoesNotHold);
}

TEST_CASE("Fee pool change without inflation",
          "[invariant][conservationoflumens]")
{
    std::default_random_engine gen;
    Config cfg = getTestConfig(0);
    cfg.INVARIANT_CHECKS = {"ConservationOfLumens"};

    std::uniform_int_distribution<int64_t> dist(0, INT64_MAX);

    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);

    LedgerHeader lh(app->getLedgerManager().getCurrentLedgerHeader());
    LedgerDelta ld(lh, app->getDatabase(), false);
    ld.getHeader().feePool = dist(gen);
    OperationResult res;
    REQUIRE_THROWS_AS(
        app->getInvariantManager().checkOnOperationApply({}, res, ld),
        InvariantDoesNotHold);
}

TEST_CASE("Account balances changed without inflation",
          "[invariant][conservationoflumens]")
{
    std::default_random_engine gen;
    Config cfg = getTestConfig(0);
    cfg.INVARIANT_CHECKS = {"ConservationOfLumens"};

    uint32_t const N = 10;
    for (uint32_t i = 0; i < 100; ++i)
    {
        VirtualClock clock;
        Application::pointer app = createTestApplication(clock, cfg);

        std::vector<LedgerEntry> entries1;
        std::generate_n(std::back_inserter(entries1), N,
                        std::bind(generateRandomAccount, 2));
        entries1 = updateBalances(entries1, *app, gen);
        {
            UpdateList updates = generateUpdateList(
                generateEntryFrames(entries1),
                std::vector<EntryFrame::pointer>(entries1.size()));
            REQUIRE(!store(*app, updates));
        }

        auto entries2 = updateBalances(entries1, *app, gen);
        {
            UpdateList updates = generateUpdateList(
                generateEntryFrames(entries2), generateEntryFrames(entries1));
            REQUIRE(!store(*app, updates));
        }

        {
            UpdateList updates = generateUpdateList(
                std::vector<EntryFrame::pointer>(entries1.size()),
                generateEntryFrames(entries1));
            REQUIRE(!store(*app, updates));
        }
    }
}

TEST_CASE("Account balances unchanged without inflation",
          "[invariant][conservationoflumens]")
{
    std::default_random_engine gen;
    Config cfg = getTestConfig(0);
    cfg.INVARIANT_CHECKS = {"ConservationOfLumens"};

    uint32_t const N = 10;
    for (uint32_t i = 0; i < 100; ++i)
    {
        VirtualClock clock;
        Application::pointer app = createTestApplication(clock, cfg);

        std::vector<LedgerEntry> entries1;
        std::generate_n(std::back_inserter(entries1), N,
                        std::bind(generateRandomAccount, 2));
        entries1 = updateBalances(entries1, *app, gen);
        {
            UpdateList updates = generateUpdateList(
                generateEntryFrames(entries1),
                std::vector<EntryFrame::pointer>(entries1.size()));
            REQUIRE(!store(*app, updates));
        }

        auto entries2 = updateBalances(entries1, *app, gen, 0);
        {
            UpdateList updates = generateUpdateList(
                generateEntryFrames(entries2), generateEntryFrames(entries1));
            REQUIRE(store(*app, updates));
        }

        auto keepEnd = entries2.begin() + N / 2;
        std::vector<LedgerEntry> entries3(entries2.begin(), keepEnd);
        std::vector<LedgerEntry> toDelete(keepEnd, entries2.end());
        int64_t balanceToDelete = getTotalBalance(toDelete);
        entries3 = updateBalances(entries3, *app, gen, balanceToDelete);
        {
            auto entryFrames3 = generateEntryFrames(entries3);
            std::fill_n(std::back_inserter(entryFrames3), toDelete.size(),
                        nullptr);
            UpdateList updates =
                generateUpdateList(entryFrames3, generateEntryFrames(entries2));
            REQUIRE(store(*app, updates));
        }
    }
}

TEST_CASE("Inflation changes are consistent",
          "[invariant][conservationoflumens]")
{
    std::default_random_engine gen;
    Config cfg = getTestConfig(0);
    cfg.INVARIANT_CHECKS = {"ConservationOfLumens"};
    std::uniform_int_distribution<uint32_t> payoutsDist(1, 100);
    std::uniform_int_distribution<int64_t> amountDist(1, 100000);

    uint32_t const N = 10;
    for (uint32_t i = 0; i < 100; ++i)
    {
        VirtualClock clock;
        Application::pointer app = createTestApplication(clock, cfg);

        std::vector<LedgerEntry> entries1;
        std::generate_n(std::back_inserter(entries1), N,
                        std::bind(generateRandomAccount, 2));
        entries1 = updateBalances(entries1, *app, gen);
        {
            UpdateList updates = generateUpdateList(
                generateEntryFrames(entries1),
                std::vector<EntryFrame::pointer>(entries1.size()));
            REQUIRE(!store(*app, updates));
        }

        OperationResult opRes;
        opRes.tr().type(INFLATION);
        opRes.tr().inflationResult().code(INFLATION_SUCCESS);
        int64_t inflationAmount = 0;
        auto& payouts = opRes.tr().inflationResult().payouts();
        std::generate_n(std::back_inserter(payouts), payoutsDist(gen),
                        [&gen, &amountDist, &inflationAmount]() {
                            InflationPayout ip;
                            ip.amount = amountDist(gen);
                            inflationAmount += ip.amount;
                            return ip;
                        });

        std::uniform_int_distribution<int64_t> deltaFeePoolDist(
            0, 2 * inflationAmount);
        auto deltaFeePool = deltaFeePoolDist(gen);

        LedgerHeader lh(app->getLedgerManager().getCurrentLedgerHeader());
        LedgerDelta ld(lh, app->getDatabase(), false);
        ld.getHeader().feePool += deltaFeePool;
        REQUIRE_THROWS_AS(
            app->getInvariantManager().checkOnOperationApply({}, opRes, ld),
            InvariantDoesNotHold);

        ld.getHeader().totalCoins += deltaFeePool + inflationAmount;
        REQUIRE_THROWS_AS(
            app->getInvariantManager().checkOnOperationApply({}, opRes, ld),
            InvariantDoesNotHold);

        auto entries2 = updateBalances(entries1, *app, gen, inflationAmount);
        {
            UpdateList updates = generateUpdateList(
                generateEntryFrames(entries2), generateEntryFrames(entries1));
            REQUIRE(store(*app, updates, &ld, &opRes));
        }
    }
}
