// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "crypto/Hex.h"
#include "crypto/KeyUtils.h"
#include "crypto/SecretKey.h"
#include "database/Database.h"
#include "ledger/LedgerHeaderUtils.h"
#include "ledger/LedgerTxn.h"
#include "ledger/test/LedgerTestUtils.h"
#include "lib/util/stdrandom.h"
#include "main/Application.h"
#include "main/Config.h"
#include "main/PersistentState.h"
#include "overlay/BanManager.h"
#include "overlay/OverlayManager.h"
#include "test/Catch2.h"
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

    auto& session = app->getDatabase().getRawSession();

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
    Config const& cfg = getTestConfig(0, Config::TESTDB_IN_MEMORY);

    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg, true, false);
    transactionTest(app);
}

TEST_CASE("database on-disk smoketest", "[db]")
{
    Config const& cfg = getTestConfig(0, Config::TESTDB_BUCKET_DB_PERSISTENT);

    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg, true, false);
    transactionTest(app);
}

static void
checkMVCCIsolation(Application::pointer app)
{

    int v0 = 1;

    // Values we insert/update in different txs
    int tx1v1 = 11, tx1v2 = 12;

    int tx2v1 = 21;

    // Values we read back out of different sessions
    int s1r1 = 0, s1r2 = 0, s1r3 = 0;

    int s2r1 = 0, s2r2 = 0, s2r3 = 0, s2r4 = 0;

    auto& sess1 = app->getDatabase().getRawSession();

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
    Config const& cfg = getTestConfig(0, Config::TESTDB_BUCKET_DB_PERSISTENT);
    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg, true, false);
    checkMVCCIsolation(app);
}

TEST_CASE("schema test", "[db]")
{
    Config const& cfg = getTestConfig(0, Config::TESTDB_IN_MEMORY);

    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);

    auto& db = app->getDatabase();
    auto dbv = db.getMainDBSchemaVersion();
    REQUIRE(dbv == SCHEMA_VERSION);
}

TEST_CASE("getMiscDBName handles various file extensions", "[db]")
{
    SECTION("Standard .db extension")
    {
        std::string result = Database::getMiscDBName("stellar.db");
        REQUIRE(result == "stellar-misc.db");
    }

    SECTION("SQLite3 extension")
    {
        std::string result = Database::getMiscDBName("stellar.sqlite3");
        REQUIRE(result == "stellar-misc.sqlite3");
    }

    SECTION("SQLite extension")
    {
        std::string result = Database::getMiscDBName("stellar.sqlite");
        REQUIRE(result == "stellar-misc.sqlite");
    }

    SECTION("No extension")
    {
        std::string result = Database::getMiscDBName("stellar");
        REQUIRE(result == "stellar-misc.db");
    }

    SECTION("Multiple dots in filename")
    {
        std::string result = Database::getMiscDBName("stellar.backup.db");
        REQUIRE(result == "stellar.backup-misc.db");
    }

    SECTION("Path with directories")
    {
        std::string result = Database::getMiscDBName("/path/to/stellar.db");
        REQUIRE(result == "/path/to/stellar-misc.db");
    }
}

