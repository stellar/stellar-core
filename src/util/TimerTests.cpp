// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "util/Timer.h"
#include "autocheck/autocheck.hpp"
#include "main/Application.h"
#include "main/Config.h"
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

TEST_CASE("virtual event dispatch order and times", "[timer]")
{
    Config cfg(getTestConfig());
    VirtualClock clock;
    Application app(clock, cfg);

    VirtualTimer timer1(app.getClock());
    VirtualTimer timer20(app.getClock());
    VirtualTimer timer21(app.getClock());
    VirtualTimer timer200(app.getClock());

    size_t eventsDispatched = 0;

    timer1.expires_from_now(std::chrono::nanoseconds(1));
    timer1.async_wait(
        [&](asio::error_code const& e)
        {
            CHECK(clock.now().time_since_epoch().count() == 1);
            CHECK(eventsDispatched++ == 0);
        });

    timer20.expires_from_now(std::chrono::nanoseconds(20));
    timer20.async_wait(
        [&](asio::error_code const& e)
        {
            CHECK(clock.now().time_since_epoch().count() == 20);
            CHECK(eventsDispatched++ == 1);
        });

    timer21.expires_from_now(std::chrono::nanoseconds(21));
    timer21.async_wait(
        [&](asio::error_code const& e)
        {
            CHECK(clock.now().time_since_epoch().count() == 21);
            CHECK(eventsDispatched++ == 2);
        });

    timer200.expires_from_now(std::chrono::nanoseconds(200));
    timer200.async_wait(
        [&](asio::error_code const& e)
        {
            CHECK(clock.now().time_since_epoch().count() == 200);
            CHECK(eventsDispatched++ == 3);
        });

    while(app.crank(false) > 0);
    CHECK(eventsDispatched == 4);
}
