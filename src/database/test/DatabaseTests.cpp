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
#include "main/Application.h"
#include "main/Config.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Decoder.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "util/Timer.h"
#include "util/TmpDir.h"
#include "util/optional.h"
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
    Application::pointer app = createTestApplication(clock, cfg);
    transactionTest(app);
}

TEST_CASE("database on-disk smoketest", "[db]")
{
    Config const& cfg = getTestConfig(0, Config::TESTDB_ON_DISK_SQLITE);

    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);
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
    CLOG(DEBUG, "Database") << "Checking sess2 observes sess1 changes";
    sess2 << "SELECT x FROM test", soci::into(s2r1);
    CHECK(s2r1 == v0);

    // Open tx and modify through sess1
    CLOG(DEBUG, "Database") << "Opening tx1 against sess1";
    soci::transaction tx1(sess1);

    CLOG(DEBUG, "Database") << "Writing through tx1 to sess1";
    sess1 << "UPDATE test SET x=:v", soci::use(tx1v1);

    // Check that sess2 does not observe tx1-pending write
    CLOG(DEBUG, "Database") << "Checking that sess2 does not observe tx1 write";
    sess2 << "SELECT x FROM test", soci::into(s2r2);
    CHECK(s2r2 == v0);

    {
        // Open 2nd tx on sess2
        CLOG(DEBUG, "Database") << "Opening tx2 against sess2";
        soci::transaction tx2(sess2);

        // First select upgrades us from deferred to a read-lock.
        CLOG(DEBUG, "Database")
            << "Issuing select to acquire read lock for sess2/tx2";
        sess2 << "SELECT x FROM test", soci::into(s2r3);
        CHECK(s2r3 == v0);

        if (app->getDatabase().isSqlite())
        {
            // Try to modify through sess2; this _would_ upgrade the read-lock
            // on the row or page in question to a write lock, but that would
            // collide with tx1's write-lock via sess1, so it throws. On
            // postgres
            // this just blocks, so we only check on sqlite.

            CLOG(DEBUG, "Database") << "Checking failure to upgrade read lock "
                                       "to conflicting write lock";
            try
            {
                soci::statement st =
                    (sess2.prepare << "UPDATE test SET x=:v", soci::use(tx2v1));
                st.execute(true);
                REQUIRE(false);
            }
            catch (soci::soci_error& e)
            {
                CLOG(DEBUG, "Database") << "Got " << e.what();
            }
            catch (...)
            {
                REQUIRE(false);
            }

            // Check that sess1 didn't see a write via sess2
            CLOG(DEBUG, "Database") << "Checking sess1 did not observe write "
                                       "on failed sess2 write-lock upgrade";
            sess1 << "SELECT x FROM test", soci::into(s1r2);
            CHECK(s1r2 == tx1v1);
        }

        // Do another write in tx1
        CLOG(DEBUG, "Database") << "Writing through sess1/tx1 again";
        sess1 << "UPDATE test SET x=:v", soci::use(tx1v2);

        // Close tx1
        CLOG(DEBUG, "Database") << "Committing tx1";
        tx1.commit();

        // Check that sess2 is still read-isolated, back before any tx1 writes
        CLOG(DEBUG, "Database") << "Checking read-isolation of sess2/tx2";
        sess2 << "SELECT x FROM test", soci::into(s2r4);
        CHECK(s2r4 == v0);

        // tx2 rolls back here
    }

    CLOG(DEBUG, "Database") << "Checking tx1 write committed";
    sess1 << "SELECT x FROM test", soci::into(s1r3);
    CHECK(s1r3 == tx1v2);
    sess1 << "DROP TABLE test";
}

TEST_CASE("sqlite MVCC test", "[db]")
{
    Config const& cfg = getTestConfig(0, Config::TESTDB_ON_DISK_SQLITE);
    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);
    checkMVCCIsolation(app);
}

