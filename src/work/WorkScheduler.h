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
 * cranks (posting to the IO service); this is done via supplying any child work
 * with a `scheduleOne` callback.
 *
 * WorkScheduler attempts fair scheduling by doing round-robin among
 * works that wish to execute.
 */
class WorkScheduler : public Work
{
    explicit WorkScheduler(Application& app);
    bool mScheduled{false};
    VirtualTimer mTriggerTimer;
    static std::chrono::milliseconds const TRIGGER_PERIOD;

  public:
    virtual ~WorkScheduler();
    static std::shared_ptr<WorkScheduler> create(Application& app);

    template <typename T, typename... Args>
    std::shared_ptr<T>
    executeWork(Args&&... args)
    {
        auto work = scheduleWork<T>(std::forward<Args>(args)...);
        auto& clock = mApp.getClock();
        while (!clock.getIOContext().stopped() && !allChildrenDone())
        {
            clock.crank(true);
        }
        return work;
    }

    // Returns a work that's been scheduled, or nullptr if the WorkScheduler
    // is aborting.
    template <typename T, typename... Args>
    std::shared_ptr<T>
    scheduleWork(Args&&... args)
    {
        if (isAborting())
        {
            return nullptr;
        }
        std::weak_ptr<WorkScheduler> weak(
            std::static_pointer_cast<WorkScheduler>(shared_from_this()));
        // Callback to schedule next crank that will only run if WorkScheduler
        // is in non-aborting state.
        auto innerCallback = [weak]() { scheduleOne(weak); };
        return addWorkWithCallback<T>(innerCallback,
                                      std::forward<Args>(args)...);
    }

    void shutdown() override;

  protected:
    static void scheduleOne(std::weak_ptr<WorkScheduler> weak);
    State doWork() override;
};
}
