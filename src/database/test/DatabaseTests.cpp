// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "crypto/Hex.h"
#include "crypto/KeyUtils.h"
#include "database/Database.h"
#include "ledger/LedgerTxn.h"
#include "ledger/test/LedgerTestUtils.h"
#include "lib/catch.hpp"
#include "lib/util/stdrandom.h"
#include "main/Application.h"
#include "main/Config.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Decoder.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "util/Timer.h"
#include "util/TmpDir.h"
#include <algorithm>
#include <optional>
#include <random>

using namespace stellar;

void
transactionTest(Application::pointer app)
{
    int a = 10, b = 0;
    int a0 = a + 1;
    int a1 = a + 2;

    auto& session = app->getDatabase().getSession();

    session << "DROP TABLE IF EXISTS test";
    session << "CREATE TABLE test (x INTEGER)";

    {
        soci::transaction tx(session);

        session << "INSERT INTO test (x) VALUES (:aa)", soci::use(a0, "aa");

        session << "SELECT x FROM test", soci::into(b);
        CHECK(a0 == b);

        {
            soci::transaction tx2(session);
            session << "UPDATE test SET x = :v", soci::use(a1, "v");
            tx2.rollback();
        }

        session << "SELECT x FROM test", soci::into(b);
        CHECK(a0 == b);

        {
            soci::transaction tx3(session);
            session << "UPDATE test SET x = :v", soci::use(a, "v");
            tx3.commit();
        }
        session << "SELECT x FROM test", soci::into(b);
        CHECK(a == b);

        tx.commit();
    }

    session << "SELECT x FROM test", soci::into(b);
    CHECK(a == b);
    session << "DROP TABLE test";
}

TEST_CASE("database smoketest", "[db]")
{
    Config const& cfg = getTestConfig(0, Config::TESTDB_IN_MEMORY_SQLITE);

    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg, true, false);
    transactionTest(app);
}

TEST_CASE("database on-disk smoketest", "[db]")
{
    Config const& cfg = getTestConfig(0, Config::TESTDB_ON_DISK_SQLITE);

    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg, true, false);
    transactionTest(app);
}

void
checkMVCCIsolation(Application::pointer app)
{

    int v0 = 1;

    // Values we insert/update in different txs
    int tx1v1 = 11, tx1v2 = 12;

    int tx2v1 = 21;

    // Values we read back out of different sessions
    int s1r1 = 0, s1r2 = 0, s1r3 = 0;

    int s2r1 = 0, s2r2 = 0, s2r3 = 0, s2r4 = 0;

    auto& sess1 = app->getDatabase().getSession();

    sess1 << "DROP TABLE IF EXISTS test";
    sess1 << "CREATE TABLE test (x INTEGER)";
    sess1 << "INSERT INTO test (x) VALUES (:v)", soci::use(v0);

    // Check that our write was committed to sess1
    sess1 << "SELECT x FROM test", soci::into(s1r1);
    CHECK(s1r1 == v0);

    soci::session sess2(app->getDatabase().getPool());

    // Check that sess2 can observe changes from sess1
    CLOG_DEBUG(Database, "Checking sess2 observes sess1 changes");
    sess2 << "SELECT x FROM test", soci::into(s2r1);
    CHECK(s2r1 == v0);

    // Open tx and modify through sess1
    CLOG_DEBUG(Database, "Opening tx1 against sess1");
    soci::transaction tx1(sess1);

    CLOG_DEBUG(Database, "Writing through tx1 to sess1");
    sess1 << "UPDATE test SET x=:v", soci::use(tx1v1);

    // Check that sess2 does not observe tx1-pending write
    CLOG_DEBUG(Database, "Checking that sess2 does not observe tx1 write");
    sess2 << "SELECT x FROM test", soci::into(s2r2);
    CHECK(s2r2 == v0);

    {
        // Open 2nd tx on sess2
        CLOG_DEBUG(Database, "Opening tx2 against sess2");
        soci::transaction tx2(sess2);

        // First select upgrades us from deferred to a read-lock.
        CLOG_DEBUG(Database,
                   "Issuing select to acquire read lock for sess2/tx2");
        sess2 << "SELECT x FROM test", soci::into(s2r3);
        CHECK(s2r3 == v0);

        if (app->getDatabase().isSqlite())
        {
            // Try to modify through sess2; this _would_ upgrade the read-lock
            // on the row or page in question to a write lock, but that would
            // collide with tx1's write-lock via sess1, so it throws.

            CLOG_DEBUG(Database, "Checking failure to upgrade read lock "
                                 "to conflicting write lock");
            try
            {
                soci::statement st =
                    (sess2.prepare << "UPDATE test SET x=:v", soci::use(tx2v1));
                st.execute(true);
                REQUIRE(false);
            }
            catch (soci::soci_error& e)
            {
                CLOG_DEBUG(Database, "Got {}", e.what());
            }
            catch (...)
            {
                REQUIRE(false);
            }

            // Check that sess1 didn't see a write via sess2
            CLOG_DEBUG(Database, "Checking sess1 did not observe write "
                                 "on failed sess2 write-lock upgrade");
            sess1 << "SELECT x FROM test", soci::into(s1r2);
            CHECK(s1r2 == tx1v1);
        }

        // Do another write in tx1
        CLOG_DEBUG(Database, "Writing through sess1/tx1 again");
        sess1 << "UPDATE test SET x=:v", soci::use(tx1v2);

        // Close tx1
        CLOG_DEBUG(Database, "Committing tx1");
        tx1.commit();

        // Check that sess2 is still read-isolated, back before any tx1 writes
        CLOG_DEBUG(Database, "Checking read-isolation of sess2/tx2");
        sess2 << "SELECT x FROM test", soci::into(s2r4);
        CHECK(s2r4 == v0);

        // tx2 rolls back here
    }

    CLOG_DEBUG(Database, "Checking tx1 write committed");
    sess1 << "SELECT x FROM test", soci::into(s1r3);
    CHECK(s1r3 == tx1v2);
    sess1 << "DROP TABLE test";
}

TEST_CASE("sqlite MVCC test", "[db]")
{
    Config const& cfg = getTestConfig(0, Config::TESTDB_ON_DISK_SQLITE);
    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg, true, false);
    checkMVCCIsolation(app);
}

TEST_CASE("schema test", "[db]")
{
    Config const& cfg = getTestConfig(0, Config::TESTDB_IN_MEMORY_SQLITE);

    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);

    auto& db = app->getDatabase();
    auto dbv = db.getDBSchemaVersion();
    auto av = db.getAppSchemaVersion();
    REQUIRE(dbv == av);
}
