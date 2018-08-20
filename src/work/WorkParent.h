#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/NonCopyable.h"
#include <deque>
#include <map>
#include <memory>
#include <string>

namespace stellar
{

class Application;
class Work;

/**
 * WorkParent is a class of things-that-hold Work, and are notified by work
 * when it completes. This is an abstract base that's implemented by both
 * WorkManager and Work itself.
 *
 * It also has a utility method addWork<W>(...) for subclasses of Work;
 * these are constructed with appropriate application and parent links and
 * automatically added to the child list.
 */
class WorkParent : public std::enable_shared_from_this<WorkParent>,
                   private NonMovableOrCopyable
{
  protected:
    Application& mApp;
    std::map<std::string, std::shared_ptr<Work>> mChildren;

  public:
    WorkParent(Application& app);
    virtual ~WorkParent();
    virtual void notify(std::string const& childChanged) = 0;
    void addChild(std::shared_ptr<Work> child);
    void clearChildren();
    void advanceChildren();
    bool anyChildRaiseFailure() const;
    bool anyChildFatalFailure() const;
    bool allChildrenSuccessful() const;
    bool allChildrenDone() const;

    Application& app() const;

    template <typename T, typename... Args>
    std::shared_ptr<T>
    addWork(Args&&... args)
    {
        auto w = std::make_shared<T>(mApp, *this, std::forward<Args>(args)...);
        addChild(w);
        return w;
    }

    std::map<std::string, std::shared_ptr<Work>> const&
    getChildren()
    {
        return mChildren;
    }
};
}
