// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "util/Timer.h"
#include "autocheck/autocheck.hpp"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"

using namespace stellar;

TEST_CASE("VirtualClock::pointToISOString", "[timer]")
{
    VirtualClock clock;

    VirtualClock::time_point now = clock.now();
    CHECK(VirtualClock::pointToISOString(now) ==
          std::string("1970-01-01T00:00:00Z"));

    now += std::chrono::hours(36);
    CHECK(VirtualClock::pointToISOString(now) ==
          std::string("1970-01-02T12:00:00Z"));

    now += std::chrono::minutes(10);
    CHECK(VirtualClock::pointToISOString(now) ==
          std::string("1970-01-02T12:10:00Z"));

    now += std::chrono::seconds(3618);
    CHECK(VirtualClock::pointToISOString(now) ==
          std::string("1970-01-02T13:10:18Z"));
}
