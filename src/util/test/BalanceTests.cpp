// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/catch.hpp"
#include "util/types.h"

using namespace stellar;

bool
addBalance(int64_t balance, int64_t delta, int64_t resultBalance,
           int64_t maxBalance = std::numeric_limits<int64_t>::max())
{
    auto r = stellar::addBalance(balance, delta, maxBalance);
    REQUIRE(balance == resultBalance);
    return r;
}

TEST_CASE("balance", "[balance]")
{
    auto const max = std::numeric_limits<int64_t>::max();
    auto const min = std::numeric_limits<int64_t>::min();

    REQUIRE(addBalance(0, 0, 0));
    REQUIRE(addBalance(0, 10, 10));
    REQUIRE(addBalance(10, 0, 10));
    REQUIRE(addBalance(10, 10, 20));
    REQUIRE(!addBalance(0, -5, 0));
    REQUIRE(addBalance(10, -10, 0));
    REQUIRE(addBalance(10, -9, 1));
    REQUIRE(!addBalance(10, -11, 10));
    REQUIRE(!addBalance(5, 5, 5, 9));

    REQUIRE(!addBalance(0, 1, 0, 0));
    REQUIRE(addBalance(0, 1, 1, max));
    REQUIRE(!addBalance(0, max, 0, 0));
    REQUIRE(addBalance(0, max, max, max));
    REQUIRE(!addBalance(max, 1, max, 0));
    REQUIRE(!addBalance(max, 1, max, max));
    REQUIRE(!addBalance(max, max, max, 0));
    REQUIRE(!addBalance(max, max, max, max));
    REQUIRE(!addBalance(0, -1, 0, 0));
    REQUIRE(!addBalance(0, -1, 0, max));
    REQUIRE(!addBalance(0, min, 0, 0));
    REQUIRE(!addBalance(0, -max, 0, 0));
    REQUIRE(!addBalance(0, min, 0, max));
    REQUIRE(!addBalance(0, -max, 0, max));
    REQUIRE(!addBalance(max, -1, max, 0));
    REQUIRE(addBalance(max, -1, max - 1, max));
    REQUIRE(!addBalance(max, min, max, 0));
    REQUIRE(addBalance(max, -max, 0, 0));
    REQUIRE(!addBalance(max, min, max, max));
    REQUIRE(addBalance(max, -max, 0, max));
}
