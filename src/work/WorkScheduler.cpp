// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "work/WorkScheduler.h"
#include "util/Logging.h"

namespace stellar
{

WorkScheduler::WorkScheduler(Application& app)
    : Work(app, "work-scheduler", BasicWork::RETRY_NEVER)
    , mDebtTracker("work", 1024)
{
}

WorkScheduler::~WorkScheduler()
{
}

std::shared_ptr<WorkScheduler>
WorkScheduler::create(Application& app)
{
    auto work = std::shared_ptr<WorkScheduler>(new WorkScheduler(app));
    work->startWork(nullptr);
    work->crankWork();
    return work;
};

BasicWork::State
WorkScheduler::doWork()
{
    if (anyChildRunning())
    {
        return State::WORK_RUNNING;
    }
    return State::WORK_WAITING;
}

void
WorkScheduler::scheduleOne(std::weak_ptr<WorkScheduler> weak)
{
    auto self = weak.lock();
    if (!self || self->mScheduled)
    {
        return;
    }

    self->mScheduled = true;
    self->mApp.postOnMainThread(
        [weak]() {
            auto innerSelf = weak.lock();
            if (!innerSelf)
            {
                return;
            }

            try
            {
                // Loop as to perform some meaningful amount of work.
                //
                // Note this is charged to the WorkScheduler's local
                // TimeDebtTracker as we expect Work steps to run over budget
                // relatively frequently and do not want their debt mixed with
                // the main VirtualClock ExecutionDebtTracker, which would cause
                // other higher-priority callbacks to be delayed to pay off our
                // debts.
                YieldTimer yt(innerSelf->mApp.getClock(),
                              innerSelf->mDebtTracker,
                              std::chrono::milliseconds(1));
                while (innerSelf->getState() == State::WORK_RUNNING &&
                       yt.shouldKeepGoing())
                {
                    innerSelf->crankWork();
                };
            }
            catch (...)
            {
                innerSelf->mScheduled = false;
                throw;
            }
            innerSelf->mScheduled = false;

            if (innerSelf->getState() == State::WORK_RUNNING)
            {
                scheduleOne(weak);
            }
        },
        {VirtualClock::ExecutionCategory::Type::NORMAL_EVENT, "WorkScheduler"});
}

void
WorkScheduler::shutdown()
{
    if (isDone())
    {
        return;
    }
    Work::shutdown();
    std::weak_ptr<WorkScheduler> weak(
        std::static_pointer_cast<WorkScheduler>(shared_from_this()));
    scheduleOne(weak);
}
}
