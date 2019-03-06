// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/catch.hpp"
#include "lib/util/lrucache.hpp"

namespace stellar
{

using IntCache = cache::lru_cache<int, int>;

TEST_CASE("empty", "[lru_cache]")
{
    auto c = IntCache{5};

    REQUIRE(c.size() == 0);
}

TEST_CASE("keeps last added items", "[lru_cache]")
{
    auto c = IntCache{5};
    c.put(0, 0);
    REQUIRE(c.size() == 1);
    c.put(1, 1);
    REQUIRE(c.size() == 2);
    c.put(2, 2);
    REQUIRE(c.size() == 3);
    c.put(3, 3);
    REQUIRE(c.size() == 4);
    c.put(4, 4);
    REQUIRE(c.size() == 5);
    c.put(5, 5);
    REQUIRE(c.size() == 5);

    REQUIRE(!c.exists(0));
    REQUIRE(c.exists(1));
    REQUIRE(c.exists(2));
    REQUIRE(c.exists(3));
    REQUIRE(c.exists(4));
    REQUIRE(c.exists(4));
}

TEST_CASE("keeps last read items", "[lru_cache]")
{
    auto c = IntCache{5};
    c.put(0, 0);
    c.put(1, 1);
    c.put(2, 2);
    c.put(3, 3);
    c.put(4, 4);
    c.get(0);
    c.put(5, 5);

    REQUIRE(c.size() == 5);
    REQUIRE(c.exists(0));
    REQUIRE(!c.exists(1));
    REQUIRE(c.exists(2));
    REQUIRE(c.exists(3));
    REQUIRE(c.exists(4));
    REQUIRE(c.exists(4));
}

TEST_CASE("replace element", "[lru_cache]")
{
    auto c = IntCache{5};
    c.put(0, 0);
    REQUIRE(c.get(0) == 0);
    c.put(0, 1);
    REQUIRE(c.get(0) == 1);
    c.put(0, 2);
    REQUIRE(c.get(0) == 2);
    c.put(0, 3);
    REQUIRE(c.get(0) == 3);
    c.put(0, 4);
    REQUIRE(c.get(0) == 4);
}

TEST_CASE("erase_if removes some nodes", "[lru_cache]")
{
    auto c = IntCache{5};
    c.put(0, 0);
    c.put(1, 1);
    c.put(2, 2);
    c.put(3, 3);
    c.put(4, 4);
    c.erase_if([](int i) { return i % 2 == 0; });

    REQUIRE(c.size() == 2);
    REQUIRE(!c.exists(0));
    REQUIRE(c.exists(1));
    REQUIRE(!c.exists(2));
    REQUIRE(c.exists(3));
    REQUIRE(!c.exists(4));
}

TEST_CASE("erase_if removes no nodes", "[lru_cache]")
{
    auto c = IntCache{5};
    c.put(0, 0);
    c.put(1, 1);
    c.put(2, 2);
    c.put(3, 3);
    c.put(4, 4);
    c.erase_if([](int) { return false; });

    REQUIRE(c.size() == 5);
    REQUIRE(c.exists(0));
    REQUIRE(c.exists(1));
    REQUIRE(c.exists(2));
    REQUIRE(c.exists(3));
    REQUIRE(c.exists(4));
}

TEST_CASE("erase_if removes all nodes", "[lru_cache]")
{
    auto c = IntCache{5};
    c.put(0, 0);
    c.put(1, 1);
    c.put(2, 2);
    c.put(3, 3);
    c.put(4, 4);
    c.erase_if([](int) { return true; });

    REQUIRE(c.size() == 0);
    REQUIRE(!c.exists(0));
    REQUIRE(!c.exists(1));
    REQUIRE(!c.exists(2));
    REQUIRE(!c.exists(3));
    REQUIRE(!c.exists(4));
}
}
