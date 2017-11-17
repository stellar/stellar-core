// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/catch.hpp"
#include "lib/util/uint128_t.h"
#include "util/types.h"
#include <functional>

using namespace stellar;

template <typename T> using Verify = std::function<void(T, T, T, T, T)>;

template <typename T> class BigDivideTester
{
  public:
    using P = std::function<bool(uint128_t, uint128_t, uint128_t)>;
    using R = std::function<uint128_t(uint128_t, uint128_t, uint128_t)>;

    explicit BigDivideTester(std::vector<T> values, Verify<T> const& verify)
        : mValues{std::move(values)}, mVerify{verify}
    {
    }

    void
    test(P const& p, R const& downResult, R const& upResult)
    {
        for (auto a : mValues)
            for (auto b : mValues)
                for (auto c : mValues)
                    if (c != 0 && p(uint128_t{a}, uint128_t{b}, uint128_t{c}))
                        mVerify(a, b, c,
                                static_cast<uint64_t>(downResult(
                                    uint128_t{a}, uint128_t{b}, uint128_t{c})),
                                static_cast<uint64_t>(upResult(
                                    uint128_t{a}, uint128_t{b}, uint128_t{c})));
    }

  private:
    std::vector<T> mValues;
    Verify<T> mVerify;
};

TEST_CASE("big divide", "[bigDivide]")
{
    auto verifySignedUp = [](int64_t a, int64_t b, int64_t c,
                             int64_t expectedResult) {
        REQUIRE(bigDivide(a, b, c, ROUND_UP) == expectedResult);
        auto signedResult = int64_t{};
        REQUIRE(bigDivide(signedResult, a, b, c, ROUND_UP));
        REQUIRE(signedResult == expectedResult);
    };

    auto verifySignedDown = [](int64_t a, int64_t b, int64_t c,
                               int64_t expectedResult) {
        REQUIRE(bigDivide(a, b, c, ROUND_DOWN) == expectedResult);
        auto signedResult = int64_t{};
        REQUIRE(bigDivide(signedResult, a, b, c, ROUND_DOWN));
        REQUIRE(signedResult == expectedResult);
    };

    auto verifySigned = [&](int64_t a, int64_t b, int64_t c,
                            int64_t expectedRoundedDownResult,
                            int64_t expectedRoundedUpResult) {
        verifySignedUp(a, b, c, expectedRoundedUpResult);
        verifySignedDown(a, b, c, expectedRoundedDownResult);
    };

    auto verifySignedUpOverflow = [](int64_t a, int64_t b, int64_t c) {
        REQUIRE_THROWS_AS(bigDivide(a, b, c, ROUND_UP), std::overflow_error);
        auto x = int64_t{};
        REQUIRE(!bigDivide(x, a, b, c, ROUND_UP));
    };

    auto verifySignedDownOverflow = [](int64_t a, int64_t b, int64_t c) {
        REQUIRE_THROWS_AS(bigDivide(a, b, c, ROUND_DOWN), std::overflow_error);
        auto x = int64_t{};
        REQUIRE(!bigDivide(x, a, b, c, ROUND_DOWN));
    };

    auto verifySignedOverflow = [&](int64_t a, int64_t b, int64_t c) {
        verifySignedUpOverflow(a, b, c);
        verifySignedDownOverflow(a, b, c);
    };

    auto verifyUnsignedUp = [](uint64_t a, uint64_t b, uint64_t c,
                               uint64_t expectedResult) {
        auto unsignedResult = uint64_t{};
        REQUIRE(bigDivide(unsignedResult, a, b, c, ROUND_UP));
        REQUIRE(unsignedResult == expectedResult);
    };

    auto verifyUnsignedDown = [](uint64_t a, uint64_t b, uint64_t c,
                                 uint64_t expectedResult) {
        auto unsignedResult = uint64_t{};
        REQUIRE(bigDivide(unsignedResult, a, b, c, ROUND_DOWN));
        REQUIRE(unsignedResult == expectedResult);
    };

    auto verifyUnsigned = [&](uint64_t a, uint64_t b, uint64_t c,
                              uint64_t expectedRoundedDownResult,
                              uint64_t expectedRoundedUpResult) {
        verifyUnsignedUp(a, b, c, expectedRoundedUpResult);
        verifyUnsignedDown(a, b, c, expectedRoundedDownResult);
    };

    auto verifyUnsignedUpOverflow = [](uint64_t a, uint64_t b, uint64_t c) {
        auto x = uint64_t{};
        REQUIRE(!bigDivide(x, a, b, c, ROUND_UP));
    };

    auto verifyUnsignedDownOverflow = [](uint64_t a, uint64_t b, uint64_t c) {
        auto x = uint64_t{};
        REQUIRE(!bigDivide(x, a, b, c, ROUND_DOWN));
    };

    auto verifyUnsignedOverflow = [&](uint64_t a, uint64_t b, uint64_t c) {
        verifyUnsignedUpOverflow(a, b, c);
        verifyUnsignedDownOverflow(a, b, c);
    };

    auto signedValues = std::vector<int64_t>{
        0, 1, 2, 3, 1000, INT32_MAX - 1, INT32_MAX, INT64_MAX - 1, INT64_MAX};
    auto unsignedValues = std::vector<uint64_t>{0,          1,
                                                2,          3,
                                                1000,       INT32_MAX - 1,
                                                INT32_MAX,  UINT32_MAX - 1,
                                                UINT32_MAX, INT64_MAX - 1,
                                                INT64_MAX,  UINT64_MAX - 1,
                                                UINT64_MAX};

    auto signedTester = BigDivideTester<int64_t>{signedValues, verifySigned};
    auto unsignedTester =
        BigDivideTester<uint64_t>{unsignedValues, verifyUnsigned};

#undef WHEN
#define H(T, x) [](uint128_t a, uint128_t b, uint128_t c) -> T { return (x); }
#define WHEN(x) H(bool, (x)),
#define DOWN_IS(x) H(uint128_t, uint128_t{x}),
#define UP_IS(x) H(uint128_t, uint128_t{x})

#define TEST(when, down, up) \
    SECTION("a * b/c WHEN " #when) \
    { \
        signedTester.test(WHEN(when) DOWN_IS(down) UP_IS(up)); \
        unsignedTester.test(WHEN(when) DOWN_IS(down) UP_IS(up)); \
    }

    TEST(a == 0, 0, 0)
    TEST(b == 0, 0, 0)
    TEST(a <= INT32_MAX && b <= INT32_MAX && c == 1, a * b, a * b)
    TEST(b == 1 && c == 2, a / 2, (a + 1) / 2)
    TEST(b == 1 && c == 3, a / 3, (a + 2) / 3)
    TEST(a == 1 && b > 0 && b < c, 0, 1)
    TEST(a == 1 && b == c, 1, 1)
    TEST(a == c && b > 0, b, b);

    SECTION("upper limits")
    {
        verifySigned(INT64_MAX, INT64_MAX, INT64_MAX, INT64_MAX, INT64_MAX);
        verifyUnsigned(UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX,
                       UINT64_MAX);
    }

    SECTION("overflows")
    {
        verifySignedOverflow(INT64_MAX, INT64_MAX, 1);
        verifyUnsignedOverflow(UINT64_MAX, UINT64_MAX, 1);
    }

    SECTION("rounding up overflows")
    {
        verifySignedUpOverflow(2753074036095, 6700417, 2);
        verifySignedDown(2753074036095, 6700417, 2, INT64_MAX);
        verifyUnsignedUpOverflow(145295143558111, 253921, 2);
        verifyUnsignedDown(145295143558111, 253921, 2, UINT64_MAX);
    }
}
