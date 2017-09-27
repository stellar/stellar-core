// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "work/WorkParent.h"
#include "work/Work.h"

#include "lib/util/format.h"
#include "util/Logging.h"
#include "util/make_unique.h"

namespace stellar
{

WorkParent::WorkParent(Application& app) : mApp(app)
{
}

WorkParent::~WorkParent()
{
}

void
WorkParent::addChild(std::shared_ptr<Work> child)
{
    auto name = child->getUniqueName();
    if (mChildren.find(name) != mChildren.end())
    {
        std::string msg = fmt::format("duplicate child work: {} ", name);
        CLOG(ERROR, "Work") << msg;
        throw std::runtime_error(msg);
    }
    mChildren.insert(std::make_pair(name, child));
    child->reset();
}

void
WorkParent::clearChildren()
{
    mChildren.clear();
}

void
WorkParent::advanceChildren()
{
    for (auto& c : mChildren)
    {
        c.second->advance();
    }
}

bool
WorkParent::anyChildRaiseFailure() const
{
    for (auto& c : mChildren)
    {
        if (c.second->getState() == Work::WORK_FAILURE_RAISE)
        {
            return true;
        }
    }
    return false;
}

bool
WorkParent::anyChildFatalFailure() const
{
    for (auto& c : mChildren)
    {
        if (c.second->getState() == Work::WORK_FAILURE_FATAL)
        {
            return true;
        }
    }
    return false;
}

bool
WorkParent::allChildrenSuccessful() const
{
    for (auto& c : mChildren)
    {
        if (c.second->getState() != Work::WORK_SUCCESS)
        {
            return false;
        }
    }
    return true;
}

bool
WorkParent::allChildrenDone() const
{
    for (auto& c : mChildren)
    {
        if (c.second->getState() != Work::WORK_SUCCESS &&
            c.second->getState() != Work::WORK_FAILURE_RAISE &&
            c.second->getState() != Work::WORK_FAILURE_FATAL)
        {
            return false;
        }
    }
    return true;
}

Application&
WorkParent::app() const
{
    return mApp;
}
}
