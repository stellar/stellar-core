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

VirtualClock::VirtualClock(Mode mode) : mMode(mode), mRealTimer(mIOContext)
{
    mExecutionIterator = mExecutionQueue.begin();
    resetIdleCrankPercent();
}

VirtualClock::time_point
VirtualClock::now() noexcept
{
    if (mMode == REAL_TIME)
    {
        return std::chrono::system_clock::now();
    }
    else
    {
        return mVirtualNow;
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
VirtualClock::setCurrentVirtualTime(time_point t)
{
    assert(mMode == VIRTUAL_TIME);
    mVirtualNow = t;
}

void
VirtualClock::sleep_for(std::chrono::microseconds us)
{
    if (mMode == VIRTUAL_TIME)
    {
        setCurrentVirtualTime(now() + us);
    }
    else
    {
        std::this_thread::sleep_for(us);
    }
}

void
VirtualClock::advanceExecutionQueue()
{
    // limit in terms of number of items
    // ideally we would just pick up everything we can up to the duration.
    // There is a potential issue with picking up too many items all the time,
    // in that round robin gets defeated if work is submitted in batches.
    size_t EXECUTION_QUEUE_BATCH_SIZE = 100;

    // DURATION here needs to be small enough that timers get to fire often
    // enough but it needs to be big enough so that we do enough work generated
    // by pulling items from the IO queue
    std::chrono::milliseconds MAX_EXECUTION_QUEUE_DURATION{500};

    // safety in case we can't keep up at all with work
    const size_t QUEUE_SIZE_DROP_THRESHOLD = 10000;
    if (mExecutionQueueSize > QUEUE_SIZE_DROP_THRESHOLD)
    {
        // 1. drain elements that can be dropped
        for (auto it = mExecutionQueue.begin(); it != mExecutionQueue.end();)
        {
            if (it->first.mExecutionType ==
                ExecutionCategory::Type::DROPPABLE_EVENT)
            {
                mExecutionQueueSize -= it->second.size();
                it = mExecutionQueue.erase(it);
            }
            else
            {
                ++it;
            }
        }
        // 2. update parameters so that we help drain the queue
        // this may lock up the instance for a while, but this is the best thing
        // we can do
        EXECUTION_QUEUE_BATCH_SIZE = mExecutionQueueSize;
        MAX_EXECUTION_QUEUE_DURATION = std::chrono::seconds(30);

        mExecutionIterator = mExecutionQueue.begin();
    }

    YieldTimer queueYt(*this, MAX_EXECUTION_QUEUE_DURATION,
                       EXECUTION_QUEUE_BATCH_SIZE);

    // moves any future work into the main execution queue
    mergeExecutionQueue();

    // consumes items in a round robin fashion
    // note that mExecutionIterator stays valid outside of this function as only
    // insertions are performed on mExecutionQueue
    while (mExecutionQueueSize != 0 && queueYt.shouldKeepGoing())
    {
        if (mExecutionIterator == mExecutionQueue.end())
        {
            mExecutionIterator = mExecutionQueue.begin();
        }

        auto& q = mExecutionIterator->second;
        auto elem = std::move(q.front());
        --mExecutionQueueSize;
        q.pop_front();
        if (q.empty())
        {
            mExecutionIterator = mExecutionQueue.erase(mExecutionIterator);
        }
        else
        {
            ++mExecutionIterator;
        }

        elem();
    }
}

void
VirtualClock::mergeExecutionQueue()
{
    while (!mDelayedExecutionQueue.empty())
    {
        auto& d = mDelayedExecutionQueue.front();
        mExecutionQueue[std::move(d.first)].emplace_back(std::move(d.second));
        ++mExecutionQueueSize;
        mDelayedExecutionQueue.pop_front();
    }
}

size_t
VirtualClock::crank(bool block)
{
    if (mDestructing)
    {
        return 0;
    }

    size_t nWorkDone = 0;

    {
        std::lock_guard<std::recursive_mutex> lock(mDelayExecutionMutex);
        // Adding to mDelayedExecutionQueue is now restricted to main thread.

        mDelayExecution = true;

        nRealTimerCancelEvents = 0;

        if (mMode == REAL_TIME)
        {
            // Fire all pending timers.
            // May add work to mDelayedExecutionQueue
            nWorkDone += advanceToNow();
        }

        // Pick up some work off the IO queue.
        // Calling mIOContext.poll() here may introduce unbounded delays
        // to trigger timers.
        size_t lastPoll;
        // to trigger timers so we instead put bounds on the amount of work we
        // do at once both in size and time. As MAX_CRANK_WORK_DURATION is
        // not enforced in virtual mode, we use a smaller batch size.
        // We later process items queued up in the execution queue, that has
        // similar parameters.
        size_t const WORK_BATCH_SIZE = 100;
        std::chrono::milliseconds const MAX_CRANK_WORK_DURATION{200};

        YieldTimer ioYt(*this, MAX_CRANK_WORK_DURATION, WORK_BATCH_SIZE);
        do
        {
            // May add work to mDelayedExecutionQueue.
            lastPoll = mIOContext.poll_one();
            nWorkDone += lastPoll;
        } while (lastPoll != 0 && ioYt.shouldKeepGoing());

        nWorkDone -= nRealTimerCancelEvents;

        if (!mExecutionQueue.empty() || !mDelayedExecutionQueue.empty())
        {
            // If any work is added here, we don't want to advance VIRTUAL_TIME
            // and also we don't need to block, as next crank will have
            // something to execute.
            nWorkDone++;
            advanceExecutionQueue();
        }

        if (mMode == VIRTUAL_TIME && nWorkDone == 0)
        {
            // If we did nothing and we're in virtual mode, we're idle and can
            // skip time forward.
            // May add work to mDelayedExecutionQueue for next crank.
            nWorkDone += advanceToNext();
        }

        mDelayExecution = false;
    }

    // At this point main and background threads can add work to next crank.
    if (block && nWorkDone == 0)
    {
        nWorkDone += mIOContext.run_one();
    }

    noteCrankOccurred(nWorkDone == 0);

    return nWorkDone;
}

void
VirtualClock::postToExecutionQueue(std::function<void()>&& f,
                                   ExecutionCategory&& id)
{
    std::lock_guard<std::recursive_mutex> lock(mDelayExecutionMutex);

    if (!mDelayExecution)
    {
        // Either we are waiting on io_context().run_one, or by some chance
        // run_one was woken up by network activity and postToExecutionQueue was
        // called from a background thread.

        // In any case, all we need to do is ensure that we wake up `crank`, we
        // do this by posting an empty event to the main IO queue
        mDelayExecution = true;
        asio::post(mIOContext, []() {});
    }

    // We currently post all callbacks to a temporary "pre-queue" that's
    // disjoint from anything that will be digested in the current invocation of
    // `advanceExecutionQueue`.
    //
    // This might be surprising and isn't necessarily .. necessary; notably we
    // batch-transfer all those callbacks to the the RR queues at the beginning
    // of the _next_ call to `advanceExecutionQueue` anyway!
    //
    // But this disjoint "pre-queue" behaviour mandates an interleaving of IO
    // with batches of CPU-bound callbacks, which can help avoid the phenomenon
    // of a callback near the end of its timeslice posting back another
    // long-running callback that will then _exceed_ the timeslice. In this
    // scheme, batches of callbacks are more likely to run out of things to do
    // and switch to IO than they are to overshoot their timeslices and yield
    // (possibly somewhat yielding too late and incurring IO debt).
    //
    // This is all somewhat heuristic, but it seems not to do any harm and maybe
    // does good. We may experiment more in the future.
    mDelayedExecutionQueue.emplace_back(
        std::make_pair(std::move(id), std::move(f)));
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
    if (mDestructing)
    {
        return 0;
    }
    assertThreadIsMain();

    // LOG(DEBUG) << "VirtualClock::advanceTo("
    //            << n.time_since_epoch().count() << ")";
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
