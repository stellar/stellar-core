// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Timer.h"
#include "main/Application.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include <chrono>
#include <cstdio>
#include <thread>

namespace stellar
{

using namespace std;

static const uint32_t RECENT_CRANK_WINDOW = 1024;

VirtualClock::VirtualClock(Mode mode) : mRealTimer(mIOService), mMode(mode)
{
    resetIdleCrankPercent();
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
        auto nextp = next();
        if (nextp != mRealTimer.expires_at())
        {
            mRealTimer.expires_at(nextp);
            mRealTimer.async_wait([this](asio::error_code const& ec) {
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
    assertThreadIsMain();
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
            point.time_since_epoch())
            .count());
}

#ifdef _WIN32
time_t
timegm(struct tm* tm)
{
    // Timezone independant version
    return _mkgmtime(tm);
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

std::tm
VirtualClock::isoStringToTm(std::string const& iso)
{
    std::tm res;
    int y, M, d, h, m, s;
    if (std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%dZ", &y, &M, &d, &h, &m,
                    &s) != 6)
    {
        throw std::invalid_argument("Could not parse iso date");
    }
    res.tm_year = y - 1900;
    res.tm_mon = M - 1;
    res.tm_mday = d;
    res.tm_hour = h;
    res.tm_min = m;
    res.tm_sec = s;
    res.tm_isdst = 0;
    res.tm_wday = 0;
    res.tm_yday = 0;
    return res;
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
    assertThreadIsMain();
    // LOG(DEBUG) << "VirtualClock::enqueue";
    mEvents.emplace(ve);
    maybeSetRealtimer();
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
    assertThreadIsMain();
    // LOG(DEBUG) << "VirtualClock::cancelAllEventsFrom";

    auto toKeep = vector<shared_ptr<VirtualClockEvent>>();
    toKeep.reserve(mEvents.size());

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
    maybeSetRealtimer();
}

bool
VirtualClock::cancelAllEvents()
{
    assertThreadIsMain();

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

void
VirtualClock::setCurrentTime(time_point t)
{
    assert(mMode == VIRTUAL_TIME);
    mNow = t;
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

    // pick up some work off the IO queue
    // calling mIOService.poll() here may introduce unbounded delays
    // to trigger timers
    const size_t WORK_BATCH_SIZE = 10;
    size_t lastPoll;
    size_t i = 0;
    do
    {
        lastPoll = mIOService.poll_one();
        nWorkDone += lastPoll;
    } while (lastPoll != 0 && ++i < WORK_BATCH_SIZE);

    nWorkDone -= nRealTimerCancelEvents;

    if (mMode == VIRTUAL_TIME && nWorkDone == 0)
    {
        // If we did nothing and we're in virtual mode,
        // we're idle and can skip time forward.
        nWorkDone += advanceToNext();
    }

    if (block && nWorkDone == 0)
    {
        nWorkDone += mIOService.run_one();
    }

    noteCrankOccurred(nWorkDone == 0);

    return nWorkDone;
}

void
VirtualClock::noteCrankOccurred(bool hadIdle)
{
    // Record execution of a crank and, optionally, whether we had an idle
    // poll event; this is used to estimate overall business of the system
    // via recentIdleCrankPercent().
    ++mRecentCrankCount;
    if (hadIdle)
    {
        ++mRecentIdleCrankCount;
    }

    // Divide-out older samples once we have a suitable number of cranks to
    // evaluate a ratio. This makes the measurement "present-biased" and
    // the threshold (RECENT_CRANK_WINDOW) sets the size of the window (in
    // cranks) that we consider as "the present". Using an event count
    // rather than a time based EWMA (as in a medida::Meter) makes the
    // ratio more precise, at the expense of accuracy of present-focus --
    // we might accidentally consider samples from 1s, 1m or even 1h ago as
    // "present" if the system is very lightly loaded. But since we're
    // going to use this value to disconnect peers when overloaded, this is
    // the preferred tradeoff.
    if (mRecentCrankCount > RECENT_CRANK_WINDOW)
    {
        mRecentCrankCount >>= 1;
        mRecentIdleCrankCount >>= 1;
    }
}

uint32_t
VirtualClock::recentIdleCrankPercent() const
{
    if (mRecentCrankCount == 0)
    {
        return 0;
    }
    uint32_t v = static_cast<uint32_t>(
        (100ULL * static_cast<uint64_t>(mRecentIdleCrankCount)) /
        static_cast<uint64_t>(mRecentCrankCount));

    LOG(DEBUG) << "Estimated clock loop idle: " << v << "% ("
               << mRecentIdleCrankCount << "/" << mRecentCrankCount << ")";

    // This should _really_ never be >100, it's a bug in our code if so.
    assert(v <= 100);

    return v;
}

void
VirtualClock::resetIdleCrankPercent()
{
    mRecentCrankCount = RECENT_CRANK_WINDOW >> 1;
    mRecentIdleCrankCount = RECENT_CRANK_WINDOW >> 1;
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
    for (auto ev : toDispatch)
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
    assertThreadIsMain();
    if (mEvents.empty())
    {
        return 0;
    }
    return advanceTo(next());
}

VirtualClockEvent::VirtualClockEvent(
    VirtualClock::time_point when, size_t seq,
    std::function<void(asio::error_code)> callback)
    : mCallback(callback), mTriggered(false), mWhen(when), mSeq(seq)
{
}

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
        mCallback = nullptr;
    }
}

void
VirtualClockEvent::cancel()
{
    if (!mTriggered)
    {
        mTriggered = true;
        mCallback(asio::error::operation_aborted);
        mCallback = nullptr;
    }
}

bool
VirtualClockEvent::operator<(VirtualClockEvent const& other) const
{
    // For purposes of priority queue, a timer is "less than"
    // another timer if it occurs in the future (has a higher
    // expiry time). The "greatest" timer is timer 0.
    // To break time-based ties (but preserve the ordering in which
    // events were enqueued) we add an event-sequence number as well,
    // such that a higher sequence number makes an event "less than"
    // another.
    return mWhen > other.mWhen || (mWhen == other.mWhen && mSeq > other.mSeq);
}

VirtualTimer::VirtualTimer(Application& app) : VirtualTimer(app.getClock())
{
}

VirtualTimer::VirtualTimer(VirtualClock& clock)
    : mClock(clock)
    , mExpiryTime(mClock.now())
    , mCancelled(false)
    , mDeleting(false)
{
}

VirtualTimer::~VirtualTimer()
{
    mDeleting = true;
    cancel();
}

void
VirtualTimer::cancel()
{
    if (!mCancelled)
    {
        mCancelled = true;
        for (auto ev : mEvents)
        {
            ev->cancel();
        }
        mClock.flushCancelledEvents();
        mEvents.clear();
    }
}

VirtualClock::time_point const&
VirtualTimer::expiry_time() const
{
    return mExpiryTime;
}

size_t
VirtualTimer::seq() const
{
    return mEvents.empty() ? 0 : mEvents.back()->mSeq + 1;
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
        assert(!mDeleting);
        auto ve = make_shared<VirtualClockEvent>(mExpiryTime, seq(), fn);
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
        assert(!mDeleting);
        auto ve = make_shared<VirtualClockEvent>(
            mExpiryTime, seq(), [onSuccess, onFailure](asio::error_code error) {
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
