#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"
#include "util/NonCopyable.h"

#include <chrono>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>

namespace stellar
{

/**
 * The purpose of this module is to provide "timing service" to stellar-core;
 *but in
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
class VirtualClockEvent;
class VirtualClockEventCompare
{
  public:
    bool operator()(std::shared_ptr<VirtualClockEvent> a,
                    std::shared_ptr<VirtualClockEvent> b);
};

class VirtualClock
{
  public:
    // We don't want to deal with systems that have not moved to 64bit time_t
    // yet.
    static_assert(sizeof(uint64_t) == sizeof(std::time_t),
                  "Require 64bit time_t");

    // These model most of the std::chrono clock concept, with the exception of
    // now()
    // which is non-static.
    typedef std::chrono::system_clock::duration duration;
    typedef duration::rep rep;
    typedef duration::period period;
    typedef std::chrono::system_clock::time_point time_point;
    static const bool is_steady = false;

    /**
     * NB: Please please please use these helpers for date-time conversions
     * against the application's view of time (VirtualClock::time_point); and if
     * you need to do manual arithmetic on time, use the std::chrono types
     * (time_point and duration) as far as possible, only converting to a
     * unit-less number when absolutely required.
     *
     * In particular, prefer std::duration_cast<>() over any manual
     * multiplication or division of units. It will convert
     * VirtualClock::time_point and ::duration into the units you're interested
     * in.
     */

    // These two are named to mimic the std::chrono::system_clock methods
    static std::time_t to_time_t(time_point);
    static time_point from_time_t(std::time_t);

    static std::tm pointToTm(time_point);
    static VirtualClock::time_point tmToPoint(tm t);

    static std::tm isoStringToTm(std::string const& iso);
    static std::string tmToISOString(std::tm const& tm);
    static std::string pointToISOString(time_point point);

    enum Mode
    {
        REAL_TIME,
        VIRTUAL_TIME
    };

  private:
    asio::io_service mIOService;
    Mode mMode;

    uint32_t mRecentCrankCount;
    uint32_t mRecentIdleCrankCount;

    size_t nRealTimerCancelEvents;
    time_point mNow;

    bool mDelayExecution{true};
    std::recursive_mutex mDelayExecutionMutex;
    std::vector<std::function<void()>> mDelayedExecutionQueue;

    using PrQueue =
        std::priority_queue<std::shared_ptr<VirtualClockEvent>,
                            std::vector<std::shared_ptr<VirtualClockEvent>>,
                            VirtualClockEventCompare>;
    PrQueue mEvents;
    size_t mFlushesIgnored = 0;

    bool mDestructing{false};

    void maybeSetRealtimer();
    size_t advanceTo(time_point n);
    size_t advanceToNext();
    size_t advanceToNow();

    // timer should be last to ensure it gets destroyed first
    asio::basic_waitable_timer<std::chrono::system_clock> mRealTimer;

  public:
    // A VirtualClock is instantiated in either real or virtual mode. In real
    // mode, crank() sleeps until the next event, either timer or IO; in virtual
    // mode it processes IO events until IO is idle then advances to the time of
    // the next virtual event instantly.

    VirtualClock(Mode mode = VIRTUAL_TIME);
    ~VirtualClock();
    size_t crank(bool block = true);
    void noteCrankOccurred(bool hadIdle);
    uint32_t recentIdleCrankPercent() const;
    void resetIdleCrankPercent();
    asio::io_service& getIOService();

    // Note: this is not a static method, which means that VirtualClock is
    // not an implementation of the C++ `Clock` concept; there is no global
    // virtual time. Each virtual clock has its own time.
    time_point now() noexcept;

    void enqueue(std::shared_ptr<VirtualClockEvent> ve);
    void flushCancelledEvents();
    bool cancelAllEvents();

    // only valid with VIRTUAL_TIME: sets the current value
    // of the clock
    void setCurrentTime(time_point t);

    // returns the time of the next scheduled event
    time_point next();

    void postToCurrentCrank(std::function<void()>&& f);
    void postToNextCrank(std::function<void()>&& f);
};

class VirtualClockEvent : public NonMovableOrCopyable
{
    std::function<void(asio::error_code)> mCallback;
    bool mTriggered;

  public:
    VirtualClock::time_point mWhen;
    size_t mSeq;
    VirtualClockEvent(VirtualClock::time_point when, size_t seq,
                      std::function<void(asio::error_code)> callback);
    bool getTriggered();
    void trigger();
    void cancel();
    bool operator<(VirtualClockEvent const& other) const;
};

/**
 * This is the class you probably want to use: it is coupled with a
 * VirtualClock, so advances with per-VirtualClock simulated time, and therefore
 * runs at full speed during simulation/testing.
 */
class VirtualTimer : private NonMovableOrCopyable
{
    VirtualClock& mClock;
    VirtualClock::time_point mExpiryTime;
    std::vector<std::shared_ptr<VirtualClockEvent>> mEvents;
    bool mCancelled;
    bool mDeleting;

  public:
    VirtualTimer(Application& app);
    VirtualTimer(VirtualClock& app);
    ~VirtualTimer();

    VirtualClock::time_point const& expiry_time() const;
    size_t seq() const;
    void expires_at(VirtualClock::time_point t);
    void expires_from_now(VirtualClock::duration d);
    template <typename R, typename P>
    void
    expires_from_now(std::chrono::duration<R, P> const& d)
    {
        expires_from_now(std::chrono::duration_cast<VirtualClock::duration>(d));
    }
    void async_wait(std::function<void(asio::error_code)> const& fn);
    void async_wait(std::function<void()> const& onSuccess,
                    std::function<void(asio::error_code)> const& onFailure);
    void cancel();

    static void onFailureNoop(asio::error_code const&){};
};

// This is almost certainly not the type you want to use. So much so
// that we will not even show it to you unless you define an unwieldy
// symbol:
#ifdef STELLAR_CORE_REAL_TIMER_FOR_CERTAIN_NOT_JUST_VIRTUAL_TIME
class RealTimer : public asio::basic_waitable_timer<std::chrono::system_clock>
{
  public:
    RealTimer(asio::io_service& io)
        : asio::basic_waitable_timer<std::chrono::system_clock>(io)
    {
    }
};
#endif
}
