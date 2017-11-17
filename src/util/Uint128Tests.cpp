// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "autocheck/autocheck.hpp"
#include "lib/catch.hpp"
#include "lib/util/uint128_t.h"
#include "test/test.h"
#include "util/Logging.h"
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
    return uint128_t(x >> 64, x & full);
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
            BINOP (^);
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
