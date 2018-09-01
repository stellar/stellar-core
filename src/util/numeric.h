#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/util/uint128_t.h"
#include <cstdint>

namespace stellar
{
enum Rounding
{
    ROUND_DOWN,
    ROUND_UP
};

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
