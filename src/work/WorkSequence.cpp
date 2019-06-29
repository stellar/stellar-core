// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "WorkSequence.h"
#include "util/Logging.h"

namespace stellar
{

WorkSequence::WorkSequence(Application& app, std::string name,
                           std::vector<std::shared_ptr<BasicWork>> sequence,
                           std::vector<ConditionFn> conditions,
                           size_t maxRetries)
    : BasicWork(app, std::move(name), maxRetries)
    , mSequenceOfWork(std::move(sequence))
    , mConditions(std::move(conditions))
    , mNextInSequence(mSequenceOfWork.begin())
{
    if (mConditions.size() != mSequenceOfWork.size() && !mConditions.empty())
    {
        throw std::runtime_error("Invalid arguments: please provide a list of "
                                 "conditions for each work in sequence or an "
                                 "empty list (no conditions)");
    }
}

BasicWork::State
WorkSequence::onRun()
{
    if (mNextInSequence == mSequenceOfWork.end())
    {
        // Completed all the work
        return State::WORK_SUCCESS;
    }

    auto w = *mNextInSequence;
    assert(w);
    if (!mCurrentExecuting)
    {
        bool conditionSatisfied = true;
        try
        {
            // No pre-condition, or condition satisfied
            if (!mConditions.empty())
            {
                auto i =
                    std::distance(mSequenceOfWork.cbegin(), mNextInSequence);
                conditionSatisfied = !mConditions.at(i) || mConditions.at(i)();
            }
        }
        catch (std::exception& e)
        {
            CLOG(ERROR, "Work")
                << "WorkSequence: condition function for " << w->getName()
                << " threw an exception: " << e.what();
            return State::WORK_FAILURE;
        }

        if (conditionSatisfied)
        {
            w->startWork(wakeSelfUpCallback());
            mCurrentExecuting = w;
        }
        else
        {
            // We'll be woken up when condition is satisfied
            return State::WORK_WAITING;
        }
    }
    else
    {
        auto state = w->getState();
        if (state == State::WORK_SUCCESS)
        {
            mCurrentExecuting = nullptr;
            ++mNextInSequence;
            return this->onRun();
        }
        else if (state != State::WORK_RUNNING)
        {
            return state;
        }
    }
    w->crankWork();
    return State::WORK_RUNNING;
}

bool
WorkSequence::onAbort()
{
    if (mCurrentExecuting && !mCurrentExecuting->isDone())
    {
        // Wait for the current work in sequence to finish aborting
        mCurrentExecuting->crankWork();
        return false;
    }
    return true;
}

void
WorkSequence::onReset()
{
    assert(std::all_of(
        mSequenceOfWork.cbegin(), mNextInSequence,
        [](std::shared_ptr<BasicWork> const& w) { return w->isDone(); }));
    mNextInSequence = mSequenceOfWork.begin();
    mCurrentExecuting.reset();
}

std::string
WorkSequence::getStatus() const
{
    if (!isDone() && mNextInSequence != mSequenceOfWork.end())
    {
        return (*mNextInSequence)->getStatus();
    }
    return BasicWork::getStatus();
}

void
WorkSequence::shutdown()
{
    if (mCurrentExecuting)
    {
        mCurrentExecuting->shutdown();
    }

    BasicWork::shutdown();
}
}
