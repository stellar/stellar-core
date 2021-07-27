// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "WorkSequence.h"
#include "util/GlobalChecks.h"
#include "work/Work.h"
#include <Tracy.hpp>

namespace stellar
{

WorkSequence::WorkSequence(Application& app, std::string name,
                           std::vector<std::shared_ptr<BasicWork>> sequence,
                           size_t maxRetries, bool stopAtFirstFailure)
    : BasicWork(app, std::move(name), maxRetries)
    , mSequenceOfWork(std::move(sequence))
    , mNextInSequence(mSequenceOfWork.begin())
    , mStopAtFirstFailure(stopAtFirstFailure)
{
}

BasicWork::State
WorkSequence::onRun()
{
    ZoneScoped;
    if (mNextInSequence == mSequenceOfWork.end())
    {
        // Completed the work.
        return WorkUtils::getWorkStatus(
            std::list(mSequenceOfWork.begin(), mSequenceOfWork.end()));
    }

    auto w = *mNextInSequence;
    releaseAssert(w);
    if (!mCurrentExecuting)
    {
        w->startWork(wakeSelfUpCallback());
        mCurrentExecuting = w;
    }
    else
    {
        auto state = w->getState();
        if (state == State::WORK_SUCCESS ||
            (state == State::WORK_FAILURE && !mStopAtFirstFailure))
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
    ZoneScoped;
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
    ZoneScoped;
    releaseAssert(std::all_of(
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
    ZoneScoped;
    if (mCurrentExecuting)
    {
        mCurrentExecuting->shutdown();
    }

    BasicWork::shutdown();
}
}
