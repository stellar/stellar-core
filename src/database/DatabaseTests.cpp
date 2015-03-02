// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "util/asio.h"
#include "database/Database.h"
#include "main/Application.h"
#include "main/Config.h"
#include "main/test.h"
#include "crypto/Hex.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include "lib/catch.hpp"

using namespace stellar;

void transactionTest(Application::pointer app)
{
    int a = 10, b = 0;
    int a0 = a + 1;
    int a1 = a + 2;

    auto& session = app->getDatabase().getSession();

    session << "drop table if exists test";
    session << "create table test (x integer)";

    {
        soci::transaction tx(session);

        session << "insert into test (x) values (:aa)", soci::use(a0, "aa");

        session << "select x from test", soci::into(b);
        CHECK(a0 == b);

        {
            soci::transaction tx2(session);
            session << "update test set x = :v", soci::use(a1, "v");
            tx2.rollback();
        }

        session << "select x from test", soci::into(b);
        CHECK(a0 == b);

        {
            soci::transaction tx3(session);
            session << "update test set x = :v", soci::use(a, "v");
            tx3.commit();
        }
        session << "select x from test", soci::into(b);
        CHECK(a == b);


        tx.commit();
    }

    session << "select x from test", soci::into(b);
    CHECK(a == b);
}

TEST_CASE("database smoketest", "[db]")
{
    Config cfg;
    cfg.RUN_STANDALONE=true;
    VirtualClock clock;
    cfg.DATABASE = "sqlite3://:memory:";
    Application::pointer app = Application::create(clock, cfg);
    transactionTest(app);
}

#ifdef USE_POSTGRES
TEST_CASE("postgres smoketest", "[db]")
{
    Config cfg;
    cfg.RUN_STANDALONE = true;
    VirtualClock clock;
    cfg.DATABASE = "postgresql://host=localhost dbname=test user=test password=test";
    try
    {
        Application::pointer app = Application::create(clock, cfg);

        int a = 10, b = 0;

        auto& session = app->getDatabase().getSession();

        SECTION("round trip")
        {
            transactionTest(app);
        }

        SECTION("blob storage")
        {
            soci::transaction tx(session);
            std::vector<uint8_t> x = { 0, 1, 2, 3, 4, 5, 6}, y;
            soci::blob blobX(session);
            blobX.append(reinterpret_cast<char const*>(x.data()), x.size());
            session << "drop table if exists test";
            session << "create table test (a integer, b oid)";
            session << "insert into test (a, b) values (:aa, :bb)",
                soci::use(a, "aa"), soci::use(blobX, "bb");

            soci::blob blobY(session);
            session << "select a, b from test", soci::into(b), soci::into(blobY);
            y.resize(blobY.get_len());
            blobY.read(0, reinterpret_cast<char*>(y.data()), y.size());
            CHECK(x == y);
            LOG(DEBUG) << "blob round trip with postgresql database: "
                       << binToHex(x) << " == " << binToHex(y);
            tx.commit();
        }

    }
    catch(soci::soci_error& err)
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
