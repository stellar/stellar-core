// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Timer.h"
#include "main/Application.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/Scheduler.h"
#include <Tracy.hpp>
#include <chrono>
#include <cstdio>
#include <thread>

namespace stellar
{

using namespace std;

static const std::chrono::milliseconds CRANK_TIME_SLICE(500);
static const size_t CRANK_EVENT_SLICE = 100;
const std::chrono::seconds SCHEDULER_LATENCY_WINDOW(5);

VirtualClock::VirtualClock(Mode mode)
    : mMode(mode)
    , mActionScheduler(
          std::make_unique<Scheduler>(*this, SCHEDULER_LATENCY_WINDOW))
    , mRealTimer(mIOContext)
{
}

VirtualClock::time_point
VirtualClock::now() const noexcept
{
    if (mMode == REAL_TIME)
    {
        return std::chrono::steady_clock::now();
    }
    else
    {
        return mVirtualNow;
    }
}

VirtualClock::system_time_point
VirtualClock::system_now() const noexcept
{
    if (mMode == REAL_TIME)
    {
        return std::chrono::system_clock::now();
    }
    else
    {
        auto offset = mVirtualNow.time_since_epoch();
        return std::chrono::system_clock::time_point(
            std::chrono::duration_cast<
                std::chrono::system_clock::time_point::duration>(offset));
    }
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
VirtualClock::next() const
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

VirtualClock::system_time_point
VirtualClock::from_time_t(std::time_t timet)
{
    system_time_point epoch;
    return epoch + std::chrono::seconds(timet);
}

std::time_t
VirtualClock::to_time_t(system_time_point point)
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
VirtualClock::systemPointToTm(system_time_point point)
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

VirtualClock::system_time_point
VirtualClock::tmToSystemPoint(tm t)
{
    time_t tt = timegm(&t);
    return VirtualClock::system_time_point() + std::chrono::seconds(tt);
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
    std::memset(&res, 0, sizeof(res));
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
VirtualClock::systemPointToISOString(system_time_point point)
{
    return tmToISOString(systemPointToTm(point));
}

void
VirtualClock::enqueue(shared_ptr<VirtualClockEvent> ve)
{
    if (mDestructing)
    {
        return;
    }
    assertThreadIsMain();
    mEvents.emplace(ve);
    maybeSetRealtimer();
}

void
VirtualClock::flushCancelledEvents()
{
    ZoneScoped;
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
    ZoneScoped;
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
VirtualClock::setCurrentVirtualTime(time_point t)
{
    assert(mMode == VIRTUAL_TIME);
    // Maintain monotonicity in VIRTUAL_TIME mode.
    releaseAssert(t >= mVirtualNow);
    mVirtualNow = t;
}

void
VirtualClock::setCurrentVirtualTime(system_time_point t)
{
    auto offset = t.time_since_epoch();
    setCurrentVirtualTime(time_point(offset));
}

void
VirtualClock::sleep_for(std::chrono::microseconds us)
{
    ZoneScoped;
    if (mMode == VIRTUAL_TIME)
    {
        setCurrentVirtualTime(now() + us);
    }
    else
    {
        std::this_thread::sleep_for(us);
    }
}

bool
VirtualClock::shouldYield() const
{
    if (mMode == VIRTUAL_TIME)
    {
        return true;
    }
    else
    {
        using namespace std::chrono;
        auto dur = now() - mLastDispatchStart;
        return duration_cast<milliseconds>(dur) > CRANK_TIME_SLICE;
    }
}

static size_t
crankStep(VirtualClock& clock, std::function<size_t()> step, size_t divisor = 1)
{
    size_t eCount = 0;
    auto tLimit = clock.now() + (CRANK_TIME_SLICE / divisor);
    size_t totalProgress = 0;
    while (clock.now() < tLimit && ++eCount < (CRANK_EVENT_SLICE / divisor))
    {
        size_t stepProgress = step();
        totalProgress += stepProgress;
        if (stepProgress == 0)
        {
            break;
        }
    }
    return totalProgress;
}

size_t
VirtualClock::crank(bool block)
{
    ZoneScoped;
    if (mDestructing)
    {
        return 0;
    }
    size_t progressCount = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(mDispatchingMutex);
        mDispatching = true;
        mLastDispatchStart = now();
        nRealTimerCancelEvents = 0;
        if (mMode == REAL_TIME)
        {
            ZoneNamedN(timerZone, "dispatch timers", true);
            // Dispatch all pending timers.
            progressCount += advanceToNow();
        }

        // Dispatch some IO event completions.
        mLastDispatchStart = now();
        // Bias towards the execution queue exponentially based on how long the
        // scheduler has been overloaded.
        auto overloadedDuration =
            std::min(static_cast<std::chrono::seconds::rep>(30),
                     mActionScheduler->getOverloadedDuration().count());
        std::string overloadStr =
            overloadedDuration > 0 ? "overloaded" : "slack";
        size_t ioDivisor = 1ULL << overloadedDuration;
        {
            ZoneNamedN(ioPollZone, "ASIO polling", true);
            ZoneText(overloadStr.c_str(), overloadStr.size());
            progressCount += crankStep(
                *this, [this] { return this->mIOContext.poll_one(); },
                ioDivisor);
        }

        // Dispatch some scheduled actions.
        mLastDispatchStart = now();
        {
            ZoneNamedN(schedZone, "scheduler", true);
            progressCount += crankStep(
                *this, [this] { return this->mActionScheduler->runOne(); });
        }

        // Subtract out any timer cancellations from the above two steps.
        progressCount -= nRealTimerCancelEvents;
        if (mMode == VIRTUAL_TIME && progressCount == 0)
        {
            // If we did nothing and we're in virtual mode, we're idle and can
            // skip time forward, dispatching all timers at the next time-step.
            progressCount += advanceToNext();
        }
        mDispatching = false;
    }
    // At this point main and background threads can add work to next crank.
    if (block && progressCount == 0)
    {
        ZoneNamedN(blockingZone, "ASIO blocking", true);
        // If we didn't make progress and caller wants blocking, block now.
        progressCount += mIOContext.run_one();
    }
    return progressCount;
}

void
VirtualClock::postAction(std::function<void()>&& f, std::string&& name,
                         Scheduler::ActionType type)
{
    std::lock_guard<std::recursive_mutex> lock(mDispatchingMutex);
    if (!mDispatching)
    {
        // Either we are waiting on io_context().run_one, or by some chance
        // run_one was woken up by network activity and postAction was
        // called from a background thread.

        // In any case, all we need to do is ensure that we wake up `crank`, we
        // do this by posting an empty event to the main IO queue
        mDispatching = true;
        asio::post(mIOContext, []() {});
    }
    mActionScheduler->enqueue(std::move(name), std::move(f), type);
}

size_t
VirtualClock::getActionQueueSize() const
{
    return mActionScheduler->size();
}

bool
VirtualClock::actionQueueIsOverloaded() const
{
    return mActionScheduler->getOverloadedDuration().count() != 0;
}

Scheduler::ActionType
VirtualClock::currentSchedulerActionType() const
{
    return mActionScheduler->currentActionType();
}

asio::io_context&
VirtualClock::getIOContext()
{
    return mIOContext;
}

VirtualClock::~VirtualClock()
{
    mDestructing = true;
    cancelAllEvents();
}

size_t
VirtualClock::advanceToNow()
{
    ZoneScoped;
    if (mDestructing)
    {
        return 0;
    }
    assertThreadIsMain();

    auto n = now();
    vector<shared_ptr<VirtualClockEvent>> toDispatch;
    while (!mEvents.empty())
    {
        if (mEvents.top()->mWhen > n)
            break;
        toDispatch.push_back(mEvents.top());
        mEvents.pop();
    }
    // Keep the dispatch loop separate from the pop()-ing loop
    // so the triggered events can't mutate the priority queue
    // from underneath us while we are looping.
    for (auto ev : toDispatch)
    {
        ev->trigger();
    }
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

    auto nextEvent = next();
    // jump forward in time, if needed
    if (mVirtualNow < nextEvent)
    {
        mVirtualNow = nextEvent;
    }
    return advanceToNow();
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
        ZoneScoped;
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
    ZoneScoped;
    cancel();
    mExpiryTime = t;
    mCancelled = false;
}

void
VirtualTimer::expires_from_now(VirtualClock::duration d)
{
    ZoneScoped;
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
