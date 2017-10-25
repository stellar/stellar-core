// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Timer.h"

#include "autocheck/autocheck.hpp"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Logging.h"
#include "util/make_unique.h"
#include <chrono>

using namespace stellar;

TEST_CASE("pointToTm tmToPoint stuff", "[timer]")
{
    VirtualClock::time_point tp;
    tp = tp + std::chrono::seconds(12); // 01/01/70 00:00:12 UTC+8 is before GMT
                                        // epoch, mktime may fail.

    std::tm tt = VirtualClock::pointToTm(tp);

    VirtualClock::time_point tp2 = VirtualClock::tmToPoint(tt);

    auto twelvesec = VirtualClock::to_time_t(tp2);
    CHECK(twelvesec == 12);
}

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

TEST_CASE("VirtualClock::to_time_t", "[timer]")
{
    VirtualClock clock;

    VirtualClock::time_point now = clock.now();
    CHECK(VirtualClock::to_time_t(now) == 0);

    now += std::chrono::hours(36);
    CHECK(VirtualClock::to_time_t(now) == 129600);

    now += std::chrono::minutes(10);
    CHECK(VirtualClock::to_time_t(now) == 130200);

    now += std::chrono::seconds(3618);
    CHECK(VirtualClock::to_time_t(now) == 133818);
}

TEST_CASE("VirtualClock::from_time_t", "[timer]")
{
    VirtualClock clock;

    VirtualClock::time_point now = clock.now();
    CHECK(now == VirtualClock::from_time_t(0));

    now += std::chrono::hours(36);
    CHECK(now == VirtualClock::from_time_t(129600));

    now += std::chrono::minutes(10);
    CHECK(now == VirtualClock::from_time_t(130200));

    now += std::chrono::seconds(3618);
    CHECK(now == VirtualClock::from_time_t(133818));
}

TEST_CASE("virtual event dispatch order and times", "[timer]")
{
    Config cfg(getTestConfig());
    VirtualClock clock;
    Application::pointer appPtr = createTestApplication(clock, cfg);
    Application& app = *appPtr;

    VirtualTimer timer1(app);
    VirtualTimer timer20(app);
    VirtualTimer timer21(app);
    VirtualTimer timer200(app);

    size_t eventsDispatched = 0;

    timer1.expires_from_now(std::chrono::milliseconds(1));
    timer1.async_wait([&](asio::error_code const& e) {
        auto ns = std::chrono::duration_cast<std::chrono::milliseconds>(
                      clock.now().time_since_epoch())
                      .count();
        CHECK(ns == 1);
        CHECK(eventsDispatched++ == 0);
    });

    timer20.expires_from_now(std::chrono::milliseconds(20));
    timer20.async_wait([&](asio::error_code const& e) {
        auto ns = std::chrono::duration_cast<std::chrono::milliseconds>(
                      clock.now().time_since_epoch())
                      .count();
        CHECK(ns == 20);
        CHECK(eventsDispatched++ == 1);
    });

    timer21.expires_from_now(std::chrono::milliseconds(21));
    timer21.async_wait([&](asio::error_code const& e) {
        auto ns = std::chrono::duration_cast<std::chrono::milliseconds>(
                      clock.now().time_since_epoch())
                      .count();
        CHECK(ns == 21);
        CHECK(eventsDispatched++ == 2);
    });

    timer200.expires_from_now(std::chrono::milliseconds(200));
    timer200.async_wait([&](asio::error_code const& e) {
        auto ns = std::chrono::duration_cast<std::chrono::milliseconds>(
                      clock.now().time_since_epoch())
                      .count();
        CHECK(ns == 200);
        CHECK(eventsDispatched++ == 3);
    });

    while (clock.crank(false) > 0)
        ;
    CHECK(eventsDispatched == 4);
}

TEST_CASE("shared virtual time advances only when all apps idle",
          "[timer][sharedtimer]")
{
    VirtualClock clock;
    Application::pointer app1 = createTestApplication(clock, getTestConfig(0));
    Application::pointer app2 = createTestApplication(clock, getTestConfig(1));

    size_t app1Event = 0;
    size_t app2Event = 0;
    size_t timerFired = 0;

    auto& io = clock.getIOService();

    // Fire one event on the app's queue
    io.post([&]() { ++app1Event; });
    clock.crank(false);
    CHECK(app1Event == 1);
    CHECK(app2Event == 0);
    CHECK(timerFired == 0);

    // Fire one timer
    VirtualTimer timer(*app1);
    timer.expires_from_now(std::chrono::seconds(1));
    timer.async_wait([&](asio::error_code const& e) { ++timerFired; });
    clock.crank(false);
    CHECK(app1Event == 1);
    CHECK(app2Event == 0);
    CHECK(timerFired == 1);

    // Queue 2 new events and 1 new timer
    io.post([&]() { ++app1Event; });
    io.post([&]() { ++app2Event; });
    timer.expires_from_now(std::chrono::seconds(1));
    timer.async_wait([&](asio::error_code const& e) { ++timerFired; });

    // Check that cranking the clock advances both events and does not fire the
    // timer.
    clock.crank(false);
    CHECK(app1Event == 2);
    CHECK(app2Event == 1);
    CHECK(timerFired == 1);

    // Check that one last crank fires the timer.
    clock.crank(false);
    CHECK(app1Event == 2);
    CHECK(app2Event == 1);
    CHECK(timerFired == 2);
}

TEST_CASE("timer cancels", "[timer]")
{
    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, getTestConfig(0));

    int timerFired = 0;
    int timerCancelled = 0;
    std::vector<std::unique_ptr<VirtualTimer>> timers;
    for (int i = 0; i < 10; i++)
    {
        timers.push_back(make_unique<VirtualTimer>(*app));
        timers.back()->expires_from_now(std::chrono::seconds(i));
        timers.back()->async_wait(
            [&timerFired, &timerCancelled, i](asio::error_code const& ec) {
                if (ec)
                    ++timerCancelled;
                else
                    ++timerFired;
            });
    }
    timers[5]->async_wait([&](asio::error_code const& ec) {
        if (!ec)
        {
            timers[4]->cancel();
            timers[5]->cancel();
            timers[6]->cancel();
            timers[7]->cancel();
        }
    });
    while (clock.crank(false) > 0)
        ;
    REQUIRE(timerFired == 8);
    REQUIRE(timerCancelled == 2);
}
