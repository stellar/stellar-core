// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Timer.h"
#include <chrono>
#include "main/Application.h"
#include "util/Logging.h"
#include <thread>
#include "util/GlobalChecks.h"

namespace stellar
{

using namespace std;

VirtualClock::VirtualClock(Mode mode) : mRealTimer(mIOService), mMode(mode)
{
    if (mMode == REAL_TIME)
    {
        mNow = std::chrono::system_clock::now();
    }
}

VirtualClock::time_point
VirtualClock::now() noexcept
{
    if (mMode == REAL_TIME)
    {
        mNow = std::chrono::system_clock::now();
    }
    return mNow;
}

void
VirtualClock::maybeSetRealtimer()
{
    if (mMode == REAL_TIME)
    {
        mRealTimer.expires_at(next());
        mRealTimer.async_wait([this](asio::error_code const& ec)
                              {
                                  if (ec == asio::error::operation_aborted)
                                  {
                                      ++this->nRealTimerCancelEvents;
                                  }
                                  else
                                  {
                                      this->advanceToNow();
                                  }
                              });
    }
}

VirtualClock::time_point
VirtualClock::next()
{
    assertThreadIsMain();
    VirtualClock::time_point least = time_point::max();
    if (!mEvents.empty())
    {
        if (mEvents.top().mWhen < least)
        {
            least = mEvents.top().mWhen;
        }
    }
    return least;
}

VirtualClock::time_point
VirtualClock::from_time_t(std::time_t timet)
{
    time_point epoch;
    return epoch + std::chrono::seconds(timet);
}

std::time_t
VirtualClock::to_time_t(time_point point)
{
    return static_cast<std::time_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            point.time_since_epoch()).count());
}

#ifdef _WIN32
time_t
timegm(struct tm* tm)
{
    time_t zero = 0;
    time_t localEpoch = mktime(gmtime(&zero));
    time_t local = mktime(tm);
    return local - localEpoch;
}
#endif

std::tm
VirtualClock::pointToTm(time_point point)
{
    std::time_t rawtime = to_time_t(point);
    std::tm out;
#ifdef _WIN32
    // On Win32 this is returns a thread-local and there's no _r variant.
    std::tm* tmPtr = gmtime(&rawtime);
    out = *tmPtr;
#else
    // On Unix the _r variant uses a local output, so is thread safe.
    gmtime_r(&rawtime, &out);
#endif
    return out;
}

VirtualClock::time_point
VirtualClock::tmToPoint(tm t)
{
    time_t tt = timegm(&t);
    return VirtualClock::time_point() + std::chrono::seconds(tt);
}

std::string
VirtualClock::tmToISOString(std::tm const& tm)
{
    char buf[sizeof("0000-00-00T00:00:00Z")];
    size_t conv = strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    assert(conv == (sizeof(buf) - 1));
    return std::string(buf);
}

std::string
VirtualClock::pointToISOString(time_point point)
{
    return tmToISOString(pointToTm(point));
}

void
VirtualClock::enqueue(VirtualClockEvent const& ve)
{
    if (mDestructing)
    {
        return;
    }
    // LOG(DEBUG) << "VirtualClock::enqueue";
    mEvents.emplace(ve);
    assertThreadIsMain();
}

bool
VirtualClock::cancelAllEventsFrom(VirtualTimer& v)
{
    if (mDestructing)
    {
        return false;
    }
    assertThreadIsMain();
    // LOG(DEBUG) << "VirtualClock::cancelAllEventsFrom";
    // Brute force approach; could be done better with a linked list
    // per timer, as is done in asio. For the time being this should
    // be tolerable.
    vector<VirtualClockEvent> toCancel;
    vector<VirtualClockEvent> toKeep;
    toCancel.reserve(mEvents.size());
    toKeep.reserve(mEvents.size());
    bool changed = false;
    while (!mEvents.empty())
    {
        auto const& e = mEvents.top();
        if (e.live())
        {
            if (e.mTimer == &v)
            {
                changed = true;
                toCancel.emplace_back(e);
            }
            else
            {
                toKeep.emplace_back(e);
            }
        }
        else
        {
            changed = true;
        }
        mEvents.pop();
    }
    for (auto& e : toCancel)
    {
        e.mCallback(asio::error::operation_aborted);
    }
    mEvents = priority_queue<VirtualClockEvent>(toKeep.begin(), toKeep.end());
    return changed;
}

