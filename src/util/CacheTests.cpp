// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/catch.hpp"
#include "util/CacheTable.h"

using namespace stellar;

TEST_CASE("cachetable works as a cache", "[cachetable]")
{
    size_t sz = 1000;
    CacheTable<int, int> cache(sz);
    auto const& ctrs = cache.getCounters();

    // Fill the cache
    for (auto i = 0; i < sz; ++i)
    {
        cache.put(i, i * 100);
    }
    for (auto i = 0; i < sz; ++i)
    {
        auto p = cache.get(i);
        REQUIRE(p == i * 100);
    }
    REQUIRE(ctrs.mInserts == sz);
    REQUIRE(ctrs.mUpdates == 0);
    REQUIRE(ctrs.mHits == sz);
    REQUIRE(ctrs.mMisses == 0);
    REQUIRE(ctrs.mEvicts == 0);

    // Clobber the entries
    for (auto i = 0; i < sz; ++i)
    {
        cache.put(i, i * 200);
    }
    for (auto i = 0; i < sz; ++i)
    {
        auto p = cache.get(i);
        REQUIRE(p == i * 200);
    }
    REQUIRE(ctrs.mInserts == sz);
    REQUIRE(ctrs.mUpdates == sz);
    REQUIRE(ctrs.mHits == 2 * sz);
    REQUIRE(ctrs.mMisses == 0);
    REQUIRE(ctrs.mEvicts == 0);

    // Add some entries and evict some.
    for (auto i = 0; i < sz / 2; ++i)
    {
        cache.put((int)sz + i * (int)sz, i);
    }
    REQUIRE(ctrs.mInserts == sz + sz / 2);
    REQUIRE(ctrs.mUpdates == sz);
    REQUIRE(ctrs.mHits == 2 * sz);
    REQUIRE(ctrs.mMisses == 0);
    REQUIRE(ctrs.mEvicts == sz / 2);

    // Check that enough un-evicted entries are still there.
    for (auto i = 0; i < sz; ++i)
    {
        cache.maybeGet(i);
    }
    REQUIRE(ctrs.mInserts == sz + sz / 2);
    REQUIRE(ctrs.mUpdates == sz);
    REQUIRE(ctrs.mHits >= 2 * sz + sz / 2);
    REQUIRE(ctrs.mMisses <= sz / 2);
    REQUIRE(ctrs.mEvicts == sz / 2);
}

TEST_CASE("cachetable does not thrash", "[cachetablethrash][!hide]")
{
    gRandomEngine.seed(time(nullptr));
    size_t sz = 1000;
    CacheTable<int, int> cache(sz);
    auto const& ctrs = cache.getCounters();
    // Fill the cache and then over-fill it by 1, so it expires an entry. In
    // LRU-land this can be the beginning of the end.
    for (auto i = 0; i <= sz; ++i)
    {
        cache.put(i, i * 100);
    }
    REQUIRE(ctrs.mInserts == sz + 1);
    REQUIRE(ctrs.mEvicts == 1);
    // Access sequentially in the same order as the first time. If we were doing
    // LRU this would be the bad case.
    for (auto i = 0; i <= sz; ++i)
    {
        if (!cache.maybeGet(i))
        {
            cache.put(i, i * 100);
        }
    }
    // These are statistical: it's theoretically possible that a terrible stroke
    // of PRNG luck can cause a sequence of evictions right in front of us over
    // and over, but the odds are good that we get only a few evictions through
    // the second pass. Anyway since they're not _guaranteed_ this test is
    // hidden. Run it in a while loop to check manually.
    REQUIRE(ctrs.mInserts < sz + 10);
    REQUIRE(ctrs.mUpdates == 0);
    REQUIRE(ctrs.mMisses < 10);
    REQUIRE(ctrs.mEvicts < 11);
}

// Tests from previous lru_cache tests

using IntCache = CacheTable<int, int>;

TEST_CASE("cachetable empty", "[cachetable]")
{
    auto c = IntCache{5};

    REQUIRE(c.size() == 0);
}

TEST_CASE("cachetable keeps most added items", "[cachetable]")
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

    size_t existing = 0;
    for (int i = 0; i <= 5; ++i)
    {
        if (c.exists(i))
        {
            ++existing;
        }
    }
    REQUIRE(existing == 5);
}

TEST_CASE("cachetable keeps last read items", "[cachetable]")
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
    size_t existing = 0;
    for (int i = 0; i <= 5; ++i)
    {
        if (c.exists(i))
        {
            ++existing;
        }
    }
    REQUIRE(existing == 5);
}

TEST_CASE("cachetable replace element", "[cachetable]")
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

TEST_CASE("cachetable erase_if removes some nodes", "[cachetable]")
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

TEST_CASE("cachetable erase_if removes no nodes", "[cachetable]")
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

TEST_CASE("cachetable erase_if removes all nodes", "[cachetable]")
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
