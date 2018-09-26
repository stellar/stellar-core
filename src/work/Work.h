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

class Work : public BasicWork
{
    bool mScheduleSelf{false};
    std::list<std::shared_ptr<Work>> mChildren;
    std::list<std::shared_ptr<Work>>::const_iterator mNextChild;
    std::shared_ptr<Work> yieldNextRunningChild();

    void addChild(std::shared_ptr<Work> child);

  public:
    virtual ~Work();

    // Note: `shared_from_this` assumes there exists a shared_ptr to the
    // references class. This relates to who owns what in Work interface.
    // Thus, `addWork` should be used to create work (then the parent holds
    // the reference).

    template <typename T, typename... Args>
    std::shared_ptr<T>
    addWork(Args&&... args)
    {
        // `wakeUp` is sufficient as a callback for any child work of
        // WorkScheduler (see custom callback for WorkScheduler)
        std::weak_ptr<Work> weak(
            std::static_pointer_cast<Work>(shared_from_this()));
        auto callback = [weak]() {
            auto self = weak.lock();
            if (self)
            {
                self->wakeUp();
            }
        };

        auto child =
            std::make_shared<T>(mApp, callback, std::forward<Args>(args)...);
        addChild(child);
        wakeUp();
        return child;
    }

    // Children status helper methods
    bool allChildrenSuccessful() const;
    bool allChildrenDone() const;
    bool anyChildRaiseFailure() const;
    bool anyChildFatalFailure() const;
    bool anyChildRunning() const;
    bool hasChildren() const;

  protected:
    Work(Application& app, std::function<void()> callback, std::string name,
         size_t retries = BasicWork::RETRY_A_FEW);
    void clearChildren();
    State onRun() final;
    void onReset() override;

    // Implementers decide what they want to do: spawn more children,
    // wait for all children to finish, or perform work
    virtual BasicWork::State doWork() = 0;
};
}