bool
VirtualClock::cancelAllEvents()
{
    assertThreadIsMain();
    bool empty = mEvents.empty();
    auto events = mEvents;
    mEvents = priority_queue<VirtualClockEvent>();
    while (!events.empty())
    {
        events.top().mCallback(asio::error::operation_aborted);
        events.pop();
    }
    return !empty;
}

size_t
VirtualClock::advanceToNow()
{
    if (mDestructing)
    {
        return 0;
    }
    assert(mMode == REAL_TIME);
    return advanceTo(now());
}

size_t
VirtualClock::crank(bool block)
{
    if (mDestructing)
    {
        return 0;
    }
    nRealTimerCancelEvents = 0;
    size_t nWorkDone = 0;
    if (mMode == REAL_TIME)
    {
        // Fire all pending timers.
        nWorkDone += advanceToNow();
    }

    if (block)
    {
        nWorkDone += mIOService.run();
    }
    else
    {
        nWorkDone += mIOService.poll();
    }

    nWorkDone -= nRealTimerCancelEvents;
    if (mMode == VIRTUAL_TIME && nWorkDone == 0)
    {
        // If we did nothing and we're in virtual mode,
        // we're idle and can skip time forward.
        nWorkDone += advanceToNext();
    }

    return nWorkDone;
}

asio::io_service&
VirtualClock::getIOService()
{
    return mIOService;
}

VirtualClock::~VirtualClock()
{
    mDestructing = true;
    cancelAllEvents();
}


size_t
VirtualClock::advanceTo(time_point n)
{
    if (mDestructing)
    {
        return 0;
    }
    assertThreadIsMain();
    priority_queue<VirtualClockEvent> toDispatch;
    // LOG(DEBUG) << "VirtualClock::advanceTo("
    //            << n.time_since_epoch().count() << ")";
    mNow = n;
    while (!mEvents.empty())
    {
        if (mEvents.top().mWhen > mNow)
            break;
        toDispatch.emplace(mEvents.top());
        mEvents.pop();
    }
    // LOG(DEBUG) << "VirtualClock::advanceTo running "
    //            << toDispatch.size() << " events";
    size_t c = toDispatch.size();
    while (!toDispatch.empty())
    {
        auto& e = toDispatch.top();
        // LOG(DEBUG) << "VirtualClock::advanceTo callback set for @ "
        //            << e->mWhen.time_since_epoch().count();
        e.mCallback(asio::error_code());
        toDispatch.pop();
    }
    // LOG(DEBUG) << "VirtualClock::advanceTo done";
    maybeSetRealtimer();
    return c;
}

size_t
VirtualClock::advanceToNext()
{
    if (mDestructing)
    {
        return 0;
    }
    assert(mMode == VIRTUAL_TIME);
    assertThreadIsMain();
    if (mEvents.empty())
    {
        return 0;
    }
    return advanceTo(next());
}

VirtualClockEvent::~VirtualClockEvent()
{
    mTimer = nullptr;
}

bool
VirtualClockEvent::live() const
{
    return mTimer != nullptr;
}

VirtualTimer::VirtualTimer(Application& app)
    : VirtualTimer(app.getClock())
{
}

VirtualTimer::VirtualTimer(VirtualClock& clock)
    : mClock(clock), mExpiryTime(mClock.now()), mCancelled(false)
{
}

VirtualTimer::~VirtualTimer()
{
    cancel();
}

void
VirtualTimer::cancel()
{
    if (!mCancelled)
    {
        mCancelled = true;
        mClock.cancelAllEventsFrom(*this);
    }
}

void
VirtualTimer::expires_at(VirtualClock::time_point t)
{
    cancel();
    mExpiryTime = t;
    mCancelled = false;
}

void
VirtualTimer::expires_from_now(VirtualClock::duration d)
{
    cancel();
    mExpiryTime = mClock.now() + d;
    mCancelled = false;
}

void
VirtualTimer::async_wait(function<void(asio::error_code)> const& fn)
{
    if (!mCancelled)
    {
        mClock.enqueue(VirtualClockEvent{mExpiryTime, fn, this});
    }
}

void
VirtualTimer::async_wait(std::function<void()> const& onSuccess,
                         std::function<void(asio::error_code)> const& onFailure)
{
    if (!mCancelled)
    {
        mClock.enqueue(
            VirtualClockEvent{mExpiryTime,
                              [onSuccess, onFailure](asio::error_code error)
                              {
                                  if (error)
                                      onFailure(error);
                                  else
                                      onSuccess();
                              },
                              this});
    }
}
}