TEST_CASE("Database splitting migration works correctly", "[db]")
{
    TmpDir tmpDir("db-migration-test");
    Config cfg = getTestConfig(0, Config::TESTDB_BUCKET_DB_PERSISTENT);
    cfg.DATABASE = SecretValue{"sqlite3://" + tmpDir.getName() + "/test.db"};

    VirtualClock clock;
    // Set startApp to false to trigger migration manually
    Application::pointer app = createTestApplication(
        clock, cfg, /* newDB */ true, /* startApp */ false);

    releaseAssert(app->getDatabase().canUseMiscDB());

    SECTION("Fresh database creates misc DB correctly")
    {
        app->getDatabase().initialize();
        app->getDatabase().upgradeToCurrentSchema();

        // Verify schema versions
        REQUIRE(app->getDatabase().getMainDBSchemaVersion() == SCHEMA_VERSION);
        REQUIRE(app->getDatabase().getMiscDBSchemaVersion() ==
                MISC_SCHEMA_VERSION);
    }

    SECTION("Migrate data to Misc DB")
    {
        app->getDatabase().initialize();

        auto& db = app->getDatabase();

        // Helper to execute SQL on a session
        auto execSQL = [&](std::string const& sql, SessionWrapper& session) {
            auto prep = db.getPreparedStatement(sql, session);
            auto& st = prep.statement();
            st.define_and_bind();
            st.execute(true);
        };

        // Helper to count rows in a table
        auto countRows = [&](std::string const& table,
                             SessionWrapper& session) {
            int count = 0;
            auto prep = db.getPreparedStatement("SELECT COUNT(*) FROM " + table,
                                                session);
            auto& st = prep.statement();
            st.exchange(soci::into(count));
            st.define_and_bind();
            st.execute(true);
            return count;
        };

        // Helper to check if table exists in a session
        auto tableExists = [&](std::string const& table,
                               SessionWrapper& session) {
            int count = 0;
            auto prep = db.getPreparedStatement(
                "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND "
                "name='" +
                    table + "'",
                session);
            auto& st = prep.statement();
            st.exchange(soci::into(count));
            st.define_and_bind();
            st.execute(true);
            return count > 0;
        };

        // Insert test data into all tables that should be migrated
        execSQL("INSERT INTO peers (ip, port, nextattempt, numfailures, type) "
                "VALUES ('127.0.0.1', 11625, '2024-01-01 00:00:00', 0, 1)",
                db.getSession());
        execSQL("INSERT INTO ban (nodeid) VALUES "
                "('GAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAWHF')",
                db.getSession());
        execSQL("INSERT INTO scphistory (nodeid, ledgerseq, envelope) VALUES "
                "('GAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAWHF', "
                "100, 'test_envelope')",
                db.getSession());
        execSQL(
            "INSERT INTO scpquorums (qsethash, lastledgerseq, qset) VALUES "
            "('abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234"
            "', 100, 'test_qset')",
            db.getSession());
        execSQL(
            "INSERT INTO quoruminfo (nodeid, qsethash) VALUES "
            "('GAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAWHF', "
            "'abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234'"
            ")",
            db.getSession());
        execSQL("INSERT INTO slotstate (statename, state) VALUES "
                "('ledgerupgrades', 'testvalue')",
                db.getSession());

        LedgerHeader header = LedgerManager::genesisLedger();
        // Change one value from the genesis ledger to ensure that we did
        // actually migrate correctly
        header.ledgerSeq = 12345;
        LedgerHeaderUtils::storeInDatabase(db, header, db.getSession());
        std::string hash;
        std::string headerEncoded =
            LedgerHeaderUtils::encodeHeader(header, hash);

        // Insert test data that should stay in main DB
        execSQL("INSERT INTO storestate (statename, state) VALUES " +
                    fmt::format("('lastclosedledger', '{}')", hash),
                db.getSession());

        // Verify data exists in main before migration
        REQUIRE(countRows("peers", db.getSession()) == 1);
        REQUIRE(countRows("ban", db.getSession()) == 1);
        REQUIRE(countRows("scphistory", db.getSession()) == 1);
        REQUIRE(countRows("scpquorums", db.getSession()) == 1);
        REQUIRE(countRows("quoruminfo", db.getSession()) == 1);
        REQUIRE(countRows("slotstate", db.getSession()) == 1);

        // Trigger migration
        db.upgradeToCurrentSchema();

        // Verify main DB still has main data
        {
            std::string result;
            auto prep = db.getPreparedStatement(
                "SELECT state FROM storestate WHERE statename = "
                "'lastclosedledgerheader'",
                db.getSession());
            auto& st = prep.statement();
            st.exchange(soci::into(result));
            st.define_and_bind();
            st.execute(true);
            REQUIRE(result == headerEncoded);
        }

        // Verify storestate table still exists in main DB
        REQUIRE(tableExists("storestate", db.getSession()));

        // Verify main-only data did NOT get migrated to misc DB
        REQUIRE_FALSE(tableExists("storestate", db.getMiscSession()));

        // Verify all misc tables are dropped from main
        std::vector<std::string> migratedTables = {"peers",      "ban",
                                                   "scphistory", "scpquorums",
                                                   "quoruminfo", "slotstate"};
        for (auto const& table : migratedTables)
        {
            REQUIRE_FALSE(tableExists(table, db.getSession()));
        }

        // Verify data was migrated to misc DB
        // Note: slotstate has 2 rows (test data + miscdatabaseschema)
        REQUIRE(countRows("peers", db.getMiscSession()) == 1);
        REQUIRE(countRows("ban", db.getMiscSession()) == 1);
        REQUIRE(countRows("scphistory", db.getMiscSession()) == 1);
        REQUIRE(countRows("scpquorums", db.getMiscSession()) == 1);
        REQUIRE(countRows("quoruminfo", db.getMiscSession()) == 1);
        REQUIRE(countRows("slotstate", db.getMiscSession()) == 2);

        // Verify specific data values in misc DB
        {
            std::string ip;
            int port = 0;
            auto prep = db.getPreparedStatement("SELECT ip, port FROM peers",
                                                db.getMiscSession());
            auto& st = prep.statement();
            st.exchange(soci::into(ip));
            st.exchange(soci::into(port));
            st.define_and_bind();
            st.execute(true);
            REQUIRE(ip == "127.0.0.1");
            REQUIRE(port == 11625);
        }
        {
            std::string state;
            auto prep = db.getPreparedStatement(
                "SELECT state FROM slotstate WHERE statename = "
                "'ledgerupgrades'",
                db.getMiscSession());
            auto& st = prep.statement();
            st.exchange(soci::into(state));
            st.define_and_bind();
            st.execute(true);
            REQUIRE(state == "testvalue");
        }
    }
}

