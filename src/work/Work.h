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
 * Work is an extension of BasicWork,
 * which additionally manages children. This allows the following:
 *  - Work might be dependent on the state of its children before performing
 *  its duties
 *  - Work may have children that are independent and could run in parallel,
 *  or dispatch children serially.
 *
 *  It's worth noting that now since crank calls are propagated down the tree
 *  from the work scheduler, implementations should aim to create flatter
 *  structures for better efficiency;
 *  They can utilize WorkSequence if serial order needs to be enforced
 */
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

    template <typename T, typename... Args>
    std::shared_ptr<T>
    addWorkWithCallback(std::function<void()> cb, Args&&... args)
    {
        if (isAborting())
        {
            throw std::runtime_error(getName() + " is being aborted!");
        }

        auto child = std::make_shared<T>(mApp, std::forward<Args>(args)...);
        addChild(child);
        auto wakeSelf = wakeSelfUpCallback(cb);
        child->startWork(wakeSelf);
        wakeSelf();
        return child;
    }

    State onRun() final;
    bool onAbort() final;
    void onReset() final;
    void onFailureRaise() override;
    void onFailureRetry() override;

    // Implementers decide what they want to do: spawn more children,
    // wait for all children to finish, or perform Work
    virtual BasicWork::State doWork() = 0;

    // Provide additional cleanup logic for reset
    virtual void doReset();

  private:
    std::list<std::shared_ptr<BasicWork>> mChildren;
    std::list<std::shared_ptr<BasicWork>>::const_iterator mNextChild;

    uint32_t mDoneChildren{0};
    uint32_t mTotalChildren{0};

    std::shared_ptr<BasicWork> yieldNextRunningChild();
    void addChild(std::shared_ptr<BasicWork> child);
    void clearChildren();
    void shutdownChildren();

    bool mAbortChildrenButNotSelf{false};
};

/*
 * WorkSequence is a helper class, that implementers can use if they
 * wish to enforce the order of work execution. Users are required to pass
 * a vector of BasicWork pointers in the expected execution order.
 */
class WorkSequence : public BasicWork
{
    std::vector<std::shared_ptr<BasicWork>> mSequenceOfWork;
    std::vector<std::shared_ptr<BasicWork>>::const_iterator mNextInSequence;
    std::shared_ptr<BasicWork> mCurrentExecuting;

  public:
    WorkSequence(Application& app, std::string name,
                 std::vector<std::shared_ptr<BasicWork>> sequence,
                 size_t maxRetries = RETRY_A_FEW);
    ~WorkSequence() = default;

    std::string getStatus() const override;
    void shutdown() override;

  protected:
    State onRun() final;
    bool onAbort() final;
    void onReset() final;
};

namespace WorkUtils
{
BasicWork::State checkChildrenStatus(Work const& w);
}
}
