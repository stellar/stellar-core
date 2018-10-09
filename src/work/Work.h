// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
#pragma once

#include "main/Application.h"
#include "work/BasicWork.h"
#include <list>
#include <set>

namespace stellar
{
/**
 * Work is an extension of BasicWork,
 * which additionally manages children. This allows the following:
 *  - Work might be dependent on the state of its children before performing
 *  its duties
 *  - Work may have children that are independent and could run in parallel,
 *  or dispatch children serially.
 */
class Work : public BasicWork
{
  public:
    enum Execution
    {
        WORK_PARALLEL,
        WORK_SERIAL
    };

    virtual ~Work();

    // Children status helper methods
    bool allChildrenSuccessful() const;
    bool allChildrenDone() const;
    bool allChildrenWaiting() const;
    bool anyChildRaiseFailure() const;
    bool anyChildRunning() const;
    bool hasChildren() const;

  protected:
    Work(Application& app, std::function<void()> callback, std::string name,
         size_t retries = BasicWork::RETRY_A_FEW,
         Execution order = WORK_PARALLEL);

    // Note: `shared_from_this` assumes there exists a shared_ptr to the
    // references class. This relates to who owns what in Work interface.
    // Thus, `addWork` should be used to create Work (then the parent holds
    // the reference).
    template <typename T, typename... Args>
    std::shared_ptr<T>
    addWork(Args&&... args)
    {
        auto child = std::make_shared<T>(mApp, wakeUpCallback(),
                                         std::forward<Args>(args)...);
        addChild(child);
        child->reset();
        wakeUp();
        return child;
    }

    State onRun() final;
    void onReset() final;

    // Implementers decide what they want to do: spawn more children,
    // wait for all children to finish, or perform Work
    virtual BasicWork::State doWork() = 0;

    // Provide additional cleanup logic for reset
    virtual void doReset();

  private:
    std::list<std::shared_ptr<BasicWork>> mChildren;
    std::list<std::shared_ptr<BasicWork>>::const_iterator mNextChild;
    Execution const mExecutionOrder;

    std::shared_ptr<BasicWork> yieldNextRunningChild();
    void addChild(std::shared_ptr<BasicWork> child);
    void clearChildren();

    friend class WorkSequence;
};

/*
 * WorkSequence is a helper class, that implementers can use if they
 * wish to enforce the order of work execution. It also allows parent works
 * to construct more complex work trees by exposing public `addToSequence`
 * method.
 */
class WorkSequence : public Work
{
  public:
    WorkSequence(Application& app, std::function<void()> callback,
                 std::string name);
    ~WorkSequence() = default;

    template <typename T, typename... Args>
    std::shared_ptr<T>
    addToSequence(Args&&... args)
    {
        return addWork<T>(std::forward<Args>(args)...);
    }

  protected:
    State doWork() final;
};
}