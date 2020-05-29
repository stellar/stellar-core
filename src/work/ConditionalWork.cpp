// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ConditionalWork.h"
#include "util/Logging.h"
#include <Tracy.hpp>
#include <fmt/format.h>

namespace stellar
{

ConditionalWork::ConditionalWork(Application& app, std::string name,
                                 ConditionFn condition,
                                 std::shared_ptr<BasicWork> conditionedWork,
                                 std::chrono::milliseconds sleepTime)
    : BasicWork(app, std::move(name), BasicWork::RETRY_NEVER)
    , mCondition(std::move(condition))
    , mConditionedWork(std::move(conditionedWork))
    , mSleepDelay(sleepTime)
    , mSleepTimer(std::make_unique<VirtualTimer>(app.getClock()))
{
    if (!mConditionedWork)
    {
        throw std::invalid_argument("Must provide valid work");
    }

    if (!mCondition)
    {
        throw std::invalid_argument("Must provide valid condition function. "
                                    "Use BasicWork if condition is not "
                                    "needed.");
    }
}

BasicWork::State
ConditionalWork::onRun()
{
    ZoneScoped;
    if (mWorkStarted)
    {
        mConditionedWork->crankWork();
        return mConditionedWork->getState();
    }

    // Work is not started, so check the condition
    if (!mCondition())
    {
        std::weak_ptr<ConditionalWork> weak(
            std::static_pointer_cast<ConditionalWork>(shared_from_this()));
        auto handler = [weak](asio::error_code const& ec) {
            auto self = weak.lock();
            if (self)
            {
                self->wakeUp();
            }
        };

        CLOG(TRACE, "Work")
            << fmt::format("Condition for {} is not satisfied: sleeping {} ms",
                           getName(), mSleepDelay.count());
        mSleepTimer->expires_from_now(mSleepDelay);
        mSleepTimer->async_wait(handler);
        return State::WORK_WAITING;
    }
    else
    {
        CLOG(TRACE, "Work") << fmt::format(
            "Condition for {} is satisfied: starting work", getName());
        mConditionedWork->startWork(wakeSelfUpCallback());
        mWorkStarted = true;
        mCondition = nullptr;
        return this->onRun();
    }
}

void
ConditionalWork::shutdown()
{
    ZoneScoped;
    if (mWorkStarted)
    {
        mConditionedWork->shutdown();
    }
    BasicWork::shutdown();
}

bool
ConditionalWork::onAbort()
{
    ZoneScoped;
    if (mWorkStarted && !mConditionedWork->isDone())
    {
        mConditionedWork->crankWork();
        return false;
    }
    return true;
}

void
ConditionalWork::onReset()
{
    mWorkStarted = false;
}

std::string
ConditionalWork::getStatus() const
{
    return fmt::format("{}{}", mWorkStarted ? "" : "Waiting before starting ",
                       mConditionedWork->getStatus());
}
}
