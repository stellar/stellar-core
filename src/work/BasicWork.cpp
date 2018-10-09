// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "work/BasicWork.h"
#include "lib/util/format.h"
#include "util/Logging.h"
#include "util/Math.h"

namespace stellar
{

size_t const BasicWork::RETRY_NEVER = 0;
size_t const BasicWork::RETRY_ONCE = 1;
size_t const BasicWork::RETRY_A_FEW = 5;
size_t const BasicWork::RETRY_A_LOT = 32;
size_t const BasicWork::RETRY_FOREVER = 0xffffffff;

BasicWork::BasicWork(Application& app, std::function<void()> callback,
                     std::string name, size_t maxRetries)
    : mApp(app)
    , mNotifyCallback(std::move(callback))
    , mName(std::move(name))
    , mMaxRetries(maxRetries)
{
}

BasicWork::~BasicWork()
{
    mState = WORK_DESTRUCTING;
}

std::string const&
BasicWork::getName() const
{
    return mName;
}

std::string
BasicWork::getStatus() const
{
    // Work is in `WAITING` state when retrying
    auto state = mRetryTimer ? WORK_FAILURE_RETRY : mState;

    switch (state)
    {
    case WORK_RUNNING:
        return fmt::format("Running: {:s}", getName());
    case WORK_WAITING:
        return fmt::format("Waiting: {:s}", getName());
    case WORK_SUCCESS:
        return fmt::format("Succeeded: {:s}", getName());
    case WORK_FAILURE_RETRY:
    {
        auto eta = getRetryETA();
        return fmt::format("Retrying in {:d} sec: {:s}", eta, getName());
    }
    case WORK_FAILURE_RAISE:
        return fmt::format("Failed: {:s}", getName());
    default:
        assert(false);
        return "";
    }
}

bool
BasicWork::isDone() const
{
    return mState == WORK_SUCCESS || mState == WORK_FAILURE_RAISE;
}

std::string
BasicWork::stateName(State st)
{
    switch (st)
    {
    case WORK_WAITING:
        return "WORK_WAITING";
    case WORK_RUNNING:
        return "WORK_RUNNING";
    case WORK_SUCCESS:
        return "WORK_SUCCESS";
    case WORK_FAILURE_RETRY:
        return "WORK_FAILURE_RETRY";
    case WORK_FAILURE_RAISE:
        return "WORK_FAILURE_RAISE";
    default:
        throw std::runtime_error("Unknown Work::State");
    }
}

void
BasicWork::reset()
{
    CLOG(DEBUG, "Work") << "resetting " << getName();
    setState(WORK_RUNNING);
    onReset();
}

std::function<void()>
BasicWork::wakeUpCallback()
{
    std::weak_ptr<BasicWork> weak = shared_from_this();
    auto callback = [weak]() {
        auto self = weak.lock();
        if (self)
        {
            self->wakeUp();
        }
    };
    return callback;
}

void
BasicWork::waitForRetry()
{
    if (mRetryTimer)
    {
        throw std::runtime_error(
            fmt::format("Retry timer for {} already exists!", getName()));
    }

    mRetryTimer = std::make_unique<VirtualTimer>(mApp.getClock());
    std::weak_ptr<BasicWork> weak = shared_from_this();
    auto t = getRetryDelay();
    mRetryTimer->expires_from_now(t);
    CLOG(WARNING, "Work")
        << "Scheduling retry #" << (mRetries + 1) << "/" << mMaxRetries
        << " in " << std::chrono::duration_cast<std::chrono::seconds>(t).count()
        << " sec, for " << getName();
    setState(WORK_WAITING);
    mRetryTimer->async_wait(
        [weak]() {
            auto self = weak.lock();
            if (!self)
            {
                return;
            }
            self->mRetries++;
            self->mRetryTimer = nullptr;
            self->wakeUp();
            self->reset();
        },
        VirtualTimer::onFailureNoop);
}

void
BasicWork::onReset()
{
}

void
BasicWork::onSuccess()
{
}

void
BasicWork::onFailureRetry()
{
}

void
BasicWork::onFailureRaise()
{
}

void
BasicWork::onWakeUp()
{
}

BasicWork::State
BasicWork::getState() const
{
    return mState;
}

void
BasicWork::setState(BasicWork::State st)
{
    auto maxR = getMaxRetries();
    if (st == WORK_FAILURE_RETRY && (mRetries >= maxR))
    {
        CLOG(WARNING, "Work")
            << "Reached retry limit " << maxR << " for " << getName();
        st = WORK_FAILURE_RAISE;
    }

    if (st != mState)
    {
        CLOG(DEBUG, "Work") << "work " << getName() << " : "
                            << stateName(mState) << " -> " << stateName(st);
        mState = st;
    }
}

void
BasicWork::wakeUp()
{
    if (mState == WORK_RUNNING || mState == WORK_DESTRUCTING)
    {
        return;
    }

    setState(WORK_RUNNING);
    onWakeUp();
    if (mNotifyCallback)
    {
        mNotifyCallback();
    }
}

void
BasicWork::crankWork()
{
    assert(!isDone() && mState != WORK_WAITING);

    auto nextState = onRun();
    setState(nextState);

    switch (mState)
    {
    case WORK_SUCCESS:
        onSuccess();
        onReset();
        break;
    case WORK_FAILURE_RAISE:
        onFailureRaise();
        onReset();
        break;
    case WORK_FAILURE_RETRY:
        onFailureRetry();
        waitForRetry();
        break;
    default:
        break;
    }
}

size_t
BasicWork::getMaxRetries() const
{
    return mMaxRetries;
}

VirtualClock::duration
BasicWork::getRetryDelay() const
{
    // Cap to 4096sec == a little over an hour.
    uint64_t m = 2 << std::min(uint64_t(12), uint64_t(mRetries));
    return std::chrono::seconds(rand_uniform<uint64_t>(1ULL, m));
}

uint64_t
BasicWork::getRetryETA() const
{
    uint64_t now = mApp.timeNow();
    uint64_t retry =
        mRetryTimer ? VirtualClock::to_time_t(mRetryTimer->expiry_time()) : 0;
    return now > retry ? 0 : retry - now;
}
}