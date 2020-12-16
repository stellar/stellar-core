// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "work/Work.h"
#include "util/Logging.h"
#include <Tracy.hpp>
#include <fmt/format.h>

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
    ZoneScoped;
    shutdownChildren();
    BasicWork::shutdown();
}

BasicWork::State
Work::onRun()
{
    ZoneScoped;
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
        CLOG_DEBUG(Work, "Running {}", getName());
        auto state = doWork();
        if (state == State::WORK_SUCCESS)
        {
            assert(allChildrenSuccessful());
            clearChildren();
        }
        else if (state == State::WORK_FAILURE && !allChildrenDone())
        {
            CLOG_DEBUG(Work,
                       "A child of {} failed: aborting remaining children "
                       "before failure.",
                       getName());
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
    ZoneScoped;
    auto child = yieldNextRunningChild();
    if (child)
    {
        assert(child->isAborting());
        child->crankWork();
        return false;
    }
    else
    {
        CLOG_TRACE(Work, "{}: waiting for children to abort.", getName());
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
    ZoneScoped;
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
    ZoneScoped;
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
    return WorkUtils::allSuccessful(mChildren);
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
    return WorkUtils::anyRunning(mChildren);
}

bool
Work::hasChildren() const
{
    return !mChildren.empty();
}

bool
Work::anyChildRaiseFailure() const
{
    return WorkUtils::anyFailed(mChildren);
}

BasicWork::State
Work::checkChildrenStatus() const
{
    return WorkUtils::getWorkStatus(mChildren);
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

bool
allSuccessful(std::list<std::shared_ptr<BasicWork>> const& works)
{
    return std::all_of(
        works.begin(), works.end(), [](std::shared_ptr<BasicWork> w) {
            return w->getState() == BasicWork::State::WORK_SUCCESS;
        });
}

bool
anyFailed(std::list<std::shared_ptr<BasicWork>> const& works)
{
    return std::any_of(
        works.begin(), works.end(), [](std::shared_ptr<BasicWork> w) {
            return w->getState() == BasicWork::State::WORK_FAILURE;
        });
}

bool
anyRunning(std::list<std::shared_ptr<BasicWork>> const& works)
{
    return std::any_of(
        works.begin(), works.end(), [](std::shared_ptr<BasicWork> w) {
            return w->getState() == BasicWork::State::WORK_RUNNING;
        });
}

BasicWork::State
getWorkStatus(std::list<std::shared_ptr<BasicWork>> const& works)
{
    if (allSuccessful(works))
    {
        return BasicWork::State::WORK_SUCCESS;
    }
    else if (anyFailed(works))
    {
        return BasicWork::State::WORK_FAILURE;
    }
    else if (!anyRunning(works))
    {
        return BasicWork::State::WORK_WAITING;
    }

    return BasicWork::State::WORK_RUNNING;
}
}
}
