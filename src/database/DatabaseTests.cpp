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

TEST_CASE("database smoketest", "[db]")
{
    Config cfg;
    cfg.RUN_STANDALONE=true;
    VirtualClock clock;
    cfg.DATABASE = "sqlite3://:memory:";
    Application::pointer app = Application::create(clock, cfg);

    int a = 10, b = 0;

    auto& sql = app->getDatabase().getSession();

    sql << "create table test (x integer)";
    sql << "insert into test (x) values (:aa)", soci::use(a, "aa");
    sql << "select x from test", soci::into(b);

    CHECK(a == b);
    LOG(DEBUG) << "round trip with in-memory database: " << a << " == " << b;
}

#ifndef _WIN32
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

        auto& sql = app->getDatabase().getSession();

        SECTION("round trip")
        {
            sql << "drop table if exists test";
            sql << "create table test (x integer)";
            sql << "insert into test (x) values (:aa)", soci::use(a, "aa");
            sql << "select x from test", soci::into(b);
            CHECK(a == b);
            LOG(DEBUG) << "round trip with postgresql database: " << a << " == " << b;
        }

        SECTION("blob storage")
        {
            soci::transaction tx(sql);
            std::vector<uint8_t> x = { 0, 1, 2, 3, 4, 5, 6}, y;
            soci::blob blobX(sql);
            blobX.append(reinterpret_cast<char const*>(x.data()), x.size());
            sql << "drop table if exists test";
            sql << "create table test (a integer, b oid)";
            sql << "insert into test (a, b) values (:aa, :bb)",
                soci::use(a, "aa"), soci::use(blobX, "bb");

            soci::blob blobY(sql);
            sql << "select a, b from test", soci::into(b), soci::into(blobY);
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
#endif
