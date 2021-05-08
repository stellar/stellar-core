#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/util/uint128_t.h"
#include <cstdint>
#include <limits>

namespace stellar
{
enum Rounding
{
    ROUND_DOWN,
    ROUND_UP
};

inline bool
isRepresentableAsInt64(double d)
{
    // Subtle: the min value here is a power-of-two and is exactly representable
    // as a double, so any double equal-or-greater can be rounded-up to an
    // int64_t without triggering UB.
    //
    // The max value here is one-less-than a power-of-two, but converting it to
    // a double will round _up_ to the next power-of-two (as doubles are spaced
    // 1024 integers apart out there) and so the only doubles that can be safely
    // represented as int64_t are those strictly _less_ than the resulting
    // double.
    //
    // Concretely: the max int64_t is 0x7fff_ffff_ffff_ffff which rounds up to
    // the double representing the integer 0x8000_0000_0000_0000, which is now
    // strictly too big to fix back in int64_t; the next-lower double is the one
    // representing the integer 1024 integers lower, at 0x7fff_ffff_ffff_fc00.
    //
    // The max int64_t that will round-down to that double, rather than up to
    // the too-large one, is 0x7fff_ffff_ffff_fdff or 9,223,372,036,854,775,295.
    return (d >= static_cast<double>(std::numeric_limits<int64_t>::min())) &&
           (d < static_cast<double>(std::numeric_limits<int64_t>::max()));
}

// calculates A*B/C when A*B overflows 64bits
int64_t bigDivide(int64_t A, int64_t B, int64_t C, Rounding rounding);
// no throw version, returns true if result is valid
bool bigDivide(int64_t& result, int64_t A, int64_t B, int64_t C,
               Rounding rounding);

// no throw version, returns true if result is valid
bool bigDivide(uint64_t& result, uint64_t A, uint64_t B, uint64_t C,
               Rounding rounding);

bool bigDivide(int64_t& result, uint128_t a, int64_t B, Rounding rounding);
bool bigDivide(uint64_t& result, uint128_t a, uint64_t B, Rounding rounding);
int64_t bigDivide(uint128_t a, int64_t B, Rounding rounding);

uint128_t bigMultiply(uint64_t a, uint64_t b);
uint128_t bigMultiply(int64_t a, int64_t b);
}