#ifdef USE_POSTGRES
TEST_CASE("postgres smoketest", "[db]")
{
    Config const& cfg = getTestConfig(0, Config::TESTDB_POSTGRESQL);
    VirtualClock clock;
    try
    {
        Application::pointer app = createTestApplication(clock, cfg);
        int a = 10, b = 0;

        auto& session = app->getDatabase().getSession();

        SECTION("round trip")
        {
            transactionTest(app);
        }

        SECTION("blob storage")
        {
            soci::transaction tx(session);
            std::vector<uint8_t> x = {0, 1, 2, 3, 4, 5, 6}, y;
            soci::blob blobX(session);
            blobX.append(reinterpret_cast<char const*>(x.data()), x.size());
            session << "drop table if exists test";
            session << "create table test (a integer, b oid)";
            session << "insert into test (a, b) values (:aa, :bb)",
                soci::use(a, "aa"), soci::use(blobX, "bb");

            soci::blob blobY(session);
            session << "select a, b from test", soci::into(b),
                soci::into(blobY);
            y.resize(blobY.get_len());
            blobY.read(0, reinterpret_cast<char*>(y.data()), y.size());
            CHECK(x == y);
            LOG(DEBUG) << "blob round trip with postgresql database: "
                       << binToHex(x) << " == " << binToHex(y);
            tx.commit();
        }

        SECTION("postgres MVCC test")
        {
            app->getDatabase().getSession() << "drop table if exists test";
            checkMVCCIsolation(app);
        }
    }
    catch (soci::soci_error& err)
    {
        std::string what(err.what());

        if (what.find("Cannot establish connection") != std::string::npos)
        {
            LOG(WARNING) << "Cannot connect to postgres server " << what;
        }
        else
        {
            LOG(ERROR) << "DB error: " << what;
            REQUIRE(0);
        }
    }
}

TEST_CASE("postgres performance", "[db][pgperf][!hide]")
{
    Config cfg(getTestConfig(0, Config::TESTDB_POSTGRESQL));
    VirtualClock clock;
    std::uniform_int_distribution<uint64_t> dist;

    try
    {
        Application::pointer app = createTestApplication(clock, cfg);
        auto& session = app->getDatabase().getSession();

        session << "drop table if exists txtest;";
        session << "create table txtest (a bigint, b bigint, c bigint, primary "
                   "key (a, b));";

        int64_t pk = 0;
        int64_t sz = 10000;
        int64_t div = 100;

        LOG(INFO) << "timing 10 inserts of " << sz << " rows";
        {
            for (int64_t i = 0; i < 10; ++i)
            {
                soci::transaction sqltx(session);
                for (int64_t j = 0; j < sz; ++j)
                {
                    int64_t r = dist(gRandomEngine);
                    session << "insert into txtest (a,b,c) values (:a,:b,:c)",
                        soci::use(r), soci::use(pk), soci::use(j);
                }
                sqltx.commit();
            }
        }

        LOG(INFO) << "retiming 10 inserts of " << sz << " rows"
                  << " batched into " << sz / div << " subtransactions of "
                  << div << " inserts each";
        soci::transaction sqltx(session);
        for (int64_t i = 0; i < 10; ++i)
        {
            for (int64_t j = 0; j < sz / div; ++j)
            {
                soci::transaction subtx(session);
                for (int64_t k = 0; k < div; ++k)
                {
                    int64_t r = dist(gRandomEngine);
                    pk++;
                    session << "insert into txtest (a,b,c) values (:a,:b,:c)",
                        soci::use(r), soci::use(pk), soci::use(k);
                }
                subtx.commit();
            }
        }
        {
            sqltx.commit();
        }
    }
    catch (soci::soci_error& err)
    {
        std::string what(err.what());

        if (what.find("Cannot establish connection") != std::string::npos)
        {
            LOG(WARNING) << "Cannot connect to postgres server " << what;
        }
        else
        {
            LOG(ERROR) << "DB error: " << what;
            REQUIRE(0);
        }
    }
}

#endif

