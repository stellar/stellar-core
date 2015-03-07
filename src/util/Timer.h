#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"
#include "util/NonCopyable.h"

#include <chrono>
#include <queue>
#include <map>
#include <memory>
#include <functional>
#include <ctime>

namespace stellar
{

/**
 * The purpose of this module is to provide "timing service" to stellard; but in
 * such a way that strongly favours the use of virtual time over real
 * time. Ideally there will only ever be one use of the "real" wall clock in the
 * system, which is driving the virtual clock when running non-test mode
 * (i.e. "actually running against real networks").
 *
 * Virtual time is nothing magical, it's just normal time durations (in
 * nanoseconds, via std::chrono, as usual) measured on a virtual clock, not the
 * system (wall) clock.
 *
 * The advantage of using virtual time over real time is that we can run virtual
 * time at whatever speed we like; and in particular two things become possible
 * in time-sensitive tests that would otherwise not be:
 *
 * - Precise control of simulated delay scenarios (one peer racing ahead of
 *   another etc.)
 *
 * - Tests running at the maximum speed the code can process events, not waiting
 *   for any time-based transitions to _literally_ occur due to the passage of
 *   wall-clock time, when there's no other work to do.
 *
 */

class VirtualTimer;
class Application;
struct VirtualClockEvent;

/**
 * There should be one virtual clock per application / main event loop, so that
 * the virtual clock can be advanced any time the event loop is cranked and
 * fails to do any work.
 */

class VirtualClock
{
public:
    typedef std::chrono::nanoseconds    duration;
    typedef duration::rep               rep;
    typedef duration::period            period;
    typedef std::chrono::time_point<std::chrono::steady_clock, duration>
                                        time_point;
    static const bool is_steady       = true;

    static std::time_t pointToTimeT(time_point);
    static std::tm pointToTm(time_point);
    static VirtualClock::time_point tmToPoint(tm t);
    static std::string tmToISOString(std::tm const& tm);
    static std::string pointToISOString(time_point point);

private:
    time_point mNow;
    std::map<Application*,
             std::shared_ptr<std::priority_queue<VirtualClockEvent>>> mEvents;
    std::map<Application*, bool> mIdleFlags;

public:
    // Note: this is not a static method, which means that VirtualClock is
    // not an implementation of the C++ `Clock` concept; there is no global
    // virtual time. Each virtual clock has its own time.
    time_point now() noexcept;
    time_point next();
    void enqueue(Application& app, VirtualClockEvent const& ve);
    bool cancelAllEventsFrom(Application& a,
                             std::function<bool(VirtualClockEvent const&)> pred);
    bool cancelAllEventsFrom(Application& a);
    bool cancelAllEventsFrom(Application& a, VirtualTimer& v);
    size_t advanceTo(time_point n);
    bool allIdle() const;
    bool allEmpty() const;
    void setNoneIdle();
    void setIdle(Application& app, bool isIdle);
    size_t advanceToNextIfAllIdle();
};


struct VirtualClockEvent
{
    VirtualClock::time_point mWhen;
    std::function<void(asio::error_code)> mCallback;
    VirtualTimer *mTimer;
    ~VirtualClockEvent();
    bool live() const;
    bool operator<(VirtualClockEvent const& other) const
    {
        // For purposes of priority queue, a timer is "less than"
        // another timer if it occurs in the future (has a higher
        // expiry time). The "greatest" timer is timer 0.
        return mWhen > other.mWhen;
    }
};


/**
 * This is the class you probably want to use: it is coupled with an Application
 * (thus the app's VirtualClock), so advances with per-Application simulated
 * time, and therefore runs at full speed during simulation/testing.
 */
class VirtualTimer : private NonMovableOrCopyable
{
    Application &mApp;
    VirtualClock::time_point mExpiryTime;
    bool mCancelled;
public:
    VirtualTimer(Application& app);
    ~VirtualTimer();

    void expires_at(VirtualClock::time_point t);
    void expires_from_now(VirtualClock::duration d);
    void async_wait(std::function<void(asio::error_code)> const& fn);
    void cancel();
};


// This is almost certainly not the type you want to use. So much so
// that we will not even show it to you unless you define an unwieldy
// symbol:
#ifdef STELLARD_REAL_TIMER_FOR_CERTAIN_NOT_JUST_VIRTUAL_TIME
class RealTimer : public asio::basic_waitable_timer<std::chrono::steady_clock>
{
public:
    RealTimer(asio::io_service &io)
        : asio::basic_waitable_timer<std::chrono::steady_clock>(io)
        {}
};
#endif

}


