// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "work/Work.h"
#include "lib/util/format.h"
#include "util/Logging.h"

namespace stellar
{

size_t const Work::RETRY_NEVER = 0;
size_t const Work::RETRY_ONCE = 1;
size_t const Work::RETRY_A_FEW = 5;
size_t const Work::RETRY_A_LOT = 32;
size_t const Work::RETRY_FOREVER = 0xffffffff;

Work::Work(Application& app, std::string name,
           size_t maxRetries)
    : BasicWork(app, std::move(name), maxRetries)
    , mNextChild(mChildren.begin())
{
}

Work::~Work()
{
    // TODO consider this assert when abort logic is implemented
    //    assert(!hasChildren());
}

void
Work::shutdown()
{
    for (auto& c : mChildren)
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
        CLOG(DEBUG, "Work") << "Running next child " << child->getName();
        child->crankWork();
        return BasicWork::WORK_RUNNING;
    }
    else
    {
        CLOG(DEBUG, "Work") << "Running self " << getName();
        return doWork();
    }
}

void
Work::onReset()
{
    // TODO upon proper implementation of WorkScheduler shutdown,
    // this assert needs to move to `clearChildren`
    //    assert(allChildrenDone());
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
    mChildren.clear();
    mNextChild = mChildren.begin();
}

void
Work::addChild(std::shared_ptr<BasicWork> child)
{
    // TODO (mlo) potentially check for child duplication

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
    for (auto& c : mChildren)
    {
        if (c->getState() != BasicWork::WORK_SUCCESS)
        {
            return false;
        }
    }
    return true;
}

bool
Work::allChildrenDone() const
{
    for (auto& c : mChildren)
    {
        if (!c->isDone())
        {
            return false;
        }
    }
    return true;
}

bool
Work::anyChildRunning() const
{
    for (auto& c : mChildren)
    {
        if (c->getState() == BasicWork::WORK_RUNNING)
        {
            return true;
        }
    }
    return false;
}

bool
Work::hasChildren() const
{
    return !mChildren.empty();
}

bool
Work::anyChildRaiseFailure() const
{
    for (auto& c : mChildren)
    {
        if (c->getState() == BasicWork::WORK_FAILURE_RAISE)
        {
            return true;
        }
    }
    return false;
}

std::shared_ptr<BasicWork>
Work::yieldNextRunningChild()
{
    while (mNextChild != mChildren.end())
    {
        auto next = *mNextChild;
        mNextChild++;
        if (next->getState() == BasicWork::WORK_RUNNING)
        {
            return next;
        }
    }
    mNextChild = mChildren.begin();
    return nullptr;
}

WorkSequence::WorkSequence(Application& app, std::string name,
                           std::vector<std::shared_ptr<BasicWork>> sequence)
        : Work(app, std::move(name), RETRY_A_FEW)
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
        return WORK_SUCCESS;
    }

    auto w = *mNextInSequence;
    assert(w);
    if (!hasChildren())
    {
        w->restartWork(wakeUpCallback());
        addChild(w);
        return WORK_RUNNING;
    }
    else
    {
        auto state = w->getState();
        if (state == WORK_SUCCESS)
        {
            clearChildren();
            mNextInSequence++;
            return WORK_RUNNING;
        }
        else if (state == WORK_FAILURE_RAISE)
        {
            // Work will do all the cleanup
            return WORK_FAILURE_RETRY;
        }
        return state;
    }
}

void
WorkSequence::doReset()
{
    mNextInSequence = mSequenceOfWork.begin();
}
}