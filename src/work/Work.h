#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Timer.h"
#include "work/BasicWork.h"
#include <list>
#include <map>
#include <memory>
#include <string>

namespace stellar
{

class Application;

/**
 * Work is an extension of BasicWork which additionally manages children.
 * This allows the following:
 *
 *  - Work might be dependent on the state of its children before
 *    performing its duties.
 *
 *  - Work may have children that are independent and could run in
 *    parallel, or dispatch children serially.
 *
 *  It's worth noting that now since crank calls are propagated down the
 *  tree from the work scheduler, implementations should aim to create
 *  flatter structures for better efficiency; they can use WorkSequence
 *  if a long serial order needs to be enforced.
 */

// Helper lambda that parent work can pass to children
using OnFailureCallback = std::function<void()>;

class Work : public BasicWork
{
  public:
    virtual ~Work();

    std::string getStatus() const override;

    bool allChildrenSuccessful() const;
    bool allChildrenDone() const;
    bool anyChildRaiseFailure() const;
    bool anyChildRunning() const;
    bool hasChildren() const;

    void shutdown() override;

  protected:
    Work(Application& app, std::string name,
         size_t retries = BasicWork::RETRY_A_FEW);

    // Note: `shared_from_this` assumes there exists a shared_ptr to the
    // references class. This relates to who owns what in Work interface.
    // Thus, `addWork` should be used to create Work (then the parent holds
    // the reference).
    template <typename T, typename... Args>
    std::shared_ptr<T>
    addWork(Args&&... args)
    {
        return addWorkWithCallback<T>(nullptr, std::forward<Args>(args)...);
    }

    // addWorkWithCallback creates and starts a child Work with a default
    // notification mechanism, `wakeSelfUpCallback`. Note that an additional
    // callback may be passed in, in case anything else needs to be done after
    // waking up.
    template <typename T, typename... Args>
    std::shared_ptr<T>
    addWorkWithCallback(std::function<void()> cb, Args&&... args)
    {
        if (isAborting())
        {
            throw std::runtime_error(getName() + " is being aborted!");
        }

        auto child = std::make_shared<T>(mApp, std::forward<Args>(args)...);
        addWork(cb, child);
        return child;
    }

    void
    addWork(std::function<void()> cb, std::shared_ptr<BasicWork> child)
    {
        addChild(child);
        auto wakeSelf = wakeSelfUpCallback(cb);
        child->startWork(wakeSelf);
        wakeSelf();
    }

    State onRun() final;
    bool onAbort() final;
    // Clean up Work's state and children for safe restart/destruction.
    void onReset() final;
    void onFailureRaise() override;
    void onFailureRetry() override;
    BasicWork::State checkChildrenStatus() const;

    // Work::onRun() above implements logic that propagates onRun() calls
    // to the next runnable child in round-robin order; when no children
    // are runnable, it proceeds to call this `doWork` function, which
    // implementers must override to define what they want to do _locally_
    // at this level of a work-supervision tree: spawn more children,
    // inspect and/or wait for existing children to finish, or perform
    // other local work of their own.
    virtual BasicWork::State doWork() = 0;

    // Provide additional cleanup logic for reset
    virtual void doReset();

  private:
    std::list<std::shared_ptr<BasicWork>> mChildren;
    std::list<std::shared_ptr<BasicWork>>::const_iterator mNextChild;

    size_t mDoneChildren{0};
    size_t mTotalChildren{0};

    std::shared_ptr<BasicWork> yieldNextRunningChild();
    void addChild(std::shared_ptr<BasicWork> child);
    void clearChildren();
    void shutdownChildren();

    bool mAbortChildrenButNotSelf{false};
};

namespace WorkUtils
{
BasicWork::State
getWorkStatus(std::list<std::shared_ptr<BasicWork>> const& works);

bool allSuccessful(std::list<std::shared_ptr<BasicWork>> const& works);
bool anyFailed(std::list<std::shared_ptr<BasicWork>> const& works);
bool anyRunning(std::list<std::shared_ptr<BasicWork>> const& works);
}
}
