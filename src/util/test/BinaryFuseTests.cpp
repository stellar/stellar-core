// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/ShortHash.h"
#include "lib/catch.hpp"
#include "util/BinaryFuseFilter.h"
#include "util/types.h"
#include <xdrpp/autocheck.h>

using namespace stellar;

static auto ledgerKeyGenerator = autocheck::such_that(
    [](LedgerKey const& k) { return k.type() != CONFIG_SETTING; },
    autocheck::generator<LedgerKey>());

template <class FilterT>
void
testFilter(double expectedFalsePositiveRate)
{
    LedgerKeySet keys;
    for (size_t size = 100; size <= 1'000'000; size *= 100)
    {
        while (keys.size() < size)
        {
            auto k = ledgerKeyGenerator();
            keys.insert(k);
        }

        auto seed = shortHash::getShortHashInitKey();
        FilterT filter(keys, seed);

        for (auto const& k : keys)
        {
            REQUIRE(filter.contain(k));
        }

        size_t randomMatches = 0;
        size_t trials = 2'000'000;
        for (size_t i = 0; i < trials; i++)
        {
            LedgerKey randomKey;
            do
            {
                randomKey = ledgerKeyGenerator();
            } while (keys.find(randomKey) != keys.end());

            if (filter.contain(randomKey))
            {
                ++randomMatches;
            }
        }

        if (expectedFalsePositiveRate == 0)
        {
            // 32 bit filter has too small a false positive rate to test,
            // allow up to one false positive for test stability
            REQUIRE(randomMatches <= 1);
        }
        else
        {
            // False positive rate should be within 15% of the expected rate
            // The fpp is so small, we have to give a relatively large margin
            // to account for float imprecision
            double fpp = randomMatches * 1.0 / trials;
            double upperBound = expectedFalsePositiveRate * 1.15;
            REQUIRE(fpp < upperBound);
        }
    }
}

TEST_CASE("binary fuse filter", "[BinaryFuseFilter][!hide]")
{

    SECTION("8 bit")
    {
        auto epsilon = 1ul << 8;
        testFilter<BinaryFuseFilter8>(1.0 / epsilon);
    }

    SECTION("16 bit")
    {
        auto epsilon = 1ul << 16;
        testFilter<BinaryFuseFilter16>(1.0 / epsilon);
    }

    SECTION("32 bit")
    {
        // The actual false positive rate is 1/ 4 billion
        testFilter<BinaryFuseFilter32>(0);
    }
}

// TODO: Add serialization test