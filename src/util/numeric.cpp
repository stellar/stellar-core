// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/numeric.h"
#include <cassert>
#include <stdexcept>

namespace stellar
{
// calculates A*B/C when A*B overflows 64bits
bool
bigDivide(int64_t& result, int64_t A, int64_t B, int64_t C, Rounding rounding)
{
    bool res;
    assert((A >= 0) && (B >= 0) && (C > 0));
    uint64_t r2;
    res = bigDivide(r2, (uint64_t)A, (uint64_t)B, (uint64_t)C, rounding);
    if (res)
    {
        res = r2 <= INT64_MAX;
        result = r2;
    }
    return res;
}

bool
bigDivide(uint64_t& result, uint64_t A, uint64_t B, uint64_t C,
          Rounding rounding)
{
    // update when moving to (signed) int128
    uint128_t a(A);
    uint128_t b(B);
    uint128_t c(C);
    uint128_t x = rounding == ROUND_DOWN ? (a * b) / c : (a * b + c - 1) / c;

    result = (uint64_t)x;

    return (x <= UINT64_MAX);
}

int64_t
bigDivide(int64_t A, int64_t B, int64_t C, Rounding rounding)
{
    int64_t res;
    if (!bigDivide(res, A, B, C, rounding))
    {
        throw std::overflow_error("overflow while performing bigDivide");
    }
    return res;
}

bool
bigDivide(int64_t& result, uint128_t a, int64_t B, Rounding rounding)
{
    assert(B > 0);

    uint64_t r2;
    bool res = bigDivide(r2, a, (uint64_t)B, rounding);
    if (res)
    {
        res = r2 <= INT64_MAX;
        result = r2;
    }
    return res;
}

bool
bigDivide(uint64_t& result, uint128_t a, uint64_t B, Rounding rounding)
{
    assert(B != 0);

    // update when moving to (signed) int128
    uint128_t b(B);

    // We need to handle the case a + b - 1 > UINT128_MAX separately if rounding
    // up, since in this case a + b - 1 would overflow uint128_t. This is
    // equivalent to a > UINT128_MAX - (b - 1), where b >= 1 by assumption.
    // This is not a limitation of using uint128_t, since even if intermediate
    // values could not overflow we would still find that
    //     (a + b - 1) / b
    //         > UINT128_MAX / b
    //         >= UINT128_MAX / UINT64_MAX
    //         = ((UINT64_MAX + 1) * (UINT64_MAX + 1) - 1) / UINT64_MAX
    //         = (UINT64_MAX * UINT64_MAX + 2 * UINT64_MAX) / UINT64_MAX
    //         = UINT64_MAX + 2
    // which would have overflowed uint64_t anyway.
    uint128_t const UINT128_MAX = ~uint128_0;
    if ((rounding == ROUND_UP) && (a > UINT128_MAX - (b - 1)))
    {
        return false;
    }

    uint128_t x = rounding == ROUND_DOWN ? a / b : (a + b - 1) / b;

    result = (uint64_t)x;

    return (x <= UINT64_MAX);
}

int64_t
bigDivide(uint128_t a, int64_t B, Rounding rounding)
{
    int64_t res;
    if (!bigDivide(res, a, B, rounding))
    {
        throw std::overflow_error("overflow while performing bigDivide");
    }
    return res;
}

uint128_t
bigMultiply(uint64_t a, uint64_t b)
{
    uint128_t A(a);
    uint128_t B(b);
    return A * B;
}

uint128_t
bigMultiply(int64_t a, int64_t b)
{
    assert((a >= 0) && (b >= 0));
    return bigMultiply((uint64_t)a, (uint64_t)b);
}
}
