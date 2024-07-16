// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/catch.hpp"
#include "lib/util/stdrandom.h"
#include "util/BinaryFuseFilter.h"

using namespace stellar;

template <class T>
void
testFilter(size_t size, double expectedFalsePositiveRate)
{
    // Allocate vector of contiguous values [0, 1, 2, ..., size-1]
    std::vector<size_t> hashes(size);
    std::iota(hashes.begin(), hashes.end(), 0);

    T filter(size);

    stellar::uniform_int_distribution<uint64_t> seedDist;
    uint64_t seed = seedDist(Catch::rng());

    REQUIRE(filter.populate(hashes, seed));
    for (size_t hash = 0; hash < size; ++hash)
    {
        REQUIRE(filter.contain(hash));
    }

    size_t randomMatches = 0;
    size_t trials = 100000000;

    // Make sure none of the random keys are in the filter
    stellar::uniform_int_distribution<uint64_t> hashDist(size + 1);

    for (size_t i = 0; i < trials; i++)
    {
        uint64_t randomKey = hashDist(Catch::rng());
        if (filter.contain(randomKey))
        {
            ++randomMatches;
        }
    }

    if (expectedFalsePositiveRate == 0)
    {
        // 32 bit filter has too small a false positive rate to test
        REQUIRE(randomMatches == 0);
    }
    else
    {
        // False positive rate should be within 10% of the expected rate
        double fpp = randomMatches * 1.0 / trials;
        double upperBound = expectedFalsePositiveRate * 1.1;
        double lowerBound = expectedFalsePositiveRate * 0.9;
        REQUIRE(fpp > lowerBound);
        REQUIRE(fpp < upperBound);
    }
}

TEST_CASE("binary fuse filter", "[BinaryFuseFilter]")
{
    for (size_t size = 1000; size <= 1000000; size *= 300)
    {
        SECTION("8 bit")
        {
            auto epsilon = 1ul << 8;
            testFilter<BinaryFuseFilter8>(size, 1.0 / epsilon);
        }

        SECTION("16 bit")
        {
            auto epsilon = 1ul << 16;
            testFilter<BinaryFuseFilter16>(size, 1.0 / epsilon);
        }

        SECTION("32 bit")
        {
            // The actual false positive rate is 1/ 4 billion
            testFilter<BinaryFuseFilter32>(size, 0);
        }
    }

    // TODO: Add duplicate test
    // TODO: Add serialization test
}