// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/catch.hpp"
#include "util/BitSet.h"
#include "util/Math.h"
#include "util/UnorderedSet.h"
#include <set>

using namespace stellar;

TEST_CASE("BitSet basics", "[bitset]")
{
    std::set<size_t> ref_even{0, 2, 4, 6};
    std::set<size_t> ref_odd{1, 3, 5, 7};

    BitSet bs_even(ref_even);
    BitSet bs_odd(ref_odd);
    for (auto i : ref_even)
    {
        REQUIRE(bs_even.get(i));
        REQUIRE(!bs_odd.get(i));
        REQUIRE(!bs_even.get(i + 1));
        REQUIRE(bs_odd.get(i + 1));
    }
    REQUIRE(bs_even.isSubsetEq(bs_even | bs_odd));
    REQUIRE(bs_odd.isSubsetEq(bs_even | bs_odd));
    REQUIRE(bs_odd.isSubsetEq(bs_odd));
    REQUIRE(bs_even.isSubsetEq(bs_even));
    REQUIRE(!bs_odd.isSubsetEq(bs_even));
    REQUIRE(!bs_even.isSubsetEq(bs_odd));
    REQUIRE((bs_even | bs_odd) == (bs_odd | bs_even));
    REQUIRE((bs_even & bs_odd).empty());
    REQUIRE((bs_even & (bs_even | bs_odd)) == bs_even);
    REQUIRE(bs_even.count() == bs_odd.count());
    REQUIRE(bs_even.count() == ref_even.size());
    REQUIRE(bs_even.min() == *ref_even.begin());
    REQUIRE(bs_even.max() == *ref_even.rbegin());
    REQUIRE(bs_odd.min() == *ref_odd.begin());
    REQUIRE(bs_odd.max() == *ref_odd.rbegin());
    BitSet e = bs_even;
    e.clear();
    REQUIRE(e.count() == 0);
    e.set(1);
    REQUIRE(e.count() == 1);
    e.set(100);
    REQUIRE(e.count() == 2);
    e.set(1000);
    REQUIRE(e.count() == 3);
    e |= bs_odd;
    REQUIRE(e.count() == (2 + ref_odd.size()));
    e -= bs_odd;
    REQUIRE(e.count() == 2);
    e.unset(1000);
    REQUIRE(e.count() == 1);
    e.unset(100);
    REQUIRE(e.count() == 0);
}

TEST_CASE("BitSet union", "[bitset]")
{
    for (size_t loop = 0; loop < 100; ++loop)
    {
        UnorderedSet<size_t> ref;
        BitSet bs_a, bs_b;
        for (size_t i = 0; i < rand_uniform<size_t>(size_t(1), size_t(100));
             ++i)
        {
            ref.insert(i);
            bs_a.set(i);
        }
        for (size_t i = 0; i < rand_uniform<size_t>(size_t(1), size_t(100));
             ++i)
        {
            ref.insert(i);
            bs_b.set(i);
        }
        BitSet bs_c = bs_a | bs_b;
        REQUIRE(bs_a.unionCount(bs_b) == ref.size());
        for (size_t i = 0; bs_c.nextSet(i); ++i)
        {
            REQUIRE(ref.find(i) != ref.end());
        }
        for (auto const& i : ref)
        {
            REQUIRE(bs_c.get(i));
        }
        REQUIRE(bs_c.count() == ref.size());
    }
}

TEST_CASE("BitSet intersection", "[bitset]")
{
    for (size_t loop = 0; loop < 100; ++loop)
    {
        UnorderedSet<size_t> ref;
        BitSet bs_a, bs_b;
        for (size_t i = 0; i < rand_uniform<size_t>(size_t(1), size_t(100));
             ++i)
        {
            bs_a.set(i);
            if (rand_flip())
            {
                ref.insert(i);
                bs_b.set(i);
            }
        }
        BitSet bs_c = bs_a & bs_b;
        REQUIRE(bs_a.intersectionCount(bs_b) == ref.size());
        for (size_t i = 0; bs_c.nextSet(i); ++i)
        {
            REQUIRE(ref.find(i) != ref.end());
        }
        for (auto const& i : ref)
        {
            REQUIRE(bs_c.get(i));
        }
        REQUIRE(bs_c.count() == ref.size());
    }
}

TEST_CASE("BitSet difference", "[bitset]")
{
    for (size_t loop = 0; loop < 100; ++loop)
    {
        UnorderedSet<size_t> ref;
        BitSet bs_a, bs_b;
        for (size_t i = 0; i < rand_uniform<size_t>(size_t(1), size_t(100));
             ++i)
        {
            bs_a.set(i);
            if (rand_flip())
            {
                bs_b.set(i);
            }
            else
            {
                ref.insert(i);
            }
        }
        BitSet bs_c = bs_a - bs_b;
        REQUIRE(bs_a.differenceCount(bs_b) == ref.size());
        for (size_t i = 0; bs_c.nextSet(i); ++i)
        {
            REQUIRE(ref.find(i) != ref.end());
        }
        for (auto const& i : ref)
        {
            REQUIRE(bs_c.get(i));
        }
        REQUIRE(bs_c.count() == ref.size());
    }
}

TEST_CASE("BitSet symmetric difference", "[bitset]")
{
    for (size_t loop = 0; loop < 100; ++loop)
    {
        UnorderedSet<size_t> ref;
        BitSet bs_a, bs_b;
        for (size_t i = 0; i < rand_uniform<size_t>(size_t(1), size_t(100));
             ++i)
        {
            if (rand_flip())
            {
                bs_a.set(i);
                bs_b.set(i);
            }
            else
            {
                if (rand_flip())
                {
                    bs_a.set(i);
                }
                else
                {
                    bs_b.set(i);
                }
                ref.insert(i);
            }
        }
        BitSet bs_c = bs_a.symmetricDifference(bs_b);
        REQUIRE(bs_a.symmetricDifferenceCount(bs_b) == ref.size());
        for (size_t i = 0; bs_c.nextSet(i); ++i)
        {
            REQUIRE(ref.find(i) != ref.end());
        }
        for (auto const& i : ref)
        {
            REQUIRE(bs_c.get(i));
        }
        REQUIRE(bs_c.count() == ref.size());
    }
}
