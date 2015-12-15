// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Application.h"
#include "work/Work.h"
#include "work/WorkParent.h"
#include "lib/util/format.h"
#include "util/Logging.h"
#include "util/make_unique.h"
#include "util/Math.h"

#include "medida/meter.h"
#include "medida/metrics_registry.h"

#include <algorithm>

namespace stellar
{

Work::Work(Application& app,
           WorkParent& parent,
           std::string uniqueName,
           size_t maxRetries)
    : WorkParent(app)
    , mParent(parent)
    , mUniqueName(uniqueName)
    , mMaxRetries(maxRetries)
{
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
        return fmt::format("Awaiting {:d}/{:d} prerequisites of: {:s}",
                           i, mChildren.size(), getUniqueName());
    }
    case WORK_RUNNING:
        return fmt::format("Running: {:s}", getUniqueName());
    case WORK_SUCCESS:
        return fmt::format("Succeded: {:s}", getUniqueName());
    case WORK_FAILURE_RETRY:
    {
        uint64 now = mApp.timeNow();
        uint64 retry =
            mRetryTimer ?
            VirtualClock::to_time_t(mRetryTimer->expiry_time()) :
            0;
        uint64 eta = now > retry ? 0 : retry - now;
        return fmt::format("Retrying in {:d} sec: {:s}", eta);
    }
    case WORK_FAILURE_RAISE:
        return fmt::format("Failed: {:s}", getUniqueName());
    default:
        assert(false);
        return "";
    }
}

VirtualClock::duration
Work::getRetryDelay() const
{
    uint64_t m = 2 << std::min(uint64_t(62), uint64_t(mRetries));
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
    default:
        throw std::runtime_error("Unknown Work::State");
    }
}

std::function<void(asio::error_code const& ec)>
Work::callComplete()
{
    std::weak_ptr<Work> weak(shared_from_this());
    return [weak](asio::error_code const& ec)
    {
        auto self = weak.lock();
        if (!self)
        {
            return;
        }
        self->complete(ec);
    };
}

void
Work::scheduleAdvance()
{
    if (mCallbackPending)
    {
        return;
    }
    mCallbackPending = true;
    std::weak_ptr<Work> weak(shared_from_this());
    CLOG(DEBUG, "Work") << "scheduling advance of " << getUniqueName();
    mApp.getClock().getIOService().post(
        [weak]()
        {
            auto self = weak.lock();
            if (!self)
            {
                return;
            }
            self->mCallbackPending = false;
            self->advance();
        });
}

void
Work::scheduleRun()
{
    if (mCallbackPending)
    {
        return;
    }
    mCallbackPending = true;
    std::weak_ptr<Work> weak(shared_from_this());
    CLOG(DEBUG, "Work") << "scheduling run of " << getUniqueName();
    mApp.getClock().getIOService().post(
        [weak]()
        {
            auto self = weak.lock();
            if (!self)
            {
                return;
            }
            self->mCallbackPending = false;
            self->run();
        });
}

void
Work::scheduleComplete(asio::error_code ec)
{
    if (mCallbackPending)
    {
        return;
    }
    mCallbackPending = true;
    std::weak_ptr<Work> weak(shared_from_this());
    CLOG(DEBUG, "Work") << "scheduling completion of " << getUniqueName();
    mApp.getClock().getIOService().post(
        [weak, ec]()
    {
        auto self = weak.lock();
        if (!self)
        {
            return;
        }
        self->mCallbackPending = false;
        self->complete(ec);
    });
}

void
Work::scheduleRetry()
{
    if (getState() != WORK_FAILURE_RETRY)
    {
        std::string msg = fmt::format(
            "retrying {} in state {}",
            getUniqueName(), stateName(getState()));
        CLOG(ERROR, "Work") << msg;
        throw std::runtime_error(msg);
    }

    if (mCallbackPending)
    {
        return;
    }

    mCallbackPending = true;

    if (!mRetryTimer)
    {
        mRetryTimer = make_unique<VirtualTimer>(mApp.getClock());
    }

    std::weak_ptr<Work> weak(shared_from_this());
    auto t = getRetryDelay();
    mRetryTimer->expires_from_now(getRetryDelay());
    CLOG(WARNING, "Work")
        << "Scheduling retry #" << (mRetries + 1)
        << "/" << mMaxRetries << " in "
        << std::chrono::duration_cast<std::chrono::seconds>(t).count()
        << " sec, for " << getUniqueName();
    mRetryTimer->async_wait([weak]()
    {
        auto self = weak.lock();
        if (!self)
        {
            return;
        }
        self->mCallbackPending = false;
        self->reset();
        self->mRetries++;
        self->scheduleAdvance();
    }, VirtualTimer::onFailureNoop);
}


void
Work::reset()
{
    CLOG(DEBUG, "Work") << "resetting " << getUniqueName();
    setState(WORK_PENDING);
    this->onReset();
}

void
Work::advance()
{
    CLOG(DEBUG, "Work") << "advancing " << getUniqueName();
    advanceChildren();
    if (getState() == WORK_PENDING
        && allChildrenSuccessful())
    {
        scheduleRun();
    }
    else if (anyChildRaiseFailure())
    {
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
        this->onStart();
    }
    CLOG(DEBUG, "Work") << "running " << getUniqueName();
    mApp.getMetrics().NewMeter({"work", "unit", "run"}, "unit").Mark();
    setState(WORK_RUNNING);
    this->onRun();
}

void
Work::complete(asio::error_code const& ec)
{
    CLOG(DEBUG, "Work") << "completed " << getUniqueName();
    auto& succ = mApp.getMetrics().NewMeter({"work", "unit", "success"}, "unit");
    auto& fail = mApp.getMetrics().NewMeter({"work", "unit", "failure"}, "unit");

    if (ec)
    {
        setState(WORK_FAILURE_RETRY);
    }
    else
    {
        setState(this->onSuccess());
    }

    switch (getState())
    {
    case WORK_SUCCESS:
        succ.Mark();
        notifyParent();
        break;

    case WORK_FAILURE_RETRY:
        fail.Mark();
        this->onFailureRetry();
        scheduleRetry();
        break;

    case WORK_FAILURE_RAISE:
        fail.Mark();
        this->onFailureRaise();
        notifyParent();
        break;

    case WORK_PENDING:
        succ.Mark();
        scheduleAdvance();
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
     return mState == WORK_SUCCESS || mState == WORK_FAILURE_RAISE;
 }


void
Work::setState(Work::State st)
{
    auto maxR = this->getMaxRetries();
    if (st == WORK_FAILURE_RETRY && (mRetries >= maxR))
    {
        CLOG(WARNING, "Work")
            << "Reached retry limit " << maxR
            << " for " << getUniqueName();
        st = WORK_FAILURE_RAISE;
    }

    if (st != mState)
    {
        CLOG(DEBUG, "Work")
            << "work " << getUniqueName() << " : "
            << stateName(mState)
            << " -> "
            << stateName(st);
        mState = st;
    }
}

void
Work::notifyParent()
{
    mParent.notify(getUniqueName());
}

void
Work::notify(std::string const& child)
{
    auto i = mChildren.find(child);
    if (i == mChildren.end())
    {
        CLOG(ERROR, "Work") << "work " << getUniqueName()
                              << " notified by unknown child "
                              << child;
    }
    CLOG(DEBUG, "Work") << "notified " << getUniqueName()
                        << " of completed child "
                        << child;
    scheduleAdvance();
}

}
