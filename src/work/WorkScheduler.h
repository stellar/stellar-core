// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
#pragma once

#include "main/Application.h"
#include "work/Work.h"

namespace stellar
{

/**
 * WorkScheduler is a top level Work, that is in charge of scheduling
 * cranks (posting to the IO service); this is done via custom
 * implementation of `onWakeUp`, which schedules a crank if necessary.
 */
class WorkScheduler : public Work
{
    WorkScheduler(Application& app, std::function<void()> callback = nullptr);
    bool mScheduled{false};

  public:
    virtual ~WorkScheduler();
    static std::shared_ptr<WorkScheduler> create(Application& app);

    template <typename T, typename... Args>
    std::shared_ptr<T>
    executeWork(Args&&... args)
    {
        auto work = addWork<T>(std::forward<Args>(args)...);
        auto& clock = mApp.getClock();
        while (!clock.getIOService().stopped() && !allChildrenDone())
        {
            clock.crank(true);
        }
        return work;
    }

    // TODO this probably needs a better name
    template <typename T, typename... Args>
    std::shared_ptr<T>
    addWorkTree(Args&&... args)
    {
        return addWork<T>(std::forward<Args>(args)...);
    }

  protected:
    void onWakeUp() override;
    State doWork() override;
};
}