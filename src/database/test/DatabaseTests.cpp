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

// This tests upgrading from MIN_SCHEMA_VERSION to SCHEMA_VERSION by creating
// ledger entries with the old MIN_SCHEMA_VERSION after database creation, then
// validating their contents after the upgrade to SCHEMA_VERSION.
TEST_CASE("schema upgrade test", "[db]")
{
    using OptLiabilities = stellar::optional<Liabilities>;
    using AccOptLiabilitiesPair = std::pair<AccountEntry, OptLiabilities>;
    using AccOptLiabilitiesVec = std::vector<AccOptLiabilitiesPair>;
    using TLOptLiabilitiesPair = std::pair<TrustLineEntry, OptLiabilities>;
    using TLOptLiabilitiesVec = std::vector<TLOptLiabilitiesPair>;

    auto addOneOldSchemaAccount = [](SchemaUpgradeTestApplication& app,
                                     AccOptLiabilitiesPair const& aol) {
        auto const& ae = aol.first;
        auto const& liabilities = aol.second;
        auto& session = app.getDatabase().getSession();
        auto accountIDStr = KeyUtils::toStrKey<PublicKey>(ae.accountID);
        auto inflationDestStr =
            ae.inflationDest ? KeyUtils::toStrKey<PublicKey>(*ae.inflationDest)
                             : "";
        auto inflationDestInd = ae.inflationDest ? soci::i_ok : soci::i_null;
        auto homeDomainStr = decoder::encode_b64(ae.homeDomain);
        auto signersStr = decoder::encode_b64(xdr::xdr_to_opaque(ae.signers));
        auto thresholdsStr = decoder::encode_b64(ae.thresholds);

        soci::transaction tx(session);

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

    auto addOneOldSchemaTrustLine = [](SchemaUpgradeTestApplication& app,
                                       TLOptLiabilitiesPair const& tlol) {
        auto const& tl = tlol.first;
        auto const& liabilities = tlol.second;
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
            soci::use(accountIDStr), soci::use(assetType), soci::use(issuerStr),
            soci::use(assetCodeStr), soci::use(tl.limit), soci::use(tl.balance),
            soci::use(tl.flags),
            soci::use(app.getLedgerManager().getLastClosedLedgerNum()),
            soci::use(liabilities ? liabilities->buying : 0),
            soci::use(liabilities ? liabilities->selling : 0);

        if (!liabilities)
        {
            session << "UPDATE trustlines SET buyingliabilities = NULL, "
                       "sellingliabilities = NULL WHERE "
                       "accountid = :id AND issuer = :v1 AND assetcode = :v2",
                soci::use(accountIDStr), soci::use(issuerStr),
                soci::use(assetCodeStr);
        }

        tx.commit();
    };

    auto addOneOldSchemaDataEntry = [](SchemaUpgradeTestApplication& app,
                                       DataEntry const& de) {
        auto& session = app.getDatabase().getSession();
        auto accountIDStr = KeyUtils::toStrKey<PublicKey>(de.accountID);
        auto dataNameStr = decoder::encode_b64(de.dataName);
        auto dataValueStr = decoder::encode_b64(de.dataValue);

        soci::transaction tx(session);

        session << "INSERT INTO accountdata ( "
                   "accountid, dataname, datavalue, lastmodified "
                   ") VALUES ( :id, :v1, :v2, :v3 )",
            soci::use(accountIDStr), soci::use(dataNameStr),
            soci::use(dataValueStr),
            soci::use(app.getLedgerManager().getLastClosedLedgerNum());

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

        session << "INSERT INTO offers ( "
                   "sellerid, offerid, sellingasset, buyingasset, "
                   "amount, pricen, priced, price, flags, lastmodified "
                   ") VALUES ( "
                   ":v1, :v2, :v3, :v4, :v5, :v6, :v7, :v8, :v9, :v10 "
                   ")",
            soci::use(sellerIDStr), soci::use(oe.offerID),
            soci::use(sellingStr), soci::use(buyingStr), soci::use(oe.amount),
            soci::use(oe.price.n), soci::use(oe.price.d), soci::use(price),
            soci::use(oe.flags),
            soci::use(app.getLedgerManager().getLastClosedLedgerNum());

        tx.commit();
    };

    auto prepOldSchemaDB = [addOneOldSchemaAccount, addOneOldSchemaTrustLine,
                            addOneOldSchemaDataEntry,
                            addOneOldSchemaOfferEntry](
                               SchemaUpgradeTestApplication& app,
                               AccOptLiabilitiesVec const& aols,
                               TLOptLiabilitiesVec const& tlols,
                               DataEntry const& de, OfferEntry const& oe) {
        try
        {
            for (auto aol : aols)
            {
                addOneOldSchemaAccount(app, aol);
            }
        }
        catch (std::exception& e)
        {
            CLOG(FATAL, "Database") << __func__ << ": exception " << e.what()
                                    << " while adding old-schema accounts";
            throw;
        }

        try
        {
            for (auto tlol : tlols)
            {
                addOneOldSchemaTrustLine(app, tlol);
            }
        }
        catch (std::exception& e)
        {
            CLOG(FATAL, "Database") << __func__ << ": exception " << e.what()
                                    << " while adding old-schema trustlines";
            throw;
        }

        try
        {
            addOneOldSchemaDataEntry(app, de);
        }
        catch (std::exception& e)
        {
            CLOG(FATAL, "Database") << __func__ << ": exception " << e.what()
                                    << " while adding old-schema data";
            throw;
        }

        try
        {
            addOneOldSchemaOfferEntry(app, oe);
        }
        catch (std::exception& e)
        {
            CLOG(FATAL, "Database") << __func__ << ": exception " << e.what()
                                    << " while adding old-schema offer";
            throw;
        }
    };

    auto testOneDBMode = [prepOldSchemaDB](Config::TestDbMode const dbMode) {
        // A vector of optional Liabilities entries, for each of which the test
        // will generate a valid account.
        auto const accOptLiabilities = {
            nullopt<Liabilities>(),
            make_optional<Liabilities>(Liabilities{12, 17}),
            nullopt<Liabilities>(),
            make_optional<Liabilities>(Liabilities{3, 0})};

        // A vector of optional Liabilities entries, for each of which the test
        // will generate a valid trustline.
        auto const tlOptLiabilities = {
            make_optional<Liabilities>(Liabilities{0, 6}),
            nullopt<Liabilities>(),
            make_optional<Liabilities>(Liabilities{0, 0}),
            make_optional<Liabilities>(Liabilities{5, 8}),
            nullopt<Liabilities>()};

        // Pair up each of the optional liabilities in tlOptLiabilities with a
        // new valid account.
        AccOptLiabilitiesVec accOptLiabilitiesVec;
        std::transform(accOptLiabilities.begin(), accOptLiabilities.end(),
                       std::back_inserter(accOptLiabilitiesVec),
                       [](OptLiabilities const& aol) {
                           return AccOptLiabilitiesPair(
                               LedgerTestUtils::generateValidAccountEntry(),
                               aol);
                       });

        // Pair up each of the optional liabilities in tlOptLiabilities with a
        // new valid trustline.
        TLOptLiabilitiesVec tlOptLiabilitiesVec;
        std::transform(tlOptLiabilities.begin(), tlOptLiabilities.end(),
                       std::back_inserter(tlOptLiabilitiesVec),
                       [](OptLiabilities const& tlol) {
                           return TLOptLiabilitiesPair(
                               LedgerTestUtils::generateValidTrustLineEntry(),
                               tlol);
                       });

        // Create a data entry to test that its extension and ledger
        // entry extension have the default (empty-XDR-union) values.
        auto de = LedgerTestUtils::generateValidDataEntry();

        // Create an offer entry to test that its extension and ledger
        // entry extension have the default (empty-XDR-union) values.
        auto oe = LedgerTestUtils::generateValidOfferEntry();

        // Create the application, with the code above that inserts old-schema
        // accounts and trustlines into the database injected between database
        // creation and upgrade.
        Config const& cfg = getTestConfig(0, dbMode);
        VirtualClock clock;
        Application::pointer app =
            createTestApplication<SchemaUpgradeTestApplication,
                                  SchemaUpgradeTestApplication::PreUpgradeFunc>(
                clock, cfg,
                [prepOldSchemaDB, accOptLiabilitiesVec, tlOptLiabilitiesVec, de,
                 oe](SchemaUpgradeTestApplication& sapp) {
                    prepOldSchemaDB(sapp, accOptLiabilitiesVec,
                                    tlOptLiabilitiesVec, de, oe);
                });
        app->start();

        // Validate that the accounts and trustlines have the expected
        // liabilities and ledger entry extensions now that the database upgrade
        // has completed.

        LedgerTxn ltx(app->getLedgerTxnRoot());

        for (auto aol : accOptLiabilitiesVec)
        {
            LedgerKey key;
            key.type(ACCOUNT);
            key.account().accountID = aol.first.accountID;
            auto acc = ltx.load(key);
            REQUIRE(acc.current().data.type() == ACCOUNT);
            REQUIRE(acc.current().data.account().accountID ==
                    aol.first.accountID);
            REQUIRE(acc.current().data.account().balance == aol.first.balance);
            REQUIRE(acc.current().data.account().flags == aol.first.flags);
            REQUIRE(acc.current().data.account().homeDomain ==
                    aol.first.homeDomain);
            REQUIRE(acc.current().data.account().inflationDest ==
                    aol.first.inflationDest);
            REQUIRE(acc.current().data.account().numSubEntries ==
                    aol.first.numSubEntries);
            REQUIRE(acc.current().data.account().seqNum == aol.first.seqNum);
            REQUIRE(acc.current().data.account().signers == aol.first.signers);
            REQUIRE(acc.current().data.account().thresholds ==
                    aol.first.thresholds);
            REQUIRE(acc.current().ext == LedgerEntry::_ext_t());
            REQUIRE(acc.current().ext.v() == 0);
            if (aol.second)
            {
                REQUIRE(acc.current().data.account().ext.v() == 1);
                REQUIRE(
                    acc.current().data.account().ext.v1().liabilities.buying ==
                    aol.second->buying);
                REQUIRE(
                    acc.current().data.account().ext.v1().liabilities.selling ==
                    aol.second->selling);
            }
            else
            {
                REQUIRE(acc.current().data.account().ext.v() == 0);
            }
        }

        for (auto tlol : tlOptLiabilitiesVec)
        {
            LedgerKey key;
            key.type(TRUSTLINE);
            key.trustLine().accountID = tlol.first.accountID;
            key.trustLine().asset = tlol.first.asset;
            auto tl = ltx.load(key);
            REQUIRE(tl.current().data.type() == TRUSTLINE);
            REQUIRE(tl.current().data.trustLine().accountID ==
                    tlol.first.accountID);
            REQUIRE(tl.current().data.trustLine().asset == tlol.first.asset);
            REQUIRE(tl.current().data.trustLine().balance ==
                    tlol.first.balance);
            REQUIRE(tl.current().data.trustLine().flags == tlol.first.flags);
            REQUIRE(tl.current().data.trustLine().limit == tlol.first.limit);
            REQUIRE(tl.current().ext == LedgerEntry::_ext_t());
            REQUIRE(tl.current().ext.v() == 0);
            if (tlol.second)
            {
                REQUIRE(tl.current().data.trustLine().ext.v() == 1);
                REQUIRE(
                    tl.current().data.trustLine().ext.v1().liabilities.buying ==
                    tlol.second->buying);
                REQUIRE(tl.current()
                            .data.trustLine()
                            .ext.v1()
                            .liabilities.selling == tlol.second->selling);
            }
            else
            {
                REQUIRE(tl.current().data.trustLine().ext.v() == 0);
            }
        }

        {
            LedgerKey key;
            key.type(DATA);
            key.data().accountID = de.accountID;
            key.data().dataName = de.dataName;
            auto data = ltx.load(key);
            REQUIRE(data.current().data.type() == DATA);
            REQUIRE(data.current().data.data() == de);
            REQUIRE(data.current().ext == LedgerEntry::_ext_t());
            REQUIRE(data.current().ext.v() == 0);
            REQUIRE(data.current().data.data().ext == DataEntry::_ext_t());
            REQUIRE(data.current().data.data().ext.v() == 0);
        }

        {
            LedgerKey key;
            key.type(OFFER);
            key.offer().offerID = oe.offerID;
            key.offer().sellerID = oe.sellerID;
            auto offer = ltx.load(key);
            REQUIRE(offer.current().data.type() == OFFER);
            REQUIRE(offer.current().data.offer() == oe);
            REQUIRE(offer.current().ext == LedgerEntry::_ext_t());
            REQUIRE(offer.current().ext.v() == 0);
            REQUIRE(offer.current().data.offer().ext == OfferEntry::_ext_t());
            REQUIRE(offer.current().data.offer().ext.v() == 0);
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
