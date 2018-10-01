// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "Work.h"

#include "Work.h"
#include "lib/util/format.h"
#include "util/Logging.h"

namespace stellar
{
Work::Work(Application& app, std::function<void()> callback, std::string name,
           size_t maxRetries)
    : BasicWork(app, std::move(name), std::move(callback), maxRetries)
    , mNextChild(mChildren.begin())
{
}

Work::~Work()
{
    assert(!hasChildren());
}

BasicWork::State
Work::onRun()
{
    if (mScheduleSelf || !anyChildRunning())
    {
        mScheduleSelf = false;
        CLOG(DEBUG, "Work") << "Running self " << getName();
        return doWork();
    }
    else
    {
        mScheduleSelf = true;
        auto child = yieldNextRunningChild();
        CLOG(DEBUG, "Work") << "Running next child " << child->getName();
        child->crankWork();
        return BasicWork::WORK_RUNNING;
    }
}

void
Work::onReset()
{
    // TODO (mlo) upon proper implementation of WorkScheduler shutdown,
    // this assert needs to move to `clearChildren`
    assert(allChildrenDone());
    clearChildren();
    mNextChild = mChildren.begin();
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
}

void
Work::addChild(std::shared_ptr<Work> child)
{
    // TODO (mlo) potentially check for child duplication

    mChildren.push_back(child);
    child->reset();
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

bool
Work::anyChildFatalFailure() const
{
    for (auto& c : mChildren)
    {
        if (c->getState() == BasicWork::WORK_FAILURE_FATAL)
        {
            return true;
        }
    }
    return false;
}

std::shared_ptr<Work>
Work::yieldNextRunningChild()
{
    // onRun should ensure this function is called properly,
    // that is there's a running child to return (like the name suggests)
    assert(anyChildRunning());

    for (;;)
    {
        mNextChild++;
        if (mNextChild == mChildren.end())
        {
            mNextChild = mChildren.begin();
        }

        if ((*mNextChild)->getState() == BasicWork::WORK_RUNNING)
        {
            return *mNextChild;
        }
    }
}
}