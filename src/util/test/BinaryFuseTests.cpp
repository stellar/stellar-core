// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/ShortHash.h"
#include "test/Catch2.h"
#include "test/CovMark.h"
#include "util/BinaryFuseFilter.h"
#include "util/XDRCereal.h"
#include "util/types.h"
#include "xdr/Stellar-types.h"
#include <cereal/archives/binary.hpp>
#include <siphash.h>
#include <sstream>
#include <xdrpp/autocheck.h>
#include <xdrpp/marshal.h>

using namespace stellar;

static auto ledgerKeyGenerator = autocheck::such_that(
    [](LedgerKey const& k) { return k.type() != CONFIG_SETTING; },
    autocheck::generator<LedgerKey>());

template <class FilterT>
static void
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

        std::vector<uint64_t> hashes;
        hashes.reserve(keys.size());
        for (auto const& k : keys)
        {
            auto keyBuf = xdr::xdr_to_opaque(k);

            SipHash24 hasher(seed.data());
            hasher.update(keyBuf.data(), keyBuf.size());
            hashes.emplace_back(hasher.digest());
        }

        // Test in-memory filter, serialize, deserialize, then test again
        std::stringstream ss;
        FilterT inMemoryFilter(hashes, seed);
        {
            for (auto const& k : keys)
            {
                REQUIRE(inMemoryFilter.contains(k));
            }

            cereal::BinaryOutputArchive oarchive(ss);
            oarchive(inMemoryFilter);
        }

        SerializedBinaryFuseFilter xdrFilter;
        {
            cereal::BinaryInputArchive iarchive(ss);
            iarchive(xdrFilter);
        }

        FilterT deserializedFilter(xdrFilter);
        REQUIRE(deserializedFilter == inMemoryFilter);

        // Repeat correctness check for deserialized filter
        for (auto const& k : keys)
        {
            REQUIRE(deserializedFilter.contains(k));
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

            // Make sure both filters have identical behavior
            if (inMemoryFilter.contains(randomKey))
            {
                REQUIRE(deserializedFilter.contains(randomKey));
                ++randomMatches;
            }
            else
            {
                REQUIRE(!deserializedFilter.contains(randomKey));
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

// During populate(), the t2count array can underflow when the hash distribution
// is degenerate (many keys collide into the same filter positions). This sets
// the error flag, which triggers a reset-and-retry with a rotated seed. This
// test forces the error path and verifies the retry produces a correct filter.
TEST_CASE("binary fuse filter error retry", "[BinaryFuseFilter]")
{
    COVMARK_CHECK_HIT_IN_CURR_SCOPE(BINARY_FUSE_POPULATE_ERROR_RETRY);

    // Force the error path on the first populate iteration
    gBinaryFuseForcePopulateError = true;

    LedgerKeySet keys;
    auto gen = autocheck::such_that(
        [](LedgerKey const& k) { return k.type() != CONFIG_SETTING; },
        autocheck::generator<LedgerKey>());

    while (keys.size() < 100)
    {
        keys.insert(gen());
    }

    auto seed = shortHash::getShortHashInitKey();
    std::vector<uint64_t> hashes;
    hashes.reserve(keys.size());
    for (auto const& k : keys)
    {
        auto keyBuf = xdr::xdr_to_opaque(k);
        SipHash24 hasher(seed.data());
        hasher.update(keyBuf.data(), keyBuf.size());
        hashes.emplace_back(hasher.digest());
    }

    // Construction should succeed despite forced error on first attempt
    BinaryFuseFilter8 filter(hashes, seed);

    // All keys must still be found despite the error
    for (auto const& k : keys)
    {
        REQUIRE(filter.contains(k));
    }
}

// During populate(), the internal second-level hashing (sip_hash24 on the
// pre-hashed keys) can produce collisions even when the input keys are unique.
// To simulate this, we just duplicate all the input hashes, which guarantees
// collisions.
TEST_CASE("binary fuse filter duplicate removal", "[BinaryFuseFilter]")
{
    COVMARK_CHECK_HIT_IN_CURR_SCOPE(BINARY_FUSE_DUPLICATE_REMOVAL);

    auto seed = shortHash::getShortHashInitKey();

    // Generate some unique keys
    LedgerKeySet keys;
    auto gen = autocheck::such_that(
        [](LedgerKey const& k) { return k.type() != CONFIG_SETTING; },
        autocheck::generator<LedgerKey>());

    while (keys.size() < 50)
    {
        keys.insert(gen());
    }

    std::vector<uint64_t> hashes;
    for (auto const& k : keys)
    {
        auto keyBuf = xdr::xdr_to_opaque(k);
        SipHash24 hasher(seed.data());
        hasher.update(keyBuf.data(), keyBuf.size());
        hashes.emplace_back(hasher.digest());
    }

    // Duplicate every hash to trigger the duplicate removal path
    auto originalSize = hashes.size();
    auto copy = hashes;
    hashes.insert(hashes.end(), copy.begin(), copy.end());
    REQUIRE(hashes.size() == originalSize * 2);

    BinaryFuseFilter8 filter(hashes, seed);

    for (auto const& k : keys)
    {
        REQUIRE(filter.contains(k));
    }
}

// After placing keys and computing t2count, populate() runs a "peeling" phase
// that iteratively removes positions with exactly one key, building the
// fingerprint assignment order. When the hash structure has too many cycles,
// peeling cannot remove all keys (stacksize + duplicates < size). populate()
// then resets the working arrays, rotates the seed, and retries with a
// different internal hash layout. This test forces a peeling failure on the
// first iteration and verifies the retry produces a correct filter.
TEST_CASE("binary fuse filter peeling failure retry", "[BinaryFuseFilter]")
{
    COVMARK_CHECK_HIT_IN_CURR_SCOPE(BINARY_FUSE_POPULATE_PEELING_FAILURE_RETRY);

    gBinaryFuseForcePeelingFailure = true;

    LedgerKeySet keys;
    auto gen = autocheck::such_that(
        [](LedgerKey const& k) { return k.type() != CONFIG_SETTING; },
        autocheck::generator<LedgerKey>());

    while (keys.size() < 100)
    {
        keys.insert(gen());
    }

    auto seed = shortHash::getShortHashInitKey();
    std::vector<uint64_t> hashes;
    hashes.reserve(keys.size());
    for (auto const& k : keys)
    {
        auto keyBuf = xdr::xdr_to_opaque(k);
        SipHash24 hasher(seed.data());
        hasher.update(keyBuf.data(), keyBuf.size());
        hashes.emplace_back(hasher.digest());
    }

    BinaryFuseFilter8 filter(hashes, seed);

    for (auto const& k : keys)
    {
        REQUIRE(filter.contains(k));
    }
}
