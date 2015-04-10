// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Timer.h"
#include <chrono>
#include "main/Application.h"
#include "util/Logging.h"

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

bool 
VirtualClockEventCompare::operator()(shared_ptr<VirtualClockEvent> a, 
                                     shared_ptr<VirtualClockEvent> b)
{
    return *a < *b;
}

VirtualClock::time_point
VirtualClock::next()
{
    VirtualClock::time_point least = time_point::max();
    if (!mEvents.empty())
    {
        if (mEvents.top()->mWhen < least)
        {
            least = mEvents.top()->mWhen;
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
VirtualClock::enqueue(shared_ptr<VirtualClockEvent> ve)
{
    if (mDestructing)
    {
        return;
    }
    // LOG(DEBUG) << "VirtualClock::enqueue";
    mEvents.emplace(ve);
}

void
VirtualClock::flushCancelledEvents()
{
    if (mDestructing)
    {
        return;
    }
    if (mFlushesIgnored <= mEvents.size())
    {
        // Flushing all the canceled events immediately can lead to 
        // a O(n^2) loop if many events in the queue are canceled one a time.
        //
        // By ignoring O(n) flushes, we batch the iterations together and 
        // restore O(n) performance.
        mFlushesIgnored++;
        return;
    }
    // LOG(DEBUG) << "VirtualClock::cancelAllEventsFrom";

    auto toKeep = vector<shared_ptr<VirtualClockEvent>>();

    while (!mEvents.empty())
    {
        auto const& e = mEvents.top();
        if (!e->getTriggered())
        {
            toKeep.emplace_back(e);
        }
        mEvents.pop();
    }
    mFlushesIgnored = 0;
    mEvents = PrQueue(toKeep.begin(), toKeep.end());
    return;
}

bool
VirtualClock::cancelAllEvents()
{
    bool wasEmpty = mEvents.empty();
    while (!mEvents.empty())
    {
        auto ev = mEvents.top();
        mEvents.pop();
        ev->cancel();
    }
    mEvents = PrQueue();
    return !wasEmpty;
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
    // LOG(DEBUG) << "VirtualClock::advanceTo("
    //            << n.time_since_epoch().count() << ")";
    mNow = n;
    vector<shared_ptr<VirtualClockEvent>> toDispatch;
    while (!mEvents.empty())
    {
        if (mEvents.top()->mWhen > mNow)
            break;
        toDispatch.push_back(mEvents.top());
        mEvents.pop();
    }
    // Keep the dispatch loop separate from the pop()-ing loop
    // so the triggered events can't mutate the priority queue 
    // from underneat us while we are looping.
    for(auto ev : toDispatch)
    {
        ev->trigger();
    }
    // LOG(DEBUG) << "VirtualClock::advanceTo done";
    maybeSetRealtimer();
    return toDispatch.size();
}

size_t
VirtualClock::advanceToNext()
{
    if (mDestructing)
    {
        return 0;
    }
    assert(mMode == VIRTUAL_TIME);
    if (mEvents.empty())
    {
        return 0;
    }
    return advanceTo(next());
}

VirtualClockEvent::VirtualClockEvent(
    VirtualClock::time_point when,
    std::function<void(asio::error_code)> callback) 
    :
    mCallback(callback), mTriggered(false), mWhen(when) { }

bool
VirtualClockEvent::getTriggered()
{
    return mTriggered;
}
void 
VirtualClockEvent::trigger()
{
    if (!mTriggered)
    {
        mTriggered = true;
        mCallback(asio::error_code());
    }
}

void 
VirtualClockEvent::cancel()
{
    if (!mTriggered)
    {
        mTriggered = true;
        mCallback(asio::error::operation_aborted);
    }
}

bool 
VirtualClockEvent::operator<(VirtualClockEvent const& other) const
{
    // For purposes of priority queue, a timer is "less than"
    // another timer if it occurs in the future (has a higher
    // expiry time). The "greatest" timer is timer 0.
    return mWhen > other.mWhen;
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
        for(auto ev : mEvents)
        {
            ev->cancel();
        }
        mClock.flushCancelledEvents();
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
        auto ve = make_shared<VirtualClockEvent>(mExpiryTime, fn);
        mClock.enqueue(ve);
        mEvents.push_back(ve);
    }
}

void
VirtualTimer::async_wait(std::function<void()> const& onSuccess,
                         std::function<void(asio::error_code)> const& onFailure)
{
    if (!mCancelled)
    {
        auto ve = make_shared<VirtualClockEvent>(mExpiryTime,
            [onSuccess, onFailure](asio::error_code error)
        {
            if (error)
                onFailure(error);
            else
                onSuccess();
        });
        mClock.enqueue(ve);
        mEvents.push_back(ve);
    }
}
}
