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
#include <algorithm>
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
            // collide with tx1's write-lock via sess1, so it throws. On
            // postgres
            // this just blocks, so we only check on sqlite.

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
            LOG_DEBUG(DEFAULT_LOG,
                      "blob round trip with postgresql database: {} == {}",
                      binToHex(x), binToHex(y));
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
            LOG_WARNING(DEFAULT_LOG, "Cannot connect to postgres server {}",
                        what);
        }
        else
        {
            LOG_ERROR(DEFAULT_LOG, "DB error: {}", what);
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

        LOG_INFO(DEFAULT_LOG, "timing 10 inserts of {} rows", sz);
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

        LOG_INFO(DEFAULT_LOG,
                 "retiming 10 inserts of {} rows batched into {} "
                 "subtransactions of {} inserts each",
                 sz, sz / div, div);
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
            LOG_WARNING(DEFAULT_LOG, "Cannot connect to postgres server {}",
                        what);
        }
        else
        {
            LOG_ERROR(DEFAULT_LOG, "DB error: {}", what);
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
    class SchemaUpgradeTestDatabase : public Database
    {
        SchemaUpgradeTestApplication& mApp;
        PreUpgradeFunc const mPreUpgradeFunc;

      public:
        SchemaUpgradeTestDatabase(SchemaUpgradeTestApplication& app,
                                  PreUpgradeFunc preUpgradeFunc)
            : Database(app), mApp(app), mPreUpgradeFunc(preUpgradeFunc)
        {
        }

        virtual void
        actBeforeDBSchemaUpgrade() override
        {
            if (mPreUpgradeFunc)
            {
                mPreUpgradeFunc(mApp);
            }
        }
    };

    PreUpgradeFunc const mPreUpgradeFunc;

  public:
    SchemaUpgradeTestApplication(VirtualClock& clock, Config const& cfg,
                                 PreUpgradeFunc preUpgradeFunc)
        : TestApplication(clock, cfg), mPreUpgradeFunc(preUpgradeFunc)
    {
    }

    virtual std::unique_ptr<Database>
    createDatabase() override
    {
        return std::make_unique<SchemaUpgradeTestDatabase>(*this,
                                                           mPreUpgradeFunc);
    }
};