TEST_CASE("schema test", "[db]")
{
    Config const& cfg = getTestConfig(0, Config::TESTDB_IN_MEMORY_SQLITE);

    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);
    app->start();

    auto& db = app->getDatabase();
    auto dbv = db.getDBSchemaVersion();
    auto av = db.getAppSchemaVersion();
    REQUIRE(dbv == av);
}

class SchemaUpgradeTestApplication : public TestApplication
{
  public:
    using PreUpgradeFunc = std::function<void(SchemaUpgradeTestApplication&)>;

  private:
    PreUpgradeFunc const mPreUpgradeFunc;

  public:
    SchemaUpgradeTestApplication(VirtualClock& clock, Config const& cfg,
                                 PreUpgradeFunc preUpgradeFunc)
        : TestApplication(clock, cfg), mPreUpgradeFunc(preUpgradeFunc)
    {
    }

    virtual void
    actBeforeDBSchemaUpgrade() override
    {
        if (mPreUpgradeFunc)
        {
            try
            {
                mPreUpgradeFunc(*this);
            }
            catch (std::exception& e)
            {
                CLOG(FATAL, "Database")
                    << __func__ << ": exception " << e.what()
                    << " while calling pre-upgrade function";
                throw;
            }
        }
    }
};

TEST_CASE("schema upgrade test", "[db]")
{
    auto prepOldSchemaDB = [](SchemaUpgradeTestApplication& app,
                              AccountEntry const& ae0,
                              optional<Liabilities> const liabilities0,
                              AccountEntry const& ae1,
                              optional<Liabilities> const liabilities1,
                              AccountEntry const& ae2,
                              optional<Liabilities> const liabilities2,
                              TrustLineEntry const& tl3,
                              optional<Liabilities> const liabilities3,
                              TrustLineEntry const& tl4,
                              optional<Liabilities> const liabilities4,
                              TrustLineEntry const& tl5,
                              optional<Liabilities> const liabilities5) {
        auto addOneOldSchemaAccount = [](SchemaUpgradeTestApplication& app,
                                         AccountEntry const& ae,
                                         optional<Liabilities> const
                                             liabilities) {
            auto& session = app.getDatabase().getSession();
            auto accountIDStr = KeyUtils::toStrKey<PublicKey>(ae.accountID);
            auto inflationDestStr =
                ae.inflationDest
                    ? KeyUtils::toStrKey<PublicKey>(*ae.inflationDest)
                    : "";
            auto inflationDestInd =
                ae.inflationDest ? soci::i_ok : soci::i_null;
            auto homeDomainStr = decoder::encode_b64(ae.homeDomain);
            auto signersStr =
                decoder::encode_b64(xdr::xdr_to_opaque(ae.signers));
            auto thresholdsStr = decoder::encode_b64(ae.thresholds);

            soci::transaction tx(session);

            // Use raw SQL to perform a couple of database operations,
            // since we're writing in an old database format, and calling
            // standard interfaces to create accounts or trustlines would
            // use SQL corresponding to the new format to which we'll
            // soon upgrade.
            session
                << "INSERT INTO accounts ( "
                   "accountid, balance, seqnum, numsubentries, inflationdest,"
                   "homedomain, thresholds, signers, flags, lastmodified, "
                   "buyingliabilities, sellingliabilities "
                   ") VALUES ( "
                   ":id, :v1, :v2, :v3, :v4, :v5, :v6, :v7, :v8, :v9, :v10, "
                   ":v11 "
                   ")",
                soci::use(accountIDStr), soci::use(ae.balance),
                soci::use(ae.seqNum), soci::use(ae.numSubEntries),
                soci::use(inflationDestStr, inflationDestInd),
                soci::use(homeDomainStr), soci::use(thresholdsStr),
                soci::use(signersStr), soci::use(ae.flags),
                soci::use(app.getLedgerManager().getLastClosedLedgerNum()),
                soci::use(liabilities ? liabilities->buying : 0),
                soci::use(liabilities ? liabilities->selling : 0);
            if (!liabilities)
            {
                session << "UPDATE accounts SET buyingliabilities = NULL, "
                           "sellingliabilities = NULL WHERE "
                           "accountid = :id",
                    soci::use(accountIDStr);
            }
            tx.commit();
        };

        try
        {
            addOneOldSchemaAccount(app, ae0, liabilities0);
            addOneOldSchemaAccount(app, ae1, liabilities1);
            addOneOldSchemaAccount(app, ae2, liabilities2);
        }
        catch (std::exception& e)
        {
            CLOG(FATAL, "Database") << __func__ << ": exception " << e.what()
                                    << " while adding old-schema accounts";
            throw;
        }

        auto addOneOldSchemaTrustLine = [](SchemaUpgradeTestApplication& app,
                                           TrustLineEntry const& tl,
                                           optional<Liabilities> const
                                               liabilities) {
            auto& session = app.getDatabase().getSession();
            std::string accountIDStr, issuerStr, assetCodeStr;
            getTrustLineStrings(tl.accountID, tl.asset, accountIDStr, issuerStr,
                                assetCodeStr);
            int32_t assetType = tl.asset.type();

            soci::transaction tx(session);
            session << "INSERT INTO trustlines ( "
                       "accountid, assettype, issuer, assetcode,"
                       "tlimit, balance, flags, lastmodified, "
                       "buyingliabilities, sellingliabilities "
                       ") VALUES ( "
                       ":id, :v1, :v2, :v3, :v4, :v5, :v6, :v7, :v8, :v9 "
                       ")",
                soci::use(accountIDStr), soci::use(assetType),
                soci::use(issuerStr), soci::use(assetCodeStr),
                soci::use(tl.limit), soci::use(tl.balance), soci::use(tl.flags),
                soci::use(app.getLedgerManager().getLastClosedLedgerNum()),
                soci::use(liabilities ? liabilities->buying : 0),
                soci::use(liabilities ? liabilities->selling : 0);
            if (!liabilities)
            {
                session
                    << "UPDATE trustlines SET buyingliabilities = NULL, "
                       "sellingliabilities = NULL WHERE "
                       "accountid = :id AND issuer = :v1 AND assetcode = :v2",
                    soci::use(accountIDStr), soci::use(issuerStr),
                    soci::use(assetCodeStr);
            }
            tx.commit();
        };
        try
        {
            addOneOldSchemaTrustLine(app, tl3, liabilities3);
            addOneOldSchemaTrustLine(app, tl4, liabilities4);
            addOneOldSchemaTrustLine(app, tl5, liabilities5);
        }
        catch (std::exception& e)
        {
            CLOG(FATAL, "Database") << __func__ << ": exception " << e.what()
                                    << " while adding old-schema trustlines";
            throw;
        }

    };

    auto testOneDBMode = [prepOldSchemaDB](Config::TestDbMode const dbMode) {
        Config const& cfg = getTestConfig(0, dbMode);
        VirtualClock clock;
        auto ae0 = LedgerTestUtils::generateValidAccountEntry();
        auto ae1 = LedgerTestUtils::generateValidAccountEntry();
        auto ae2 = LedgerTestUtils::generateValidAccountEntry();
        auto tl3 = LedgerTestUtils::generateValidTrustLineEntry();
        auto tl4 = LedgerTestUtils::generateValidTrustLineEntry();
        auto tl5 = LedgerTestUtils::generateValidTrustLineEntry();
        Liabilities liabilities1, liabilities2, liabilities3, liabilities4;
        liabilities1.buying = 12;
        liabilities1.selling = 17;
        liabilities2.buying = 3;
        liabilities2.selling = 0;
        liabilities3.buying = 0;
        liabilities3.selling = 6;
        liabilities4.buying = 0;
        liabilities4.selling = 0;
        Application::pointer app =
            createTestApplication<SchemaUpgradeTestApplication,
                                  SchemaUpgradeTestApplication::PreUpgradeFunc>(
                clock, cfg,
                [prepOldSchemaDB, ae0, ae1, ae2, tl3, tl4, tl5, liabilities1,
                 liabilities2, liabilities3,
                 liabilities4](SchemaUpgradeTestApplication& sapp) {
                    prepOldSchemaDB(
                        sapp, ae0, nullopt<Liabilities>(), ae1,
                        make_optional<Liabilities>(liabilities1), ae2,
                        make_optional<Liabilities>(liabilities2), tl3,
                        make_optional<Liabilities>(liabilities3), tl4,
                        make_optional<Liabilities>(liabilities4), tl5,
                        nullopt<Liabilities>());
                });
        app->start();

        LedgerTxn ltx(app->getLedgerTxnRoot());
        LedgerKey key;
        LedgerTxnEntry acc;
        LedgerTxnEntry tl;

        key.type(ACCOUNT);

        key.account().accountID = ae0.accountID;
        acc = ltx.load(key);
        REQUIRE(acc.current().data.type() == ACCOUNT);
        REQUIRE(acc.current().data.account().ext.v() == 0);

        key.account().accountID = ae1.accountID;
        acc = ltx.load(key);
        REQUIRE(acc.current().data.type() == ACCOUNT);
        REQUIRE(acc.current().data.account().ext.v() == 1);
        REQUIRE(acc.current().data.account().ext.v1().liabilities.buying ==
                liabilities1.buying);
        REQUIRE(acc.current().data.account().ext.v1().liabilities.selling ==
                liabilities1.selling);

        key.account().accountID = ae2.accountID;
        acc = ltx.load(key);
        REQUIRE(acc.current().data.type() == ACCOUNT);
        REQUIRE(acc.current().data.account().ext.v() == 1);
        REQUIRE(acc.current().data.account().ext.v1().liabilities.buying ==
                liabilities2.buying);
        REQUIRE(acc.current().data.account().ext.v1().liabilities.selling ==
                liabilities2.selling);

        key.type(TRUSTLINE);

        key.trustLine().accountID = tl3.accountID;
        key.trustLine().asset = tl3.asset;
        tl = ltx.load(key);
        REQUIRE(tl.current().data.type() == TRUSTLINE);
        REQUIRE(tl.current().data.trustLine().ext.v() == 1);
        REQUIRE(tl.current().data.trustLine().ext.v1().liabilities.buying ==
                liabilities3.buying);
        REQUIRE(tl.current().data.trustLine().ext.v1().liabilities.selling ==
                liabilities3.selling);

        key.trustLine().accountID = tl4.accountID;
        key.trustLine().asset = tl4.asset;
        tl = ltx.load(key);
        REQUIRE(tl.current().data.type() == TRUSTLINE);
        REQUIRE(tl.current().data.trustLine().ext.v() == 1);
        REQUIRE(tl.current().data.trustLine().ext.v1().liabilities.buying ==
                liabilities4.buying);
        REQUIRE(tl.current().data.trustLine().ext.v1().liabilities.selling ==
                liabilities4.selling);

        key.trustLine().accountID = tl5.accountID;
        key.trustLine().asset = tl5.asset;
        tl = ltx.load(key);
        REQUIRE(tl.current().data.type() == TRUSTLINE);
        REQUIRE(tl.current().data.trustLine().ext.v() == 0);
    };

    for (auto dbMode :
         {Config::TESTDB_IN_MEMORY_SQLITE, Config::TESTDB_ON_DISK_SQLITE
#ifdef USE_POSTGRES
          ,
          Config::TESTDB_POSTGRESQL
#endif // USE_POSTGRES
         })
    {
        try
        {
            testOneDBMode(dbMode);
        }
        catch (std::exception& e)
        {
            CLOG(FATAL, "Database") << __func__ << ": exception " << e.what()
                                    << " while testing dbMode " << dbMode;
            throw;
        }
    }
}
