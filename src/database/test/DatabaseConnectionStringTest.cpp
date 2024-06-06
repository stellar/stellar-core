// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "database/DatabaseConnectionString.h"
#include "lib/catch.hpp"
#include <soci.h>

using namespace stellar;

TEST_CASE("remove password from database connection string",
          "[db][dbconnectionstring]")
{
    SECTION("empty connection string remains empty")
    {
        REQUIRE(removePasswordFromConnectionString("") == "");
    }

    SECTION("password is removed if first")
    {
        REQUIRE(removePasswordFromConnectionString(
                    R"(sqlite3://password=abc dbname=stellar)") ==
                R"(sqlite3://password=******** dbname=stellar)");
    }

    SECTION("password is removed if second")
    {
        REQUIRE(removePasswordFromConnectionString(
                    R"(sqlite3://dbname=stellar password=dbname)") ==
                R"(sqlite3://dbname=stellar password=********)");
    }

    SECTION("database can be named password")
    {
        REQUIRE(removePasswordFromConnectionString(
                    R"(sqlite3://dbname=password password=dbname)") ==
                R"(sqlite3://dbname=password password=********)");
    }

    SECTION("quoted password is removed")
    {
        REQUIRE(
            removePasswordFromConnectionString(
                R"(sqlite3://dbname=stellar password='quoted password')") ==
            R"(sqlite3://dbname=stellar password=********)");
    }

    SECTION("quoted password with quote is removed")
    {
        REQUIRE(
            removePasswordFromConnectionString(
                R"(sqlite3://dbname=stellar password='quoted \' password')") ==
            R"(sqlite3://dbname=stellar password=********)");
    }

    SECTION("quoted password with backslash is removed")
    {
        REQUIRE(
            removePasswordFromConnectionString(
                R"(sqlite3://dbname=stellar password='quoted \\ password')") ==
            R"(sqlite3://dbname=stellar password=********)");
    }

    SECTION("quoted password with backslash and quote is removed")
    {
        REQUIRE(
            removePasswordFromConnectionString(
                R"(sqlite3://dbname=stellar password='quoted \\ password')") ==
            R"(sqlite3://dbname=stellar password=********)");
    }

    SECTION("parameters after password remain unchanged")
    {
        REQUIRE(
            removePasswordFromConnectionString(
                R"(sqlite3://dbname=stellar password='quoted \\ password' performance='as fast as possible')") ==
            R"(sqlite3://dbname=stellar password=******** performance='as fast as possible')");
    }

    SECTION("dbname can be quored")
    {
        REQUIRE(
            removePasswordFromConnectionString(
                R"(sqlite3://dbname='stellar with spaces' password='quoted \\ password' performance='as fast as possible')") ==
            R"(sqlite3://dbname='stellar with spaces' password=******** performance='as fast as possible')");
    }

    SECTION("spaces before equals are accepted")
    {
        REQUIRE(
            removePasswordFromConnectionString(
                R"(sqlite3://dbname ='stellar with spaces' password ='quoted \\ password' performance ='as fast as possible')") ==
            R"(sqlite3://dbname ='stellar with spaces' password =******** performance ='as fast as possible')");
    }

    SECTION("spaces after equals are accepted")
    {
        REQUIRE(
            removePasswordFromConnectionString(
                R"(sqlite3://dbname= 'stellar with spaces' password= 'quoted \\ password' performance= 'as fast as possible')") ==
            R"(sqlite3://dbname= 'stellar with spaces' password= ******** performance= 'as fast as possible')");
    }

    SECTION("spaces around equals are accepted")
    {
        REQUIRE(
            removePasswordFromConnectionString(
                R"(sqlite3://dbname = 'stellar with spaces' password = 'quoted \\ password' performance = 'as fast as possible')") ==
            R"(sqlite3://dbname = 'stellar with spaces' password = ******** performance = 'as fast as possible')");
    }

    SECTION(
        "invalid connection string without equals and value remains as it was")
    {
        REQUIRE(removePasswordFromConnectionString(
                    R"(sqlite3://dbname password=asbc)") ==
                R"(sqlite3://dbname password=asbc)");
    }

    SECTION("invalid connection string without value remains as it was")
    {
        REQUIRE(removePasswordFromConnectionString(
                    R"(sqlite3://dbname= password=asbc)") ==
                R"(sqlite3://dbname= password=asbc)");
    }

    SECTION("invalid connection string with unfinished quoted value")
    {
        REQUIRE(removePasswordFromConnectionString(
                    R"(sqlite3://dbname='quoted value)") ==
                R"(sqlite3://dbname='quoted value)");
    }

    SECTION("invalid connection string with quoted value with unfinished "
            "escape sequence")
    {
        REQUIRE(removePasswordFromConnectionString(
                    R"(sqlite3://dbname='quoted value\ password=abc)") ==
                R"(sqlite3://dbname='quoted value\ password=abc)");
    }

    SECTION("invalid connection string without backend name")
    {
        REQUIRE(
            removePasswordFromConnectionString(R"(dbname=name password=abc)") ==
            R"(dbname=name password=abc)");
    }

    SECTION("ignore sqlite3://:memory:")
    {
        REQUIRE(removePasswordFromConnectionString(R"(sqlite3://:memory:)") ==
                R"(sqlite3://:memory:)");
    }

    SECTION("Bug 2234 - barewords can be any non-whitespace")
    {
        // This case handles a mistake where we used to only match bareword
        // tokens using a '\w' pattern, which matches [_[:alnum:]], whereas we
        // really need to allow '\S' or [^[:space:]]. This manifests as a match
        // failure -- and thereby leads to a failure-to-scrub -- when someone
        // writes /some/path/with/slashes as a bareword. This is legal as a
        // token in a sqlite3 connect string, but we failed to recognize it
        // as such before.
        REQUIRE(
            removePasswordFromConnectionString(
                R"(sqlite3://dbname=stellar user=stellar password=thisshouldbesecret host=/var/run/sqlite3/)") ==
            R"(sqlite3://dbname=stellar user=stellar password=******** host=/var/run/sqlite3/)");
    }
}