// This tests upgrading from MIN_SCHEMA_VERSION to SCHEMA_VERSION by creating
// ledger entries with the old MIN_SCHEMA_VERSION after database creation, then
// validating their contents after the upgrade to SCHEMA_VERSION.
TEST_CASE("schema upgrade test", "[db]")
{
    using OptLiabilities = stellar::optional<Liabilities>;

    auto addOneOldSchemaAccount = [](SchemaUpgradeTestApplication& app,
                                     AccountEntry const& ae) {
        auto& session = app.getDatabase().getSession();
        auto accountIDStr = KeyUtils::toStrKey<PublicKey>(ae.accountID);
        auto inflationDestStr =
            ae.inflationDest ? KeyUtils::toStrKey<PublicKey>(*ae.inflationDest)
                             : "";
        auto inflationDestInd = ae.inflationDest ? soci::i_ok : soci::i_null;
        auto homeDomainStr = decoder::encode_b64(ae.homeDomain);
        auto signersStr = decoder::encode_b64(xdr::xdr_to_opaque(ae.signers));
        auto thresholdsStr = decoder::encode_b64(ae.thresholds);
        bool const liabilitiesPresent = (ae.ext.v() >= 1);
        int64_t buyingLiabilities =
            liabilitiesPresent ? ae.ext.v1().liabilities.buying : 0;
        int64_t sellingLiabilities =
            liabilitiesPresent ? ae.ext.v1().liabilities.selling : 0;
        soci::indicator liabilitiesInd =
            liabilitiesPresent ? soci::i_ok : soci::i_null;

        soci::transaction tx(session);

        auto lastMod = app.getLedgerManager().getLastClosedLedgerNum();
        // Use raw SQL to perform database operations, since we're writing in an
        // old database format, and calling standard interfaces to create
        // accounts or trustlines would use SQL corresponding to the new format
        // to which we'll soon upgrade.
        session << "INSERT INTO accounts ( "
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
            soci::use(signersStr), soci::use(ae.flags), soci::use(lastMod),
            soci::use(buyingLiabilities, liabilitiesInd),
            soci::use(sellingLiabilities, liabilitiesInd);

        tx.commit();
    };

    auto addOneOldSchemaTrustLine = [](SchemaUpgradeTestApplication& app,
                                       TrustLineEntry const& tl) {
        auto& session = app.getDatabase().getSession();
        std::string accountIDStr, issuerStr, assetCodeStr;
        getTrustLineStrings(tl.accountID, tl.asset, accountIDStr, issuerStr,
                            assetCodeStr, 15);
        int32_t assetType = tl.asset.type();
        bool const liabilitiesPresent = (tl.ext.v() >= 1);
        int64_t buyingLiabilities =
            liabilitiesPresent ? tl.ext.v1().liabilities.buying : 0;
        int64_t sellingLiabilities =
            liabilitiesPresent ? tl.ext.v1().liabilities.selling : 0;
        soci::indicator liabilitiesInd =
            liabilitiesPresent ? soci::i_ok : soci::i_null;

        soci::transaction tx(session);

        auto lastMod = app.getLedgerManager().getLastClosedLedgerNum();
        session << "INSERT INTO trustlines ( "
                   "accountid, assettype, issuer, assetcode,"
                   "tlimit, balance, flags, lastmodified, "
                   "buyingliabilities, sellingliabilities "
                   ") VALUES ( "
                   ":id, :v1, :v2, :v3, :v4, :v5, :v6, :v7, :v8, :v9 "
                   ")",
            soci::use(accountIDStr), soci::use(assetType), soci::use(issuerStr),
            soci::use(assetCodeStr), soci::use(tl.limit), soci::use(tl.balance),
            soci::use(tl.flags), soci::use(lastMod),
            soci::use(buyingLiabilities, liabilitiesInd),
            soci::use(sellingLiabilities, liabilitiesInd);

        tx.commit();
    };

    auto addOneOldSchemaDataEntry = [](SchemaUpgradeTestApplication& app,
                                       DataEntry const& de) {
        auto& session = app.getDatabase().getSession();
        auto accountIDStr = KeyUtils::toStrKey<PublicKey>(de.accountID);
        auto dataNameStr = decoder::encode_b64(de.dataName);
        auto dataValueStr = decoder::encode_b64(de.dataValue);

        soci::transaction tx(session);

        auto lastMod = app.getLedgerManager().getLastClosedLedgerNum();
        session << "INSERT INTO accountdata ( "
                   "accountid, dataname, datavalue, lastmodified "
                   ") VALUES ( :id, :v1, :v2, :v3 )",
            soci::use(accountIDStr), soci::use(dataNameStr),
            soci::use(dataValueStr), soci::use(lastMod);

        tx.commit();
    };

    auto addOneOldSchemaOfferEntry = [](SchemaUpgradeTestApplication& app,
                                        OfferEntry const& oe) {
        auto& session = app.getDatabase().getSession();
        auto sellerIDStr = KeyUtils::toStrKey(oe.sellerID);
        auto sellingStr = decoder::encode_b64(xdr::xdr_to_opaque(oe.selling));
        auto buyingStr = decoder::encode_b64(xdr::xdr_to_opaque(oe.buying));
        double price = double(oe.price.n) / double(oe.price.d);

        soci::transaction tx(session);

        auto lastMod = app.getLedgerManager().getLastClosedLedgerNum();
        session << "INSERT INTO offers ( "
                   "sellerid, offerid, sellingasset, buyingasset, "
                   "amount, pricen, priced, price, flags, lastmodified "
                   ") VALUES ( "
                   ":v1, :v2, :v3, :v4, :v5, :v6, :v7, :v8, :v9, :v10 "
                   ")",
            soci::use(sellerIDStr), soci::use(oe.offerID),
            soci::use(sellingStr), soci::use(buyingStr), soci::use(oe.amount),
            soci::use(oe.price.n), soci::use(oe.price.d), soci::use(price),
            soci::use(oe.flags), soci::use(lastMod);

        tx.commit();
    };

    auto prepOldSchemaDB =
        [addOneOldSchemaAccount, addOneOldSchemaTrustLine,
         addOneOldSchemaDataEntry,
         addOneOldSchemaOfferEntry](SchemaUpgradeTestApplication& app,
                                    std::vector<AccountEntry> const& aes,
                                    std::vector<TrustLineEntry> const& tls,
                                    DataEntry const& de, OfferEntry const& oe) {
            for (auto ae : aes)
            {
                addOneOldSchemaAccount(app, ae);
            }

            for (auto tl : tls)
            {
                addOneOldSchemaTrustLine(app, tl);
            }

            addOneOldSchemaDataEntry(app, de);
            addOneOldSchemaOfferEntry(app, oe);
        };

    auto testOneDBMode = [prepOldSchemaDB](Config::TestDbMode const dbMode) {
        // A vector of optional Liabilities entries, for each of which the test
        // will generate a valid account.
        auto const accOptLiabilities = {
            nullopt<Liabilities>(),
            make_optional<Liabilities>(Liabilities{12, 17}),
            make_optional<Liabilities>(Liabilities{4, 0}),
            nullopt<Liabilities>(),
            make_optional<Liabilities>(Liabilities{3, 0}),
            make_optional<Liabilities>(Liabilities{11, 11}),
            make_optional<Liabilities>(Liabilities{0, 0})};

        // A vector of optional Liabilities entries, for each of which the test
        // will generate a valid trustline.
        auto const tlOptLiabilities = {
            make_optional<Liabilities>(Liabilities{1, 0}),
            make_optional<Liabilities>(Liabilities{0, 6}),
            nullopt<Liabilities>(),
            make_optional<Liabilities>(Liabilities{0, 0}),
            make_optional<Liabilities>(Liabilities{5, 8}),
            nullopt<Liabilities>()};

        // Generate from each of the optional liabilities in accOptLiabilities a
        // new valid account.
        std::vector<AccountEntry> accountEntries;
        std::transform(
            accOptLiabilities.begin(), accOptLiabilities.end(),
            std::back_inserter(accountEntries), [](OptLiabilities const& aol) {
                AccountEntry ae = LedgerTestUtils::generateValidAccountEntry();
                if (aol)
                {
                    ae.ext.v(1);
                    ae.ext.v1().ext.v(0);
                    ae.ext.v1().liabilities = *aol;
                }
                else
                {
                    ae.ext.v(0);
                }
                return ae;
            });

        // Generate from each of the optional liabilities in tlOptLiabilities a
        // new valid trustline.
        std::vector<TrustLineEntry> trustLineEntries;
        std::transform(tlOptLiabilities.begin(), tlOptLiabilities.end(),
                       std::back_inserter(trustLineEntries),
                       [](OptLiabilities const& tlol) {
                           TrustLineEntry tl =
                               LedgerTestUtils::generateValidTrustLineEntry();
                           if (tlol)
                           {
                               tl.ext.v(1);
                               tl.ext.v1().liabilities = *tlol;
                           }
                           else
                           {
                               tl.ext.v(0);
                           }
                           return tl;
                       });

        // Create a data entry to test that its extension and ledger
        // entry extension have the default (empty-XDR-union) values.
        auto de = LedgerTestUtils::generateValidDataEntry();
        REQUIRE(de.ext.v() == 0);
        REQUIRE(de.ext == DataEntry::_ext_t());

        // Create an offer entry to test that its extension and ledger
        // entry extension have the default (empty-XDR-union) values.
        auto oe = LedgerTestUtils::generateValidOfferEntry();
        REQUIRE(oe.ext.v() == 0);
        REQUIRE(oe.ext == OfferEntry::_ext_t());

        // Create the application, with the code above that inserts old-schema
        // accounts and trustlines into the database injected between database
        // creation and upgrade.
        Config const& cfg = getTestConfig(0, dbMode);
        VirtualClock clock;
        Application::pointer app =
            createTestApplication<SchemaUpgradeTestApplication,
                                  SchemaUpgradeTestApplication::PreUpgradeFunc>(
                clock, cfg,
                [prepOldSchemaDB, accountEntries, trustLineEntries, de,
                 oe](SchemaUpgradeTestApplication& sapp) {
                    prepOldSchemaDB(sapp, accountEntries, trustLineEntries, de,
                                    oe);
                });
        app->start();

        // Validate that the accounts and trustlines have the expected
        // liabilities and ledger entry extensions now that the database upgrade
        // has completed.

        LedgerTxn ltx(app->getLedgerTxnRoot());

        for (auto ae : accountEntries)
        {
            LedgerEntry entry;
            entry.data.type(ACCOUNT);
            entry.data.account() = ae;
            LedgerKey key = LedgerEntryKey(entry);
            auto aeUpgraded = ltx.load(key);
            REQUIRE(aeUpgraded.current() == entry);
        }

        for (auto tl : trustLineEntries)
        {
            LedgerEntry entry;
            entry.data.type(TRUSTLINE);
            entry.data.trustLine() = tl;
            LedgerKey key = LedgerEntryKey(entry);
            auto tlUpgraded = ltx.load(key);
            REQUIRE(tlUpgraded.current() == entry);
        }

        {
            LedgerEntry entry;
            entry.data.type(DATA);
            entry.data.data() = de;
            LedgerKey key = LedgerEntryKey(entry);
            auto deUpgraded = ltx.load(key);
            REQUIRE(deUpgraded.current() == entry);
        }

        {
            LedgerEntry entry;
            entry.data.type(OFFER);
            entry.data.offer() = oe;
            LedgerKey key = LedgerEntryKey(entry);
            auto oeUpgraded = ltx.load(key);
            REQUIRE(oeUpgraded.current() == entry);
        }
    };

    for (auto dbMode :
         {Config::TESTDB_IN_MEMORY_SQLITE, Config::TESTDB_ON_DISK_SQLITE
#ifdef USE_POSTGRES
          ,
          Config::TESTDB_POSTGRESQL
#endif // USE_POSTGRES
         })
    {
        testOneDBMode(dbMode);
    }
}
