// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "LedgerTestUtils.h"
#include "database/Database.h"
#include "database/EntryQueries.h"
#include "ledger/AccountFrame.h"
#include "ledger/EntryFrame.h"
#include "ledgerdelta/LedgerDelta.h"
#include "ledger/LedgerEntries.h"
#include "ledger/LedgerManager.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
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
    Application::pointer app = Application::create(clock, cfg);
    app->start();
    auto& entries = app->getLedgerEntries();
    LedgerDelta delta(app->getLedgerManager().getCurrentLedgerHeader(), app->getLedgerEntries());
    auto& db = app->getDatabase();
    for (size_t i = 0; i < 100; ++i)
    {
        auto entry = EntryFrame{LedgerTestUtils::generateValidLedgerEntry(3)};
        auto key = entry.getKey();
        CHECK(!entryExists(key, db));
        delta.addEntry(entry);
        CHECK(entryExists(key, db));
        delta.deleteEntry(key);
        CHECK(!entryExists(key, db));
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
        Application::create(clock, getTestConfig(0, mode));
    app->start();

    LedgerDelta ledgerDelta(app->getLedgerManager().getCurrentLedgerHeader(),
        app->getLedgerEntries());
    auto& db = app->getDatabase();
    auto entry = EntryFrame{LedgerTestUtils::generateValidLedgerEntry(3)};
    auto ctx = db.captureAndLogSQL("ledger-insert");
    ledgerDelta.insertOrUpdateEntry(entry);
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
        Application::create(clock, getTestConfig(0, mode));
    app->start();

    auto& entries = app->getLedgerEntries();
    auto& db = app->getDatabase();
    auto& session = db.getSession();

    EntryFrame le;
    do
    {
        le = EntryFrame{LedgerTestUtils::generateValidLedgerEntry(3)};
    } while (le.getEntry().data.type() != ACCOUNT);

    auto key = le.getKey();
    {
        LedgerDelta ledgerDelta(app->getLedgerManager().getCurrentLedgerHeader(), entries);
        soci::transaction sqltx(session);
        ledgerDelta.insertOrUpdateEntry(le);
        app->getLedgerManager().apply(ledgerDelta);
        sqltx.commit();
    }

    // New version is in cache.
    REQUIRE(entries.isCached(key));
    REQUIRE(*entries.getFromCache(key) == le.getEntry());

    int64_t balance0, balance1;

    {
        soci::transaction sqltx(session);
        LedgerDelta delta(app->getLedgerManager().getCurrentLedgerHeader(), entries);

        auto acc = AccountFrame{*entries.load(key)};
        auto prevAcc = acc;
        REQUIRE(entries.isCached(key));

        balance0 = acc.getBalance();
        acc.setBalance(balance0 + 1);
        balance1 = acc.getBalance();

        delta.updateEntry(acc);
        // Write should not change cache, it will be update after delta.commit()
        REQUIRE(entries.isCached(key));
        REQUIRE(*entries.getFromCache(key) == prevAcc.getEntry());

        // Read will read old value from the cache.
        entries.load(key);
        REQUIRE(entries.isCached(key));
        REQUIRE(*entries.getFromCache(key) == prevAcc.getEntry());

        app->getLedgerManager().apply(delta);

        // After commit new value is in the the cache.
        REQUIRE(entries.isCached(key));
        REQUIRE(*entries.getFromCache(key) == acc.getEntry());

        // Read-back value should be balance1
        REQUIRE(acc.getBalance() == balance1);

        LOG(INFO) << "balance0: " << balance0;
        LOG(INFO) << "balance1: " << balance1;

        // Scope-end will rollback sqltx and delta
    }

    // Rollback should have evicted changed value from cache.
    CHECK(!entries.isCached(key));

    auto acc = AccountFrame{*entries.load(key)};
    // Read should populate cache
    CHECK(entries.isCached(key));
    LOG(INFO) << "cached balance: " << acc.getBalance();

    CHECK(balance0 == acc.getBalance());
}
