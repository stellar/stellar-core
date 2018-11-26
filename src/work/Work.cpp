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
    assert(!hasChildren());
}

std::string
Work::getStatus() const
{
    auto status = BasicWork::getStatus();
    if (hasChildren())
    {
        size_t complete = 0;
        for (auto const& c : mChildren)
        {
            if (c->isDone())
            {
                ++complete;
            }
        }
        status += fmt::format(" : {:d}/{:d} children completed", complete,
                              mChildren.size());
    }
    return status;
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
Work::onFailureRaise()
{
    shutdownChildren();
}

void
Work::onFailureRetry()
{
    shutdownChildren();
}

void
Work::shutdownChildren()
{
    // Shutdown any children that are still running
    for (auto const& c : mChildren)
    {
        if (!c->isDone())
        {
            c->shutdown();
        }
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
    // TODO (mlo) consider this assert when abort logic is implemented
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
        if ((*next)->getState() == State::WORK_RUNNING)
        {
            return *next;
        }
        else if ((*next)->isDone())
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
    : BasicWork(app, std::move(name), maxRetries)
    , mSequenceOfWork(sequence)
    , mNextInSequence(mSequenceOfWork.begin())
{
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
        w->startWork(wakeSelfUpCallback());
        mCurrentExecuting = w;
    }
    else
    {
        auto state = w->getState();
        if (state == State::WORK_SUCCESS)
        {
            mCurrentExecuting = nullptr;
            mNextInSequence++;
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

namespace WorkUtils
{
BasicWork::State
checkChildrenStatus(Work const& w)
{
    if (w.allChildrenSuccessful())
    {
        return BasicWork::State::WORK_SUCCESS;
    }
    else if (w.anyChildRaiseFailure())
    {
        return BasicWork::State::WORK_FAILURE;
    }
    else if (!w.anyChildRunning())
    {
        return BasicWork::State::WORK_WAITING;
    }

    return BasicWork::State::WORK_RUNNING;
}
}
}
