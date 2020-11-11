// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/catch.hpp"
#include "util/RandomEvictionCache.h"
#include <ctime>
#include <map>

using namespace stellar;

TEST_CASE("cachetable works as a cache", "[randomevictioncache]")
{
    size_t sz = 1000;
    RandomEvictionCache<int, int> cache(sz);
    auto const& ctrs = cache.getCounters();

    // Fill the cache
    for (int i = 0; i < sz; ++i)
    {
        cache.put(i, i * 100);
    }
    for (int i = 0; i < sz; ++i)
    {
        auto p = cache.get(i);
        REQUIRE(p == i * 100);
    }
    for (int i = 0; i < sz; ++i)
    {
        auto p = *cache.maybeGet(i);
        REQUIRE(p == i * 100);
    }
    REQUIRE(ctrs.mInserts == sz);
    REQUIRE(ctrs.mUpdates == 0);
    REQUIRE(ctrs.mHits == 2 * sz);
    REQUIRE(ctrs.mMisses == 0);
    REQUIRE(ctrs.mEvicts == 0);

    // Clobber the entries
    for (int i = 0; i < sz; ++i)
    {
        cache.put(i, i * 200);
    }
    for (int i = 0; i < sz; ++i)
    {
        int p = cache.get(i);
        REQUIRE(p == i * 200);
    }
    for (int i = 0; i < sz; ++i)
    {
        int p = *cache.maybeGet(i);
        REQUIRE(p == i * 200);
    }
    REQUIRE(ctrs.mInserts == sz);
    REQUIRE(ctrs.mUpdates == sz);
    REQUIRE(ctrs.mHits == 4 * sz);
    REQUIRE(ctrs.mMisses == 0);
    REQUIRE(ctrs.mEvicts == 0);

    // Add some entries and evict some.
    for (int i = 0; i < sz / 2; ++i)
    {
        cache.put((int)sz + i * (int)sz, i);
    }
    REQUIRE(ctrs.mInserts == sz + sz / 2);
    REQUIRE(ctrs.mUpdates == sz);
    REQUIRE(ctrs.mHits == 4 * sz);
    REQUIRE(ctrs.mMisses == 0);
    REQUIRE(ctrs.mEvicts == sz / 2);

    // Check that enough un-evicted entries are still there.
    for (int i = 0; i < sz; ++i)
    {
        if (cache.exists(i))
        {
            cache.get(i);
        }
    }
    REQUIRE(ctrs.mInserts == sz + sz / 2);
    REQUIRE(ctrs.mUpdates == sz);
    REQUIRE(ctrs.mHits >= 4 * sz + sz / 2);
    REQUIRE(ctrs.mMisses <= sz / 2);
    REQUIRE(ctrs.mEvicts == sz / 2);

    // Check that enough un-evicted entries are still there.
    for (int i = 0; i < sz; ++i)
    {
        cache.maybeGet(i);
    }
    REQUIRE(ctrs.mInserts == sz + sz / 2);
    REQUIRE(ctrs.mUpdates == sz);
    REQUIRE(ctrs.mHits >= 5 * sz);
    REQUIRE(ctrs.mMisses <= sz);
    REQUIRE(ctrs.mEvicts == sz / 2);

    // Ensure that maybeGet returns nullptr if and only if
    // cache.exists return false.
    for (int i = 0; i < sz; i++)
    {
        REQUIRE(cache.exists(i) == (cache.maybeGet(i) != nullptr));
    }
}

TEST_CASE("cachetable does not thrash", "[randomevictioncachethrash][!hide]")
{
    gRandomEngine.seed(std::time(nullptr) & UINT32_MAX);
    size_t sz = 1000;
    RandomEvictionCache<int, int> cache(sz);
    auto const& ctrs = cache.getCounters();
    // Fill the cache and then over-fill it by 1, so it expires an entry. In
    // LRU-land this can be the beginning of the end.
    for (int i = 0; i <= sz; ++i)
    {
        cache.put(i, i * 100);
    }
    REQUIRE(ctrs.mInserts == sz + 1);
    REQUIRE(ctrs.mEvicts == 1);
    // Access sequentially in the same order as the first time. If we were doing
    // LRU this would be the bad case.
    for (int i = 0; i <= sz; ++i)
    {
        if (i % 2 == 0)
        {
            if (cache.exists(i))
            {
                cache.get(i);
            }
            else
            {
                cache.put(i, i * 100);
            }
        }
        else
        {
            auto p = cache.maybeGet(i);
            if (p == nullptr)
            {
                cache.put(i, i * 100);
            }
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

using RandCache = RandomEvictionCache<int, int>;

TEMPLATE_TEST_CASE("cache empty", "[cache][template]", RandCache)
{
    TestType c{5};

    REQUIRE(c.size() == 0);
}

TEMPLATE_TEST_CASE("cache keeps most added items", "[cache][template]",
                   RandCache)
{
    TestType c{5};
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

TEMPLATE_TEST_CASE("cache keeps last read items", "[cache][template]",
                   RandCache)
{
    TestType c{5};
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

TEMPLATE_TEST_CASE("cache keeps last read items with maybeGet",
                   "[cache][template]", RandCache)
{
    TestType c{5};
    c.put(0, 0);
    c.put(1, 1);
    c.put(2, 2);
    c.put(3, 3);
    c.put(4, 4);
    c.maybeGet(0);
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

TEMPLATE_TEST_CASE("cache replace element", "[cache][template]", RandCache)
{
    TestType c{5};
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

    TestType d{5};
    d.put(0, 0);
    REQUIRE(*d.maybeGet(0) == 0);
    d.put(0, 1);
    REQUIRE(*d.maybeGet(0) == 1);
    d.put(0, 2);
    REQUIRE(*d.maybeGet(0) == 2);
    d.put(0, 3);
    REQUIRE(*d.maybeGet(0) == 3);
    d.put(0, 4);
    REQUIRE(*d.maybeGet(0) == 4);
}

TEMPLATE_TEST_CASE("cache erase_if removes some nodes", "[cache][template]",
                   RandCache)
{
    TestType c{5};
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

TEMPLATE_TEST_CASE("cache erase_if removes no nodes", "[cache][template]",
                   RandCache)
{
    TestType c{5};
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

TEMPLATE_TEST_CASE("cache erase_if removes all nodes", "[cache][template]",
                   RandCache)
{
    TestType c{5};
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
