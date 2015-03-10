// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "util/Timer.h"
#include <chrono>
#include "main/Application.h"
#include "util/Logging.h"

namespace stellar
{

using namespace std;

VirtualClock::VirtualClock(Mode mode)
    : mRealTimer(mIOService)
    , mMode(mode)
{
    if (mMode == REAL_TIME)
    {
        mNow = std::chrono::steady_clock::now();
    }
}

VirtualClock::time_point
VirtualClock::now() noexcept
{
    if (mMode == REAL_TIME)
    {
        mNow = std::chrono::steady_clock::now();
    }
    return mNow;
}

void
VirtualClock::maybeSetRealtimer()
{
    if (mMode == REAL_TIME)
    {
        mRealTimer.expires_at(next());
        mRealTimer.async_wait(
            [this](asio::error_code const& ec)
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
    VirtualClock::time_point least = time_point::max();
    for (auto& pair : mEvents)
    {
        auto& events = *pair.second;
        if (events.empty())
            continue;
        if (events.top().mWhen < least)
        {
            least = events.top().mWhen;
        }
    }
    return least;
}

std::time_t
VirtualClock::pointToTimeT(time_point point)
{
    return
        static_cast<std::time_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                point.time_since_epoch()).count());
}

#ifdef _WIN32
time_t
timegm(struct tm *tm) {
    time_t zero = 0;
    time_t localEpoch = mktime(gmtime(&zero));
    time_t local = mktime(tm);
    return local - localEpoch;
}
#endif

std::tm
VirtualClock::pointToTm(time_point point)
{
    std::time_t rawtime = pointToTimeT(point);
    std::tm out;
#ifdef _WIN32
    // On Win32 this is returns a thread-local and there's no _r variant.
    std::tm *tmPtr = gmtime(&rawtime);
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
VirtualClock::enqueue(Application& app, VirtualClockEvent const& ve)
{
    // LOG(DEBUG) << "VirtualClock::enqueue";
    if (mEvents.find(&app) == mEvents.end())
    {
        mEvents.insert(std::make_pair(&app, std::make_shared<std::priority_queue<VirtualClockEvent>>()));
    }
    mEvents[&app]->emplace(ve);
}

bool
VirtualClock::cancelAllEventsFrom(Application& a,
                                  std::function<bool(VirtualClockEvent const&)> pred)
{
    // LOG(DEBUG) << "VirtualClock::cancelAllEventsFrom";
    // Brute force approach; could be done better with a linked list
    // per timer, as is done in asio. For the time being this should
    // be tolerable.
    vector<VirtualClockEvent> toCancel;
    vector<VirtualClockEvent> toKeep;
    auto i = mEvents.find(&a);
    if (i == mEvents.end())
    {
        // No events for this application
        return false;
    }
    auto& events = *i->second;
    toCancel.reserve(events.size());
    toKeep.reserve(events.size());
    bool changed = false;
    while (!events.empty())
    {
        auto const& e = events.top();
        if (e.live())
        {
            if (pred(e))
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
        events.pop();
    }
    for (auto& e : toCancel)
    {
        e.mCallback(asio::error::operation_aborted);
    }
    events =
        priority_queue<VirtualClockEvent>(toKeep.begin(),
                                          toKeep.end());
    return changed;
}

bool
VirtualClock::cancelAllEventsFrom(Application& a)
{
    return cancelAllEventsFrom(a, [](VirtualClockEvent const&) { return true; });
}

bool
VirtualClock::cancelAllEventsFrom(Application& a, VirtualTimer& v)
{
    return cancelAllEventsFrom(a, [&v](VirtualClockEvent const& e) { return e.mTimer == &v; });
}

bool
VirtualClock::allEmpty() const
{
    bool allEmpty = true;
    for (auto const& pair : mEvents)
    {
        allEmpty = allEmpty && pair.second->empty();
    }
    return allEmpty;
}

size_t
VirtualClock::advanceToNow()
{
    assert(mMode == REAL_TIME);
    return advanceTo(now());
}

size_t
VirtualClock::crank(bool block)
{
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

size_t
VirtualClock::advanceTo(time_point n)
{
    priority_queue<VirtualClockEvent> toDispatch;
    // LOG(DEBUG) << "VirtualClock::advanceTo("
    //            << n.time_since_epoch().count() << ")";
    mNow = n;
    for (auto& pair : mEvents)
    {
        auto& events = *pair.second;
        while (!events.empty())
        {
            if (events.top().mWhen > mNow)
                break;
            toDispatch.emplace(events.top());
            events.pop();
        }
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
    assert(mMode == VIRTUAL_TIME);
    if (allEmpty())
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
    return mTimer;
}

VirtualTimer::VirtualTimer(Application& app)
    : mApp(app)
    , mExpiryTime(app.getClock().now())
    , mCancelled(false)
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
        mApp.getClock().cancelAllEventsFrom(mApp, *this);
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
    mExpiryTime = mApp.getClock().now() + d;
    mCancelled = false;
}

void
VirtualTimer::async_wait(function<void(asio::error_code)> const& fn)
{
    if (!mCancelled)
    {
        mApp.getClock().enqueue(mApp, VirtualClockEvent{mExpiryTime, fn, this});
    }
}

void
VirtualTimer::async_wait(std::function<void()> const& onSuccess, std::function<void(asio::error_code)> const& onFailure)
{
    if (!mCancelled)
    {
        mApp.getClock().enqueue(mApp, VirtualClockEvent{
                mExpiryTime,
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
