// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "database/Database.h"
#include "main/Application.h"
#include "main/Config.h"
#include "main/test.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include "lib/catch.hpp"

using namespace stellar;

TEST_CASE("database smoketest", "[db]")
{
    Config cfg;
    VirtualClock clock;
    cfg.DATABASE = "sqlite3://:memory:";
    Application app(clock, cfg);

    int a = 10, b = 0;

    auto& sql = app.getDatabase().getSession();

    sql << "create table test (x integer)";
    sql << "insert into test (x) values (:aa)", soci::use(a, "aa");
    sql << "select x from test", soci::into(b);

    CHECK(a == b);
    LOG(DEBUG) << "round trip with in-memory database: " << a << " == " << b;
}


#ifdef USE_POSTGRES
TEST_CASE("postgres smoketest", "[db]")
{
    Config cfg;
    VirtualClock clock;
    cfg.DATABASE = "postgresql://dbname=test user=test password=test";
    Application app(clock, cfg);

    int a = 10, b = 0;

    auto& sql = app.getDatabase().getSession();

    sql << "drop table if exists test";
    sql << "create table test (x integer)";
    sql << "insert into test (x) values (:aa)", soci::use(a, "aa");
    sql << "select x from test", soci::into(b);

    CHECK(a == b);
    LOG(DEBUG) << "round trip with postgresql database: " << a << " == " << b;
}
#endif
