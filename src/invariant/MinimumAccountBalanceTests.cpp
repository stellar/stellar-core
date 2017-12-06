// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/InvariantDoesNotHold.h"
#include "invariant/InvariantManager.h"
#include "invariant/InvariantTestUtils.h"
#include "invariant/MinimumAccountBalance.h"
#include "ledger/AccountFrame.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include <random>

using namespace stellar;
using namespace stellar::InvariantTestUtils;

LedgerEntry
updateAccountWithRandomBalance(LedgerEntry le, Application& app,
                               std::default_random_engine& gen,
                               bool exceedsMinimum, int32_t direction)
{
    auto& account = le.data.account();

    auto minBalance =
        app.getLedgerManager().getMinBalance(account.numSubEntries);

    int64_t lbound = 0;
    int64_t ubound = std::numeric_limits<int64_t>::max();
    if (direction > 0)
    {
        lbound = account.balance + 1;
    }
    else if (direction < 0)
    {
        ubound = account.balance - 1;
    }
    if (exceedsMinimum)
    {
        lbound = std::max(lbound, minBalance);
    }
    else
    {
        ubound = std::min(ubound, minBalance - 1);
    }
    REQUIRE(lbound <= ubound);

    std::uniform_int_distribution<int64_t> dist(lbound, ubound);
    account.balance = dist(gen);
    return le;
}

TEST_CASE("Create account above minimum balance",
          "[invariant][minimumaccountbalance]")
{
    std::default_random_engine gen;
    Config cfg = getTestConfig(0);
    cfg.INVARIANT_CHECKS = {"MinimumAccountBalance"};

    for (uint32_t i = 0; i < 10; ++i)
    {
        VirtualClock clock;
        Application::pointer app = createTestApplication(clock, cfg);

        auto le = generateRandomAccount(2);
        le = updateAccountWithRandomBalance(le, *app, gen, true, 0);
        REQUIRE(store(*app, makeUpdateList(EntryFrame::FromXDR(le), nullptr)));
    }
}

TEST_CASE("Create account below minimum balance",
          "[invariant][minimumaccountbalance]")
{
    std::default_random_engine gen;
    Config cfg = getTestConfig(0);
    cfg.INVARIANT_CHECKS = {"MinimumAccountBalance"};

    for (uint32_t i = 0; i < 10; ++i)
    {
        VirtualClock clock;
        Application::pointer app = createTestApplication(clock, cfg);

        auto le = generateRandomAccount(2);
        le = updateAccountWithRandomBalance(le, *app, gen, false, 0);
        REQUIRE(!store(*app, makeUpdateList(EntryFrame::FromXDR(le), nullptr)));
    }
}

TEST_CASE("Create account then decrease balance below minimum",
          "[invariant][minimumaccountbalance]")
{
    std::default_random_engine gen;
    Config cfg = getTestConfig(0);
    cfg.INVARIANT_CHECKS = {"MinimumAccountBalance"};

    for (uint32_t i = 0; i < 10; ++i)
    {
        VirtualClock clock;
        Application::pointer app = createTestApplication(clock, cfg);

        auto le1 = generateRandomAccount(2);
        le1 = updateAccountWithRandomBalance(le1, *app, gen, true, 0);
        auto ef = EntryFrame::FromXDR(le1);
        REQUIRE(store(*app, makeUpdateList(ef, nullptr)));
        auto le2 = updateAccountWithRandomBalance(le1, *app, gen, false, 0);
        REQUIRE(!store(*app, makeUpdateList(EntryFrame::FromXDR(le2), ef)));
    }
}

TEST_CASE("Account below minimum balance increases but stays below minimum",
          "[invariant][minimumaccountbalance]")
{
    std::default_random_engine gen;
    Config cfg = getTestConfig(0);
    cfg.INVARIANT_CHECKS = {"MinimumAccountBalance"};

    for (uint32_t i = 0; i < 10; ++i)
    {
        VirtualClock clock;
        Application::pointer app = createTestApplication(clock, cfg);

        auto le1 = generateRandomAccount(2);
        le1 = updateAccountWithRandomBalance(le1, *app, gen, false, 0);
        auto ef = EntryFrame::FromXDR(le1);
        REQUIRE(!store(*app, makeUpdateList(ef, nullptr)));
        auto le2 = updateAccountWithRandomBalance(le1, *app, gen, false, 1);
        REQUIRE(store(*app, makeUpdateList(EntryFrame::FromXDR(le2), ef)));
    }
}

TEST_CASE("Account below minimum balance decreases",
          "[invariant][minimumaccountbalance]")
{
    std::default_random_engine gen;
    Config cfg = getTestConfig(0);
    cfg.INVARIANT_CHECKS = {"MinimumAccountBalance"};

    for (uint32_t i = 0; i < 10; ++i)
    {
        VirtualClock clock;
        Application::pointer app = createTestApplication(clock, cfg);

        auto le1 = generateRandomAccount(2);
        le1 = updateAccountWithRandomBalance(le1, *app, gen, false, 0);
        auto ef = EntryFrame::FromXDR(le1);
        REQUIRE(!store(*app, makeUpdateList(ef, nullptr)));
        auto le2 = updateAccountWithRandomBalance(le1, *app, gen, false, -1);
        REQUIRE(!store(*app, makeUpdateList(EntryFrame::FromXDR(le2), ef)));
    }
}
