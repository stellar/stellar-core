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

std::set<BasicWork::Transition> const BasicWork::ALLOWED_TRANSITIONS = {
    Transition(InternalState::PENDING, InternalState::RUNNING),
    Transition(InternalState::PENDING, InternalState::ABORTING),
    Transition(InternalState::RUNNING, InternalState::RUNNING),
    Transition(InternalState::RUNNING, InternalState::WAITING),
    Transition(InternalState::RUNNING, InternalState::SUCCESS),
    Transition(InternalState::RUNNING, InternalState::FAILURE),
    Transition(InternalState::RUNNING, InternalState::RETRYING),
    Transition(InternalState::RUNNING, InternalState::ABORTING),
    Transition(InternalState::WAITING, InternalState::RUNNING),
    Transition(InternalState::WAITING, InternalState::ABORTING),
    Transition(InternalState::RETRYING, InternalState::WAITING),
    Transition(InternalState::SUCCESS, InternalState::PENDING),
    Transition(InternalState::FAILURE, InternalState::PENDING),
    Transition(InternalState::ABORTING, InternalState::ABORTING),
    Transition(InternalState::ABORTING, InternalState::ABORTED),
    Transition(InternalState::ABORTED, InternalState::PENDING),
};

BasicWork::BasicWork(Application& app, std::string name, size_t maxRetries)
    : mApp(app), mName(std::move(name)), mMaxRetries(maxRetries)
{
}

BasicWork::~BasicWork()
{
    // Work completed or has not started yet
    assert(isDone() || mState == InternalState::PENDING);
}

void
BasicWork::shutdown()
{
    CLOG(TRACE, "Work") << "Shutting down: " << getName();
    if (!isDone())
    {
        setState(InternalState::ABORTING);
    }
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
    auto state = mRetryTimer ? InternalState::RETRYING : mState;

    switch (state)
    {
    case InternalState::PENDING:
        return fmt::format("Ready to run: {:s}", getName());
    case InternalState::RUNNING:
        return fmt::format("Running: {:s}", getName());
    case InternalState::WAITING:
        return fmt::format("Waiting: {:s}", getName());
    case InternalState::SUCCESS:
        return fmt::format("Succeeded: {:s}", getName());
    case InternalState::RETRYING:
    {
        auto eta = getRetryETA();
        return fmt::format("Retrying in {:d} sec: {:s}", eta, getName());
    }
    case InternalState::FAILURE:
        return fmt::format("Failed: {:s}", getName());
    case InternalState::ABORTING:
        return fmt::format("Aborting: {:s}", getName());
    case InternalState::ABORTED:
        return fmt::format("Aborted: {:s}", getName());
    default:
        abort();
    }
}

bool
BasicWork::isDone() const
{
    return mState == InternalState::SUCCESS ||
           mState == InternalState::FAILURE || mState == InternalState::ABORTED;
}

std::string
BasicWork::stateName(InternalState st)
{
    switch (st)
    {
    case InternalState::WAITING:
        return "WORK_WAITING";
    case InternalState::RUNNING:
        return "WORK_RUNNING";
    case InternalState::SUCCESS:
        return "WORK_SUCCESS";
    case InternalState::FAILURE:
        return "WORK_FAILURE";
    case InternalState::PENDING:
        return "WORK_PENDING";
    case InternalState::ABORTING:
        return "WORK_ABORTING";
    case InternalState::ABORTED:
        return "WORK_ABORTED";
    case InternalState::RETRYING:
        return "WORK_RETRYING";
    default:
        abort();
    }
}

void
BasicWork::reset()
{
    CLOG(TRACE, "Work") << "resetting " << getName();

    if (mRetryTimer)
    {
        mRetryTimer->cancel();
        mRetryTimer.reset();
    }

    onReset();
}

