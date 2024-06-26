// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/numeric.h"
#include "util/GlobalChecks.h"
#include "util/numeric128.h"

#include <algorithm>
#include <stdexcept>

namespace stellar
{
// calculates A*B/C when A*B overflows 64bits
bool
bigDivide(int64_t& result, int64_t A, int64_t B, int64_t C, Rounding rounding)
{
    bool res;
    releaseAssertOrThrow((A >= 0) && (B >= 0) && (C > 0));
    uint64_t r2;
    res =
        bigDivideUnsigned(r2, (uint64_t)A, (uint64_t)B, (uint64_t)C, rounding);
    if (res)
    {
        res = r2 <= INT64_MAX;
        result = r2;
    }
    return res;
}

bool
bigDivideUnsigned(uint64_t& result, uint64_t A, uint64_t B, uint64_t C,
                  Rounding rounding)
{
    releaseAssertOrThrow(C > 0);

    // update when moving to (signed) int128
    uint128_t a(A);
    uint128_t b(B);
    uint128_t c(C);
    uint128_t x = rounding == ROUND_DOWN ? (a * b) / c : (a * b + c - 1u) / c;

    result = (uint64_t)x;
    return (x <= UINT64_MAX);
}

int64_t
bigDivideOrThrow(int64_t A, int64_t B, int64_t C, Rounding rounding)
{
    int64_t res;
    if (!bigDivide(res, A, B, C, rounding))
    {
        throw std::overflow_error("overflow while performing bigDivide");
    }
    return res;
}

bool
bigDivide128(int64_t& result, uint128_t const& a, int64_t B, Rounding rounding)
{
    releaseAssertOrThrow(B > 0);

    uint64_t r2;
    bool res = bigDivideUnsigned128(r2, a, (uint64_t)B, rounding);
    if (res)
    {
        res = r2 <= INT64_MAX;
        result = r2;
    }
    return res;
}

bool
bigDivideUnsigned128(uint64_t& result, uint128_t const& a, uint64_t B,
                     Rounding rounding)
{
    releaseAssertOrThrow(B != 0);

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
    uint128_t const UINT128_MAX = uint128_max();
    if ((rounding == ROUND_UP) && (a > UINT128_MAX - (b - 1u)))
    {
        return false;
    }

    uint128_t x = rounding == ROUND_DOWN ? a / b : (a + b - 1u) / b;

    result = (uint64_t)x;

    return (x <= UINT64_MAX);
}

int64_t
bigDivideOrThrow128(uint128_t const& a, int64_t B, Rounding rounding)
{
    int64_t res;
    if (!bigDivide128(res, a, B, rounding))
    {
        throw std::overflow_error("overflow while performing bigDivide");
    }
    return res;
}

uint128_t
bigMultiplyUnsigned(uint64_t a, uint64_t b)
{
    uint128_t A(a);
    uint128_t B(b);
    return A * B;
}

uint128_t
bigMultiply(int64_t a, int64_t b)
{
    releaseAssertOrThrow((a >= 0) && (b >= 0));
    return bigMultiplyUnsigned((uint64_t)a, (uint64_t)b);
}

// Fix R >= 0. Consider the modified Babylonian method that operates only on
// integers
//     x[n+1] = ceil((x[n] + ceil(R / x[n])) / 2) .
// We claim that the limit of this sequence is ceil(sqrt(R+1)) if
//     x[0] >= ceil(sqrt(R+1)) .
//
// As a first step, we show that the sequence is monotonically decreasing for
// x[n] > ceil(sqrt(R+1)). Suppose, to reach a contradiction, that the
// sequence would not decrease for some x[n] = x > ceil(sqrt(R+1)). Then we
// must have
//     ceil((x + ceil(R / x)) / 2) >= x
// which occurs if
//     x + ceil(R / x) > 2x - 2 .
// Rearranging, the condition becomes
//     ceil(R / x) - x > -2 .
// The left-hand side is decreasing with x, so if the condition cannot be
// satisfied with x = ceil(sqrt(R+1)) + 1 then it cannot be satisfied with any
// permissible x. Then
//     ceil(R / (ceil(sqrt(R+1)) + 1)) - ceil(sqrt(R+1)) - 1
//         <= ceil(R / (sqrt(R+1) + 1)) - ceil(sqrt(R+1)) - 1
//          = ceil((sqrt(R+1)^2 - 1) / (sqrt(R+1) + 1)) - ceil(sqrt(R+1)) - 1
//          = ceil(sqrt(R+1) - 1) - ceil(sqrt(R+1)) - 1
//          = -2
// so the condition is not satisfied. Therefore we have reached a contradiction
// so the sequence is monotonically decreasing for x[n] > ceil(sqrt(R+1)).
//
// As a second step, we show that the sequence is bounded below. Suppose that
// x[n] = x >= ceil(sqrt(R+1)). Then we have
//     x + ceil(R / x) >= x + R / x .
// But x >= ceil(sqrt(R+1)) > sqrt(R) so x + R / x is increasing with x. It
// follows that x + R / x is minimized by choosing the smallest permissible x.
// Applying this logic
//     x + ceil(R / x) >= x + R / x
//                      = ceil(sqrt(R+1)) + R / ceil(sqrt(R+1)) + 1
//                     >= sqrt(R+1) + R / (sqrt(R+1) + 1) + 1
//                      = sqrt(R+1) + (sqrt(R+1)^2 - 1) / (sqrt(R+1) + 1) + 1
//                      = sqrt(R+1) + (sqrt(R+1) - 1) = 1
//                      = 2 * sqrt(R+1) .
// It follows that
//     x[n+1]  = ceil((x + ceil(R / X)) / 2)
//            >= ceil((2 * sqrt(R+1)) / 2)
//             = ceil(sqrt(R+1)) .
// Therefore x[n+1] >= ceil(sqrt(R+1)) if x[n] >= ceil(sqrt(R+1)) so the
// sequence is bounded below.
//
// Using the monotone convergence theorem, the limit exists and is the infimum.
// All that remains to be shown is that the infimum is in fact ceil(sqrt(R+1)).
// But this is easy given the results we have already proven. Suppose, to reach
// a contradiction, that the infimum is L > ceil(sqrt(R+1)). A set of integers
// always achieves its infimum, so there exists some n such that x[n] = L. But
// the sequence is monotonically decreasing, so we have
//     x[n+1] <  x[n]
//            <= L - 1
// contradicting the claim that L is the infimum. We conclude that x[n]
// converges to ceil(sqrt(R+1)).
//
// Next we show that convergence is fast. Define the absolute error
//     E[n] = x[n] - ceil(sqrt(R+1)) .
// If x[n] = x >= ceil(sqrt(R+1)) and E[n] = E then
//     x[n+1]  = ceil((x + ceil(R / x)) / 2)
//            <= ceil((x + ceil(sqrt(R+1))) / 2)
//             = ceil((x + (x - x) + ceil(sqrt(R+1))) / 2)
//             = ceil((2x + ceil(sqrt(R+1)) - x) / 2)
//             = ceil((2x - E) / 2)
//             = x - floor(E / 2) .
// It is clear that
//     E[n+1] - E[n]  = x[n+1] - x[n]
//                   <= -floor(E[n] / 2)
//                   <= -E[n] / 2 + 1
// so
//     E[n+1] <= E[n] / 2 + 1 .
// Iterating this sequence, we find
//     E[n+k] <= E[n+k-1] / 2 + 1
//            <= ...
//            <= E[n] / 2^k + (1 + ... + 1/2^k)
//            <  E[n] / 2^k + 2 .
// We conclude that the order of convergence is at least linear, although
// numerical evidence suggests it is of higher order. The normal Babylonian
// method has quadratic convergence, so this is perhaps a reasonable
// conjecture.
//
// Now that we have an algorithm to compute ceil(sqrt(R+1)) then we can simply
// use R-1 instead of R in the definition of the sequence to compute
// ceil(sqrt(R)). This requires handling R=0 as a special case.
static uint64_t
bigSquareRootCeil(uint64_t a, uint64_t b)
{
    // a * b = 0 is a special-case because we can't compute a * b - 1
    if (a == 0 || b == 0)
    {
        return 0;
    }
    uint128_t R = bigMultiplyUnsigned(a, b) - 1u;

    // Seed the result with a reasonable estimate x >= ceil(sqrt(R+1))
    int numBits = uint128_bits(R) / 2 + 1;
    uint64_t x = numBits >= 64 ? UINT64_MAX : (1ull << numBits);

    uint64_t prev = 0;
    while (x != prev)
    {
        prev = x;

        uint64_t y = 0;
        bool res = bigDivideUnsigned128(y, R, x, ROUND_UP);
        if (!res)
        {
            throw std::runtime_error("Overflow during bigSquareRoot");
        }

        if (UINT64_MAX - x <= y)
        {
            uint128_t temp(1u);
            temp += x;
            temp += y;
            x = (uint64_t)(temp / 2u);
        }
        else // UINT64_MAX >= x + y + 1
        {
            x = (x + y + 1) / 2;
        }
    }

    return x;
}

// Find x such that x * x <= a * b < (x+1) * (x+1).
uint64_t
bigSquareRoot(uint64_t a, uint64_t b)
{
    uint64_t sqrtCeil = bigSquareRootCeil(a, b);

    // sqrtCeil * sqrtCeil >= a * b so
    //     sqrtCeil * sqrtCeil <= a * b
    // implies sqrtCeil * sqrtCeil = a * b.
    if (bigMultiplyUnsigned(sqrtCeil, sqrtCeil) <= bigMultiplyUnsigned(a, b))
    {
        return sqrtCeil;
    }

    // sqrtCeil > 0 because
    //     0 * 0 <= a * b
    // for all a, b.
    //
    // sqrtCeil * sqrtCeil > a * b implies that
    //     (sqrtCeil - 1) * (sqrtCeil - 1) = a * b - 2 * sqrtCeil + 1
    //                                     < a * b
    // because
    //     1 - 2 * sqrtCeil < 0 .
    return sqrtCeil - 1;
}

bool
hugeDivide(int64_t& result, int32_t a, uint128_t const& B, uint128_t const& C,
           Rounding rounding)
{
    uint128_t constexpr i32_max((uint32_t)INT32_MAX);
    uint128_t constexpr i64_max((uint64_t)INT64_MAX);

    releaseAssertOrThrow(a >= 0);
    releaseAssertOrThrow(C != 0ul);
    releaseAssertOrThrow(C <= i32_max * i64_max);

    // Use the remainder theorem to yield B = QC + R with R < C
    uint128_t Q(B / C);
    uint128_t R(B % C);

    // Result never fits if Q is too big
    if (Q > i64_max)
    {
        return false;
    }

    // We can evaluate A * Q because
    //     A * Q <= INT64_MAX * INT32_MAX
    //           <  INT128_MAX .
    // We can evaluate A * R because
    //     A * R < A * C
    //           < INT32_MAX * (INT32_MAX * INT64_MAX)
    //           < INT128_MAX
    // and A * R + C - 1 because
    //     A * R + C < (INT32_MAX + 1) * (INT32_MAX * INT64_MAX)
    //               < INT128_MAX .
    // Combining these results, we can evaluate
    //     A * Q + A * R / C < 2 * INT128_MAX < UINT128_MAX
    // and
    //     A * Q + (A * R + C - 1) / C < 2 * INT128_MAX < UINT128_MAX .
    uint128_t A((uint32_t)a);
    uint128_t res = (rounding == ROUND_DOWN) ? A * Q + A * R / C
                                             : A * Q + (A * R + C - 1u) / C;

    if (res <= i64_max)
    {
        // There is no conversion uint128_t to int64_t so have to go through an
        // intermediate type.
        result = (int64_t)(uint64_t)res;
        return true;
    }
    return false;
}

uint32_t
doubleToClampedUint32(double d)
{
    // IEEE-754 doubles have 52-bit fractions, and so can perfectly represent
    // uint32_t values. Therefore, it should be safe to convert to the full
    // range of uint32_t values.  However, the C++ standard does not mandate
    // that doubles adhere to the IEEE-754 specification. That being said, it
    // would be shocking if we built for any platform that wasn't using IEEE-754
    // doubles. Regardless, it's an easy compile-time check to be sure.
    static_assert(std::numeric_limits<double>::is_iec559,
                  "double is not IEEE-754 compliant");

    constexpr uint32_t maxUint32 = std::numeric_limits<uint32_t>::max();
    if (std::isnan(d))
    {
        return maxUint32;
    }
    return static_cast<uint32_t>(std::clamp<double>(d, 0, maxUint32));
}
}
