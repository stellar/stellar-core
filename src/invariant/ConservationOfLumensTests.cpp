// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/ConservationOfLumens.h"
#include "invariant/InvariantDoesNotHold.h"
#include "invariant/InvariantManager.h"
#include "invariant/InvariantTestUtils.h"
#include "ledger/LedgerHeaderReference.h"
#include "ledger/LedgerState.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "transactions/TransactionUtils.h"
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
                               auto& account = rhs.data.account();
                               return lhs + account.balance -
                                   getCurrentMinBalance(app.getLedgerStateRoot(),
                                                        account.numSubEntries);
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
        auto minBalance = getCurrentMinBalance(app.getLedgerStateRoot(),
                                               account.numSubEntries);
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

    LedgerState ls(app.getLedgerStateRoot());
    ls.loadHeader()->header().totalCoins += netChange;
    ls.commit();
    return entries;
}

std::vector<LedgerEntry>
updateBalances(std::vector<LedgerEntry> const& entries, Application& app,
               std::default_random_engine& gen)
{
    int64_t coinsAboveReserve = getCoinsAboveReserve(entries, app);
    LedgerState ls(app.getLedgerStateRoot());
    auto header = ls.loadHeader();
    std::uniform_int_distribution<int64_t> dist(
        header->header().totalCoins - coinsAboveReserve, INT64_MAX);
    int64_t netChange = dist(gen) - header->header().totalCoins;
    ls.rollback();
    return updateBalances(entries, app, gen, netChange);
}

UpdateList
generateUpdateList(std::vector<LedgerEntry> const& current,
                   std::vector<std::nullptr_t> const& previous)
{
    assert(current.size() == previous.size());
    UpdateList updates;
    std::transform(
        current.begin(), current.end(), previous.begin(),
        std::back_inserter(updates),
        [](LedgerEntry const& curr, LedgerEntry const* prev) {
            return UpdateList::value_type{&curr, prev};
        });
    return updates;
}

UpdateList
generateUpdateList(std::vector<LedgerEntry> const& current,
                   std::vector<LedgerEntry> const& previous)
{
    assert(current.size() == previous.size());
    UpdateList updates;
    std::transform(
        current.begin(), current.end(), previous.begin(),
        std::back_inserter(updates),
        [](LedgerEntry const& curr, LedgerEntry const& prev) {
            return UpdateList::value_type{&curr, &prev};
        });
    return updates;
}

UpdateList
generateUpdateList(std::vector<std::nullptr_t> const& current,
                   std::vector<LedgerEntry> const& previous)
{
    assert(current.size() == previous.size());
    UpdateList updates;
    std::transform(
        current.begin(), current.end(), previous.begin(),
        std::back_inserter(updates),
        [](LedgerEntry const* curr, LedgerEntry const& prev) {
            return UpdateList::value_type{curr, &prev};
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

    LedgerState ls(app->getLedgerStateRoot());
    auto header = ls.loadHeader();
    header->header().totalCoins = dist(gen);
    OperationResult res;
    REQUIRE_THROWS_AS(
        app->getInvariantManager().checkOnOperationApply({}, res, ls, header),
        InvariantDoesNotHold);
    header->invalidate();
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

    LedgerState ls(app->getLedgerStateRoot());
    auto header = ls.loadHeader();
    header->header().feePool = dist(gen);
    OperationResult res;
    REQUIRE_THROWS_AS(
        app->getInvariantManager().checkOnOperationApply({}, res, ls, header),
        InvariantDoesNotHold);
    header->invalidate();
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

        CLOG(INFO, "Invariant") << "HERE1";
        std::vector<LedgerEntry> entries1;
        std::generate_n(std::back_inserter(entries1), N,
                        std::bind(generateRandomAccount, 2));
        CLOG(INFO, "Invariant") << "HERE2";
        entries1 = updateBalances(entries1, *app, gen);
        CLOG(INFO, "Invariant") << "HERE3";
        {
            auto updates = generateUpdateList(entries1,
                std::vector<std::nullptr_t>(entries1.size()));
            REQUIRE(!store(*app, updates));
        }

        auto entries2 = updateBalances(entries1, *app, gen);
        {
            auto updates = generateUpdateList(entries2, entries1);
            REQUIRE(!store(*app, updates));
        }

        {
            auto updates = generateUpdateList(
                std::vector<std::nullptr_t>(entries1.size()),
                entries1);
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
            auto updates = generateUpdateList(entries1,
                std::vector<std::nullptr_t>(entries1.size()));
            REQUIRE(!store(*app, updates));
        }

        auto entries2 = updateBalances(entries1, *app, gen, 0);
        {
            auto updates = generateUpdateList(entries2, entries1);
            REQUIRE(store(*app, updates));
        }

        auto keepEnd = entries2.begin() + N / 2;
        std::vector<LedgerEntry> entries3(entries2.begin(), keepEnd);
        std::vector<LedgerEntry> toDelete(keepEnd, entries2.end());
        int64_t balanceToDelete = getTotalBalance(toDelete);
        auto entries4 = updateBalances(entries3, *app, gen, balanceToDelete);
        {
            auto updates = generateUpdateList(entries4, entries3);
            auto updates2 = generateUpdateList(
                std::vector<std::nullptr_t>(toDelete.size()),
                toDelete);
            updates.insert(updates.end(), updates2.begin(), updates2.end());
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
            auto updates = generateUpdateList(entries1,
                std::vector<std::nullptr_t>(entries1.size()));
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

        {
            LedgerState ls(app->getLedgerStateRoot());
            auto header = ls.loadHeader();
            header->header().feePool += deltaFeePool;
            REQUIRE_THROWS_AS(
                app->getInvariantManager().checkOnOperationApply({}, opRes, ls, header),
                InvariantDoesNotHold);

            header->header().totalCoins += deltaFeePool + inflationAmount;
            REQUIRE_THROWS_AS(
                app->getInvariantManager().checkOnOperationApply({}, opRes, ls, header),
                InvariantDoesNotHold);
            header->invalidate();
        }

        auto entries2 = updateBalances(entries1, *app, gen, inflationAmount);
        LedgerState ls(app->getLedgerStateRoot());
        auto header = ls.loadHeader();
        header->header().feePool += deltaFeePool;
        header->header().totalCoins += deltaFeePool + inflationAmount;
        header->invalidate();
        {
            auto updates = generateUpdateList(entries2, entries1);
            REQUIRE(store(*app, updates, &ls, &opRes));
        }
    }
}
