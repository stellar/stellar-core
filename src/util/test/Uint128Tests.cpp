// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "autocheck/autocheck.hpp"
#include "lib/catch.hpp"
#include "lib/util/uint128_t.h"
#include "test/test.h"
#include "util/Logging.h"
#include <chrono>
#include <fmt/chrono.h>
#include <limits>
#include <ostream>

// This file just cross-checks a selection of operators in the uint128_t class
// against the values produced by native (compiler-provided) __int128 types,
// when applied to random values. It's to help convince us that the class
// is implemented correctly.

#if defined(__SIZEOF_INT128__) || defined(_GLIBCXX_USE_INT128)

const uint64_t full = std::numeric_limits<uint64_t>::max();

uint128_t
fromNative(unsigned __int128 x)
{
    return uint128_t(uint64_t(x >> 64), uint64_t(x & full));
}

unsigned __int128
toNative(uint128_t x)
{
    return ((unsigned __int128)x.upper()) << 64 | x.lower();
}

bool
toNative(bool x)
{
    return x;
}

struct gen128
{
    typedef unsigned __int128 result_type;
    result_type
    operator()(size_t size = 0)
    {
        std::uniform_int_distribution<uint64_t> dist(1, full);
        auto a = dist(autocheck::rng());
        auto b = dist(autocheck::rng());
        return (((result_type)a) << 64) | b;
    }
};

namespace std
{
std::ostream&
operator<<(std::ostream& out, unsigned __int128 const& x)
{
    return out << std::hex << "0x" << uint64_t(x >> 64) << uint64_t(x);
}
}

TEST_CASE("uint128_t bench", "[bench][uint128][!hide]")
{
    gen128 g;
    size_t const n = 1000000;
    uint128_t res1{0};
    __uint128_t res2{0};

    std::vector<__uint128_t> native;
    std::vector<uint128_t> lib;

    for (size_t i = 0; i < n; ++i)
    {
        __uint128_t n = g();
        n |= 1; // ensure nonzero
        native.emplace_back(n);
        lib.emplace_back(fromNative(n));
    }

    auto start = std::chrono::high_resolution_clock::now();
    for (auto k : lib)
    {
        res1 = res1 + (k * (k + k)) / k;
    }
    auto step1 = std::chrono::high_resolution_clock::now();
    for (auto k : native)
    {
        res2 = res2 + (k * (k + k)) / k;
    }
    auto step2 = std::chrono::high_resolution_clock::now();
    assert(res1 == fromNative(res2));
    LOG_INFO(DEFAULT_LOG, "library time: {} per iteration",
             (step1 - start) / n);
    LOG_INFO(DEFAULT_LOG, "native time: {} per iteration", (step2 - step1) / n);
}

TEST_CASE("uint128_t", "[uint128]")
{
    auto arb = autocheck::make_arbitrary(gen128(), gen128());
    autocheck::check<unsigned __int128, unsigned __int128>(
        [](unsigned __int128 x, unsigned __int128 y) {
            bool b = true;
#define BINOP(op) \
    (b = b && ((x op y) == toNative(fromNative(x) op fromNative(y))));
            BINOP(+);
            BINOP(-);
            BINOP(*);
            BINOP(/);
            BINOP(^);
            BINOP(&);
            BINOP(|);
            BINOP(%);
            BINOP(<);
            BINOP(<=);
            BINOP(==);
            BINOP(!=);
            BINOP(>=);
            BINOP(>);
            BINOP(||);
            BINOP(&&);
            return b;
        },
        100000, arb);
}

#endif

TEST_CASE("uint128_t carry tests with positive arg")
{
    SECTION("subtraction")
    {
        SECTION("carry lower")
        {
            uint128_t x(0u, 100u);
            x -= 1u;
            REQUIRE(x.lower() == 99);
            REQUIRE(x.upper() == 0);
        }
        SECTION("carry upper")
        {
            uint128_t x(2u, 0u);
            x -= 1u;
            REQUIRE(x.lower() == UINT64_MAX);
            REQUIRE(x.upper() == 1);
        }
    }
    SECTION("addition")
    {
        SECTION("carry lower")
        {
            uint128_t x(0u, 100u);
            x += 1u;
            REQUIRE(x.lower() == 101);
            REQUIRE(x.upper() == 0);
        }
        SECTION("carry upper")
        {
            uint128_t x(1u, UINT64_MAX);
            x += 1u;
            REQUIRE(x.lower() == 0);
            REQUIRE(x.upper() == 2);
        }
    }
}