void
BasicWork::startWork(std::function<void()> notificationCallback)
{
    CLOG(TRACE, "Work") << "Starting " << getName();

    if (mState != InternalState::PENDING)
    {
        // Only restart if work is in terminal state
        setState(InternalState::PENDING);
    }

    mNotifyCallback = notificationCallback;
    setState(InternalState::RUNNING);
    assert(mRetries == 0);
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
    CLOG(DEBUG, "Work")
        << "Scheduling retry #" << (mRetries + 1) << "/" << mMaxRetries
        << " in " << std::chrono::duration_cast<std::chrono::seconds>(t).count()
        << " sec, for " << getName();
    setState(InternalState::WAITING);
    mRetryTimer->async_wait([weak](asio::error_code const& ec) {
        auto self = weak.lock();
        if (!self)
        {
            return;
        }
        assert(self->mState == InternalState::WAITING ||
               self->mState == InternalState::ABORTED ||
               self->mState == InternalState::ABORTING);
        if (self->mState == InternalState::WAITING)
        {
            self->mRetries++;
            self->mRetryTimer.reset();
            self->wakeUp();
        }
    });
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

BasicWork::State
BasicWork::getState() const
{
    switch (mState)
    {
    case InternalState::RUNNING:
    case InternalState::PENDING:
    case InternalState::ABORTING:
        return State::WORK_RUNNING;
    case InternalState::WAITING:
    case InternalState::RETRYING:
        return State::WORK_WAITING;
    case InternalState::SUCCESS:
        return State::WORK_SUCCESS;
    case InternalState::FAILURE:
        return State::WORK_FAILURE;
    case InternalState::ABORTED:
        return State::WORK_ABORTED;
    default:
        abort();
    }
}

void
BasicWork::setState(InternalState st)
{
    if (st == InternalState::FAILURE && (mRetries < mMaxRetries))
    {
        st = InternalState::RETRYING;
    }

    assertValidTransition(Transition(mState, st));
    if (mState == InternalState::PENDING && st == InternalState::RUNNING)
    {
        reset();
        mRetries = 0;
    }

    // Perform necessary action *before* changing state (in case shutdown was
    // issued in between)
    switch (st)
    {
    case InternalState::SUCCESS:
        onSuccess();
        break;
    case InternalState::FAILURE:
        onFailureRaise();
        reset();
        break;
    case InternalState::RETRYING:
        onFailureRetry();
        reset();
        break;
    case InternalState::ABORTED:
        reset();
        break;
    default:
        break;
    }

    if (mState != st)
    {
        CLOG(DEBUG, "Work") << "work " << getName() << " : "
                            << stateName(mState) << " -> " << stateName(st);
        mState = st;
    }

    if (mState == InternalState::RETRYING)
    {
        waitForRetry();
    }
}

void
BasicWork::wakeUp(std::function<void()> innerCallback)
{
    // Work should not be waking up in terminal state
    // Work should not be interrupted when retrying or destructing
    if (mState != InternalState::WAITING)
    {
        return;
    }

    CLOG(TRACE, "Work") << "Waking up: " << getName();
    setState(InternalState::RUNNING);

    if (innerCallback)
    {
        CLOG(TRACE, "Work")
            << getName() << " woke up and is executing its callback";
        innerCallback();
    }

    if (mNotifyCallback)
    {
        mNotifyCallback();
    }
}

std::function<void()>
BasicWork::wakeSelfUpCallback(std::function<void()> innerCallback)
{
    std::weak_ptr<BasicWork> weak = shared_from_this();
    auto callback = [weak, innerCallback]() {
        auto self = weak.lock();
        if (!self)
        {
            return;
        }

        self->wakeUp(innerCallback);
    };
    return callback;
}

void
BasicWork::crankWork()
{
    assert(!isDone() && mState != InternalState::WAITING);

    InternalState nextState;
    if (mState == InternalState::ABORTING)
    {
        auto doneAborting = onAbort();
        nextState =
            doneAborting ? InternalState::ABORTED : InternalState::ABORTING;
        CLOG(TRACE, "Work") << "Abort progress for " << getName()
                            << (doneAborting ? ": done" : ": still aborting");
    }
    else
    {
        nextState = getInternalState(onRun());
    }
    setState(nextState);
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

void
BasicWork::assertValidTransition(Transition const& t) const
{
    if (ALLOWED_TRANSITIONS.find(t) == ALLOWED_TRANSITIONS.end())
    {
        throw std::runtime_error(
            fmt::format("BasicWork error: illegal state transition {} -> {}",
                        stateName(t.first), stateName(t.second)));
    }
}

BasicWork::InternalState
BasicWork::getInternalState(State s) const
{
    switch (s)
    {
    case State::WORK_SUCCESS:
        return InternalState::SUCCESS;
    case State::WORK_FAILURE:
        return InternalState::FAILURE;
    case State::WORK_WAITING:
        return InternalState::WAITING;
    case State::WORK_RUNNING:
        return InternalState::RUNNING;
    case State::WORK_ABORTED:
        return InternalState::ABORTED;
    default:
        abort();
    }
}
}