TEST_CASE("ledgerheaders migration works correctly", "[db]")
{
    Config::TestDbMode mode = GENERATE(Config::TESTDB_BUCKET_DB_PERSISTENT);
    Config cfg = getTestConfig(0, mode);

    VirtualClock clock;
    // Set startApp to false to trigger migration manually
    Application::pointer app = createTestApplication(
        clock, cfg, /* newDB */ true, /* startApp */ false);

    std::optional<std::string> expectedLCLHeader;
    auto checkMigration = [&app](std::optional<std::string> expectedLCL) {
        REQUIRE(app->getDatabase().getMainDBSchemaVersion() == SCHEMA_VERSION);
        REQUIRE_THROWS(app->getDatabase().getRawSession()
                       << "SELECT COUNT(1) FROM ledgerheaders");

        {
            // Check that lastclosedledger has been removed
            auto& sess = app->getDatabase().getRawSession();
            int i;
            sess << "SELECT COUNT(1) FROM storestate WHERE statename = "
                    "'lastclosedledger'",
                soci::into(i);
            REQUIRE(sess.got_data());
            REQUIRE(i == 0);
        }

        std::string lclHeader = app->getPersistentState().getState(
            PersistentState::kLastClosedLedgerHeader,
            app->getDatabase().getSession());

        if (expectedLCL)
        {
            REQUIRE(lclHeader == expectedLCL);
        }
        else
        {
            LedgerHeader lh = LedgerHeaderUtils::decodeFromData(lclHeader);
            REQUIRE(
                app->getLedgerManager().getLastClosedLedgerHeader().header ==
                lh);
        }
    };

    SECTION("Just running newdb")
    {
        checkMigration(std::nullopt);
    }

    SECTION("Migrate from old schema with LCL header")
    {
        auto& db = app->getDatabase();
        db.initialize();

        auto& lcl = app->getLedgerManager().getLastClosedLedgerHeader();
        LedgerHeader header = lcl.header;
        header.ledgerSeq++;
        header.previousLedgerHash = lcl.hash;
        LedgerHeaderUtils::storeInDatabase(db, header, db.getSession());

        std::string hash;
        std::string headerEncoded =
            LedgerHeaderUtils::encodeHeader(header, hash);
        db.getRawSession()
            << "INSERT INTO storestate (statename, state) VALUES "
               "('lastclosedledger', :h)",
            soci::use(hash);

        db.upgradeToCurrentSchema();

        checkMigration(headerEncoded);
    }
}
