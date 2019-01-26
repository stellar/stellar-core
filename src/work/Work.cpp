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
    // Work is destroyed only if in terminal state, and is properly reset
    assert(!hasChildren());
}

std::string
Work::getStatus() const
{
    auto status = BasicWork::getStatus();
    if (mTotalChildren)
    {
        status += fmt::format(" : {:d}/{:d} children completed", mDoneChildren,
                              mTotalChildren);
    }
    return status;
}

void
Work::shutdown()
{
    shutdownChildren();
    BasicWork::shutdown();
}

BasicWork::State
Work::onRun()
{
    if (mAbortChildrenButNotSelf)
    {
        // Stop whatever work was doing, just wait for children to abort
        return onAbort() ? State::WORK_FAILURE : State::WORK_RUNNING;
    }

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
        else if (state == State::WORK_FAILURE && !allChildrenDone())
        {
            CLOG(DEBUG, "Work")
                << "A child of " << getName()
                << " failed: aborting remaining children before failure.";
            shutdownChildren();
            mAbortChildrenButNotSelf = true;
            return State::WORK_RUNNING;
        }
        return state;
    }
}

bool
Work::onAbort()
{
    auto child = yieldNextRunningChild();
    if (child)
    {
        assert(child->isAborting());
        child->crankWork();
        return false;
    }
    else
    {
        CLOG(TRACE, "Work") << getName() << ": waiting for children to abort.";
        return allChildrenDone();
    }
}

void
Work::onFailureRaise()
{
}

void
Work::onFailureRetry()
{
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
    mAbortChildrenButNotSelf = false;
    doReset();
}

void
Work::doReset()
{
}

void
Work::clearChildren()
{
    assert(allChildrenDone());
    mDoneChildren += mChildren.size();
    mChildren.clear();
    mNextChild = mChildren.begin();
}

void
Work::addChild(std::shared_ptr<BasicWork> child)
{
    bool resetIter = !hasChildren();
    mChildren.push_back(child);
    mTotalChildren += 1;
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
        else if ((*next)->isDone())
        {
            mNextChild = mChildren.erase(next);
            mDoneChildren += 1;
        }
    }

    mNextChild = mChildren.begin();

    return nullptr;
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
