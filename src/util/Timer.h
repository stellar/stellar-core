#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"
#include "util/NonCopyable.h"
#include "util/Scheduler.h"

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

extern const std::chrono::seconds SCHEDULER_LATENCY_WINDOW;

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
    typedef std::chrono::steady_clock::duration duration;
    typedef duration::rep rep;
    typedef duration::period period;
    typedef std::chrono::steady_clock::time_point time_point;
    static const bool is_steady = true;

    // We also provide a "system clock" interface. This is _not_ related to the
    // steady_clock / time_point time -- system_time_points are wall/calendar
    // time which may move forwards or backwards as NTP adjusts it. It should be
    // used only for things like nomination of a ledger close time (in consensus
    // with other peers), or interfacing to other programs' notions of absolute
    // time. Not calculating durations / timers / local-event deadlines.
    typedef std::chrono::system_clock::time_point system_time_point;

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
    static std::time_t to_time_t(system_time_point);
    static system_time_point from_time_t(std::time_t);

    // These are for conversion of system time to external contexts that
    // believe in a concept of absolute time.
    static std::tm systemPointToTm(system_time_point);
    static VirtualClock::system_time_point tmToSystemPoint(tm t);
    static std::tm isoStringToTm(std::string const& iso);
    static std::string tmToISOString(std::tm const& tm);
    static std::string systemPointToISOString(system_time_point point);

    enum Mode
    {
        REAL_TIME,
        VIRTUAL_TIME
    };

    // Call this in any loop that should continue for up-to a single
    // real-time-quantum of scheduling. NB: In VIRTUAL_TIME mode this will
    // always return true, to improve test determinism. This means you should
    // _not_ use this in a while-loop header if you need to enter the loop to
    // make progress. Use a do-while loop or break mid-loop or something.
    bool shouldYield() const;

  private:
    asio::io_context mIOContext;
    Mode mMode;

    size_t nRealTimerCancelEvents{0};
    time_point mVirtualNow;

    std::recursive_mutex mDispatchingMutex;
    bool mDispatching{true};
    std::chrono::steady_clock::time_point mLastDispatchStart;
    std::unique_ptr<Scheduler> mActionScheduler;
    using PrQueue =
        std::priority_queue<std::shared_ptr<VirtualClockEvent>,
                            std::vector<std::shared_ptr<VirtualClockEvent>>,
                            VirtualClockEventCompare>;
    PrQueue mEvents;
    size_t mFlushesIgnored = 0;

    bool mDestructing{false};

    void maybeSetRealtimer();
    size_t advanceToNext();
    size_t advanceToNow();

    // timer should be last to ensure it gets destroyed first
    asio::basic_waitable_timer<std::chrono::steady_clock> mRealTimer;

  public:
    // A VirtualClock is instantiated in either real or virtual mode. In real
    // mode, crank() sleeps until the next event, either timer or IO; in virtual
    // mode it processes IO events until IO is idle then advances to the time of
    // the next virtual event instantly.

    VirtualClock(Mode mode = VIRTUAL_TIME);
    ~VirtualClock();
    size_t crank(bool block = true);
    asio::io_context& getIOContext();

    // Note: this is not a static method, which means that VirtualClock is
    // not an implementation of the C++ `Clock` concept; there is no global
    // virtual time. Each virtual clock has its own time.
    time_point now() const noexcept;

    // This returns a system_time_point which comes from the system clock in
    // REAL_TIME mode. In VIRTUAL_TIME mode this returns the system-time epoch
    // plus the steady time offset, i.e. "some time early in 1970" (unless
    // someone has set the time forward using setCurrentVirtualTime below).
    system_time_point system_now() const noexcept;

    void enqueue(std::shared_ptr<VirtualClockEvent> ve);
    void flushCancelledEvents();
    bool cancelAllEvents();

    // Only valid with VIRTUAL_TIME: sets the current value of the
    // clock. Asserts that t is >= current virtual time.
    void setCurrentVirtualTime(time_point t);
    // Setting virtual time using a system time works too, though
    // it still has to be a forward adjustment.
    void setCurrentVirtualTime(system_time_point t);
    // Calls "sleep_for" if REAL_TIME, otherwise, just adds time to the virtual
    // clock.
    void sleep_for(std::chrono::microseconds us);

    // Returns the time of the next scheduled event.
    time_point next() const;

    void postAction(std::function<void()>&& f, std::string&& name,
                    Scheduler::ActionType type);

    size_t getActionQueueSize() const;
    bool actionQueueIsOverloaded() const;
    Scheduler::ActionType currentSchedulerActionType() const;
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
    RealTimer(asio::io_context& io)
        : asio::basic_waitable_timer<std::chrono::system_clock>(io)
    {
    }
};
#endif
}
