// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "work/Work.h"
#include "lib/util/format.h"
#include "util/Logging.h"

namespace stellar
{

Work::Work(Application& app, std::string name, size_t maxRetries)
    : BasicWork(app, std::move(name), maxRetries), mNextChild(mChildren.begin())
{
}

Work::~Work()
{
    // FIXME consider this assert when abort logic is implemented
    // assert(!hasChildren());
}

std::string
Work::getStatus() const
{
    if (hasChildren())
    {
        auto incomplete = 0;
        for (auto const& c : mChildren)
        {
            if (!c->isDone())
            {
                ++incomplete;
            }
        }
        return fmt::format("Work {:s} has incomplete children: {:d}/{:d}",
                           getName(), incomplete, mChildren.size());
    }
    return BasicWork::getStatus();
}

void
Work::shutdown()
{
    for (auto const& c : mChildren)
    {
        c->shutdown();
    }
    BasicWork::shutdown();
}

BasicWork::State
Work::onRun()
{
    auto child = yieldNextRunningChild();
    if (child)
    {
        child->crankWork();
        return State::WORK_RUNNING;
    }
    else
    {
        CLOG(DEBUG, "Work") << "Running " << getName();
        auto state = doWork();
        if (state == State::WORK_SUCCESS)
        {
            clearChildren();
        }
        return state;
    }
}

void
Work::onReset()
{
    clearChildren();
    doReset();
}

void
Work::doReset()
{
}

void
Work::clearChildren()
{
    // FIXME upon proper implementation of WorkScheduler shutdown, consider this
    // assert assert(allChildrenDone());
    mChildren.clear();
    mNextChild = mChildren.begin();
}

void
Work::addChild(std::shared_ptr<BasicWork> child)
{
    bool resetIter = !hasChildren();
    mChildren.push_back(child);

    if (resetIter)
    {
        mNextChild = mChildren.begin();
    }
}

bool
Work::allChildrenSuccessful() const
{
    return std::all_of(mChildren.begin(), mChildren.end(),
                       [](std::shared_ptr<BasicWork> const& w) {
                           return w->getState() ==
                                  BasicWork::State::WORK_SUCCESS;
                       });
}

bool
Work::allChildrenDone() const
{
    return std::all_of(
        mChildren.begin(), mChildren.end(),
        [](std::shared_ptr<BasicWork> const& w) { return w->isDone(); });
}

bool
Work::anyChildRunning() const
{
    return std::any_of(mChildren.begin(), mChildren.end(),
                       [](std::shared_ptr<BasicWork> const& w) {
                           return w->getState() ==
                                  BasicWork::State::WORK_RUNNING;
                       });
}

bool
Work::hasChildren() const
{
    return !mChildren.empty();
}

bool
Work::anyChildRaiseFailure() const
{
    return std::any_of(mChildren.begin(), mChildren.end(),
                       [](std::shared_ptr<BasicWork> const& w) {
                           return w->getState() ==
                                  BasicWork::State::WORK_FAILURE;
                       });
}

std::shared_ptr<BasicWork>
Work::yieldNextRunningChild()
{
    while (mNextChild != mChildren.end())
    {
        auto next = mNextChild;
        mNextChild++;
        assert(*next);
        auto state = (*next)->getState();
        if (state == State::WORK_RUNNING)
        {
            return *next;
        }
        else if (state == State::WORK_SUCCESS)
        {
            mNextChild = mChildren.erase(next);
        }
    }

    mNextChild = mChildren.begin();

    return nullptr;
}

WorkSequence::WorkSequence(Application& app, std::string name,
                           std::vector<std::shared_ptr<BasicWork>> sequence,
                           size_t maxRetries)
    : Work(app, std::move(name), maxRetries)
    , mSequenceOfWork(sequence)
    , mNextInSequence(mSequenceOfWork.begin())
{
}

BasicWork::State
WorkSequence::doWork()
{
    if (mNextInSequence == mSequenceOfWork.end())
    {
        // Completed all the work
        assert(!hasChildren());
        return State::WORK_SUCCESS;
    }

    auto w = *mNextInSequence;
    assert(w);
    if (!hasChildren())
    {
        w->startWork(wakeUpCallback());
        addChild(w);
        return State::WORK_RUNNING;
    }
    else
    {
        auto state = w->getState();
        if (state == State::WORK_SUCCESS)
        {
            clearChildren();
            mNextInSequence++;
            return State::WORK_RUNNING;
        }
        return state;
    }
}

void
WorkSequence::doReset()
{
    assert(std::all_of(
        mSequenceOfWork.cbegin(), mNextInSequence,
        [](std::shared_ptr<BasicWork> const& w) { return w->isDone(); }));
    mNextInSequence = mSequenceOfWork.begin();
}

std::string
WorkSequence::getStatus() const
{
    if (!isDone() && mNextInSequence != mSequenceOfWork.end())
    {
        return (*mNextInSequence)->getStatus();
    }
    return Work::getStatus();
}
}
