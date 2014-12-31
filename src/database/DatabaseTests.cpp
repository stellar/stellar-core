// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "main/Application.h"
#include "main/test.h"
#include "util/Logging.h"
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
