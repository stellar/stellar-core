#pragma once

// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/util/uint128_t.h"
#include "util/numeric.h"

namespace stellar
{
bool bigDivide128(int64_t& result, uint128_t const& a, int64_t B,
                  Rounding rounding);
bool bigDivideUnsigned128(uint64_t& result, uint128_t const& a, uint64_t B,
                          Rounding rounding);
int64_t bigDivideOrThrow128(uint128_t const& a, int64_t B, Rounding rounding);

uint128_t bigMultiplyUnsigned(uint64_t a, uint64_t b);
uint128_t bigMultiply(int64_t a, int64_t b);

// Compute a * B / C when C < INT32_MAX * INT64_MAX.
bool hugeDivide(int64_t& result, int32_t a, uint128_t const& B,
                uint128_t const& C, Rounding rounding);
}
