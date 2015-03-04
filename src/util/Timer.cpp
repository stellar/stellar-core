#include <chrono>
#include "main/Application.h"
#include "util/Timer.h"
#include "util/Logging.h"

namespace stellar
{

using namespace std;


VirtualClock::time_point
VirtualClock::now() noexcept
{
    return mNow;
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
    setNoneIdle();
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
    setNoneIdle();
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
VirtualClock::allIdle() const
{
    bool allIdle = true;
    for (auto const& pair : mIdleFlags)
    {
        allIdle = allIdle && pair.second;
    }
    return allIdle;
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

void
VirtualClock::setNoneIdle()
{
    for (auto& pair : mIdleFlags)
    {
        pair.second = false;
    }
}

void
VirtualClock::setIdle(Application& app, bool isIdle)
{
    mIdleFlags[&app] = isIdle;
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
    setNoneIdle();
    return c;
}

size_t
VirtualClock::advanceToNextIfAllIdle()
{
    if (allEmpty())
    {
        return 0;
    }
    if (!allIdle())
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

}
