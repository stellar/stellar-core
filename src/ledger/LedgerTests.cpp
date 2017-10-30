// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "LedgerTestUtils.h"
#include "database/Database.h"
#include "ledger/AccountFrame.h"
#include "ledger/EntryFrame.h"
#include "ledger/LedgerDelta.h"
#include "ledger/LedgerManager.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include "util/types.h"
#include <xdrpp/autocheck.h>

using namespace stellar;

TEST_CASE("Ledger entry db lifecycle", "[ledger]")
{
    Config cfg(getTestConfig());
    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);
    app->start();
    LedgerDelta delta(app->getLedgerManager().getCurrentLedgerHeader(),
                      app->getDatabase());
    auto& db = app->getDatabase();
    for (size_t i = 0; i < 100; ++i)
    {
        auto le =
            EntryFrame::FromXDR(LedgerTestUtils::generateValidLedgerEntry(3));
        CHECK(!EntryFrame::exists(db, le->getKey()));
        le->storeAddOrChange(delta, db);
        CHECK(EntryFrame::exists(db, le->getKey()));
        le->storeDelete(delta, db);
        CHECK(!EntryFrame::exists(db, le->getKey()));
    }
}

TEST_CASE("single ledger entry insert SQL", "[singlesql][entrysql]")
{
    Config::TestDbMode mode = Config::TESTDB_ON_DISK_SQLITE;
#ifdef USE_POSTGRES
    if (!force_sqlite)
        mode = Config::TESTDB_POSTGRESQL;
#endif

    VirtualClock clock;
    Application::pointer app =
        createTestApplication(clock, getTestConfig(0, mode));
    app->start();

    LedgerDelta delta(app->getLedgerManager().getCurrentLedgerHeader(),
                      app->getDatabase());
    auto& db = app->getDatabase();
    auto le = EntryFrame::FromXDR(LedgerTestUtils::generateValidLedgerEntry(3));
    auto ctx = db.captureAndLogSQL("ledger-insert");
    le->storeAddOrChange(delta, db);
}

TEST_CASE("DB cache interaction with transactions", "[ledger][dbcache]")
{
    Config::TestDbMode mode = Config::TESTDB_ON_DISK_SQLITE;
#ifdef USE_POSTGRES
    if (!force_sqlite)
        mode = Config::TESTDB_POSTGRESQL;
#endif

    VirtualClock clock;
    Application::pointer app =
        createTestApplication(clock, getTestConfig(0, mode));
    app->start();

    auto& db = app->getDatabase();
    auto& session = db.getSession();

    EntryFrame::pointer le;
    do
    {
        le = EntryFrame::FromXDR(LedgerTestUtils::generateValidLedgerEntry(3));
    } while (le->mEntry.data.type() != ACCOUNT);

    auto key = le->getKey();

    {
        LedgerDelta delta(app->getLedgerManager().getCurrentLedgerHeader(),
                          app->getDatabase());
        soci::transaction sqltx(session);
        le->storeAddOrChange(delta, db);
        sqltx.commit();
    }

    // The write should have removed it from the cache.
    REQUIRE(!EntryFrame::cachedEntryExists(key, db));

    int64_t balance0, balance1;

    {
        soci::transaction sqltx(session);
        LedgerDelta delta(app->getLedgerManager().getCurrentLedgerHeader(),
                          app->getDatabase());

        auto acc = AccountFrame::loadAccount(key.account().accountID, db);
        REQUIRE(EntryFrame::cachedEntryExists(key, db));

        balance0 = acc->getAccount().balance;
        acc->getAccount().balance += 1;
        balance1 = acc->getAccount().balance;

        acc->storeChange(delta, db);
        // Write should flush cache, put balance1 in DB _pending commit_.
        REQUIRE(!EntryFrame::cachedEntryExists(key, db));

        acc = AccountFrame::loadAccount(key.account().accountID, db);
        // Read should have populated cache.
        REQUIRE(EntryFrame::cachedEntryExists(key, db));

        // Read-back value should be balance1
        REQUIRE(acc->getAccount().balance == balance1);

        LOG(INFO) << "balance0: " << balance0;
        LOG(INFO) << "balance1: " << balance1;

        // Scope-end will rollback sqltx and delta
    }

    // Rollback should have evicted changed value from cache.
    CHECK(!EntryFrame::cachedEntryExists(key, db));

    auto acc = AccountFrame::loadAccount(key.account().accountID, db);
    // Read should populate cache
    CHECK(EntryFrame::cachedEntryExists(key, db));
    LOG(INFO) << "cached balance: " << acc->getAccount().balance;

    CHECK(balance0 == acc->getAccount().balance);
}
