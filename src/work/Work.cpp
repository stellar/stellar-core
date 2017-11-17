// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "work/Work.h"
#include "lib/util/format.h"
#include "main/Application.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "util/make_unique.h"
#include "work/WorkManager.h"
#include "work/WorkParent.h"

#include "medida/meter.h"
#include "medida/metrics_registry.h"

#include <algorithm>

namespace stellar
{

size_t const Work::RETRY_NEVER = 0;
size_t const Work::RETRY_ONCE = 1;
size_t const Work::RETRY_A_FEW = 5;
size_t const Work::RETRY_A_LOT = 32;
size_t const Work::RETRY_FOREVER = 0xffffffff;

Work::Work(Application& app, WorkParent& parent, std::string uniqueName,
           size_t maxRetries)
    : WorkParent(app)
    , mParent(parent.shared_from_this())
    , mUniqueName(uniqueName)
    , mMaxRetries(maxRetries)
{
}

Work::~Work()
{
    clearChildren();
}

std::string
Work::getUniqueName() const
{
    return mUniqueName;
}

std::string
Work::getStatus() const
{
    switch (mState)
    {
    case WORK_PENDING:
    {
        size_t i = 0;
        for (auto const& c : mChildren)
        {
            if (c.second->isDone())
            {
                ++i;
            }
        }
        auto total = mChildren.size();
        return fmt::format("Awaiting {:d}/{:d} prerequisites of: {:s}",
                           total - i, total, getUniqueName());
    }
    case WORK_RUNNING:
        return fmt::format("Running: {:s}", getUniqueName());
    case WORK_SUCCESS:
        return fmt::format("Succeded: {:s}", getUniqueName());
    case WORK_FAILURE_RETRY:
    {
        auto eta = getRetryETA();
        return fmt::format("Retrying in {:d} sec: {:s}", eta, getUniqueName());
    }
    case WORK_FAILURE_RAISE:
    case WORK_FAILURE_FATAL:
        return fmt::format("Failed: {:s}", getUniqueName());
    default:
        assert(false);
        return "";
    }
}

uint64_t
Work::getRetryETA() const
{
    uint64_t now = mApp.timeNow();
    uint64_t retry =
        mRetryTimer ? VirtualClock::to_time_t(mRetryTimer->expiry_time()) : 0;
    return now > retry ? 0 : retry - now;
}

VirtualClock::duration
Work::getRetryDelay() const
{
    // Cap to 4096sec == a little over an hour.
    uint64_t m = 2 << std::min(uint64_t(12), uint64_t(mRetries));
    return std::chrono::seconds(rand_uniform<uint64_t>(1ULL, m));
}

size_t
Work::getMaxRetries() const
{
    return mMaxRetries;
}

std::string
Work::stateName(State st)
{
    switch (st)
    {
    case WORK_PENDING:
        return "WORK_PENDING";
    case WORK_RUNNING:
        return "WORK_RUNNING";
    case WORK_SUCCESS:
        return "WORK_SUCCESS";
    case WORK_FAILURE_RETRY:
        return "WORK_FAILURE_RETRY";
    case WORK_FAILURE_RAISE:
        return "WORK_FAILURE_RAISE";
    case WORK_FAILURE_FATAL:
        return "WORK_FAILURE_FATAL";
    default:
        throw std::runtime_error("Unknown Work::State");
    }
}

std::function<void(asio::error_code const& ec)>
Work::callComplete()
{
    std::weak_ptr<Work> weak(
        std::static_pointer_cast<Work>(shared_from_this()));
    return [weak](asio::error_code const& ec) {
        auto self = weak.lock();
        if (!self)
        {
            return;
        }
        self->complete(ec ? WORK_COMPLETE_FAILURE : WORK_COMPLETE_OK);
    };
}

void
Work::scheduleRun()
{
    if (mScheduled)
    {
        return;
    }

    std::weak_ptr<Work> weak(
        std::static_pointer_cast<Work>(shared_from_this()));
    CLOG(DEBUG, "Work") << "scheduling run of " << getUniqueName();
    mScheduled = true;
    mApp.getClock().getIOService().post([weak]() {
        auto self = weak.lock();
        if (!self)
        {
            return;
        }
        self->mScheduled = false;
        self->run();
    });
}

void
Work::scheduleComplete(CompleteResult result)
{
    if (mScheduled)
    {
        return;
    }

    std::weak_ptr<Work> weak(
        std::static_pointer_cast<Work>(shared_from_this()));
    CLOG(DEBUG, "Work") << "scheduling completion of " << getUniqueName();
    mScheduled = true;
    mApp.getClock().getIOService().post([weak, result]() {
        auto self = weak.lock();
        if (!self)
        {
            return;
        }
        self->mScheduled = false;
        self->complete(result);
    });
}

void
Work::scheduleRetry()
{
    if (mScheduled)
    {
        return;
    }

    if (getState() != WORK_FAILURE_RETRY)
    {
        std::string msg = fmt::format("retrying {} in state {}",
                                      getUniqueName(), stateName(getState()));
        CLOG(ERROR, "Work") << msg;
        throw std::runtime_error(msg);
    }

    if (!mRetryTimer)
    {
        mRetryTimer = make_unique<VirtualTimer>(mApp.getClock());
    }

    std::weak_ptr<Work> weak(
        std::static_pointer_cast<Work>(shared_from_this()));
    auto t = getRetryDelay();
    mRetryTimer->expires_from_now(t);
    CLOG(WARNING, "Work")
        << "Scheduling retry #" << (mRetries + 1) << "/" << mMaxRetries
        << " in " << std::chrono::duration_cast<std::chrono::seconds>(t).count()
        << " sec, for " << getUniqueName();
    mScheduled = true;
    mRetryTimer->async_wait(
        [weak]() {
            auto self = weak.lock();
            if (!self)
            {
                return;
            }
            self->mScheduled = false;
            self->mRetries++;
            self->reset();
            self->advance();
        },
        VirtualTimer::onFailureNoop);
}

void
Work::reset()
{
    CLOG(DEBUG, "Work") << "resetting " << getUniqueName();
    setState(WORK_PENDING);
    onReset();
}

void
Work::advance()
{
    if (getState() != WORK_PENDING)
    {
        return;
    }

    CLOG(DEBUG, "Work") << "advancing " << getUniqueName();
    advanceChildren();
    if (allChildrenSuccessful())
    {
        CLOG(DEBUG, "Work") << "all " << mChildren.size() << " children of "
                            << getUniqueName() << " successful, scheduling run";
        scheduleRun();
    }
    else if (anyChildFatalFailure())
    {
        CLOG(DEBUG, "Work") << "some of " << mChildren.size() << " children of "
                            << getUniqueName() << " fatally failed, scheduling "
                            << "fatal failure";
        scheduleFatalFailure();
    }
    else if (anyChildRaiseFailure())
    {
        CLOG(DEBUG, "Work") << "some of " << mChildren.size() << " children of "
                            << getUniqueName() << " failed, scheduling failure";
        scheduleFailure();
    }
}

void
Work::run()
{
    if (getState() == WORK_PENDING)
    {
        CLOG(DEBUG, "Work") << "starting " << getUniqueName();
        mApp.getMetrics().NewMeter({"work", "unit", "start"}, "unit").Mark();
        onStart();
    }
    CLOG(DEBUG, "Work") << "running " << getUniqueName();
    mApp.getMetrics().NewMeter({"work", "unit", "run"}, "unit").Mark();
    setState(WORK_RUNNING);
    onRun();
}

void
Work::complete(CompleteResult result)
{
    CLOG(DEBUG, "Work") << "completed " << getUniqueName();
    auto& succ =
        mApp.getMetrics().NewMeter({"work", "unit", "success"}, "unit");
    auto& fail =
        mApp.getMetrics().NewMeter({"work", "unit", "failure"}, "unit");

    switch (result)
    {
    case WORK_COMPLETE_OK:
        setState(onSuccess());
        break;
    case WORK_COMPLETE_FAILURE:
        setState(WORK_FAILURE_RETRY);
        break;
    case WORK_COMPLETE_FATAL:
        setState(WORK_FAILURE_FATAL);
        break;
    }

    switch (getState())
    {
    case WORK_SUCCESS:
        succ.Mark();
        CLOG(DEBUG, "Work")
            << "notifying parent of successful " << getUniqueName();
        notifyParent();
        break;

    case WORK_FAILURE_RETRY:
        fail.Mark();
        onFailureRetry();
        scheduleRetry();
        break;

    case WORK_FAILURE_RAISE:
    case WORK_FAILURE_FATAL:
        fail.Mark();
        onFailureRaise();
        CLOG(DEBUG, "Work") << "notifying parent of failed " << getUniqueName();
        notifyParent();
        break;

    case WORK_PENDING:
        succ.Mark();
        advance();
        break;

    case WORK_RUNNING:
        succ.Mark();
        scheduleRun();
        break;

    default:
        assert(false);
        break;
    }
}

void
Work::onReset()
{
}

void
Work::onStart()
{
}

void
Work::onRun()
{
    scheduleSuccess();
}

Work::State
Work::onSuccess()
{
    return WORK_SUCCESS;
}

void
Work::onFailureRetry()
{
}

void
Work::onFailureRaise()
{
}

Work::State
Work::getState() const
{
    return mState;
}

bool
Work::isDone() const
{
    return mState == WORK_SUCCESS || mState == WORK_FAILURE_RAISE ||
           mState == WORK_FAILURE_FATAL;
}

void
Work::setState(Work::State st)
{
    auto maxR = getMaxRetries();
    if (st == WORK_FAILURE_RETRY && (mRetries >= maxR))
    {
        CLOG(WARNING, "Work")
            << "Reached retry limit " << maxR << " for " << getUniqueName();
        st = WORK_FAILURE_RAISE;
    }

    if (st != mState)
    {
        CLOG(DEBUG, "Work") << "work " << getUniqueName() << " : "
                            << stateName(mState) << " -> " << stateName(st);
        mState = st;
    }
}

void
Work::notifyParent()
{
    auto parent = mParent.lock();
    if (parent)
    {
        parent->notify(getUniqueName());
    }
}

void
Work::notify(std::string const& child)
{
    auto i = mChildren.find(child);
    if (i == mChildren.end())
    {
        CLOG(ERROR, "Work") << "work " << getUniqueName()
                            << " notified by unknown child " << child;
    }
    CLOG(DEBUG, "Work") << "notified " << getUniqueName()
                        << " of completed child " << child;
    advance();
}
}
