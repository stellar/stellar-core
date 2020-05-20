// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "work/WorkScheduler.h"
#include "lib/util/finally.h"
#include "util/Logging.h"

namespace stellar
{
// WorkScheduler performs a crank every TRIGGER_PERIOD
// it should be set small enough that work gets executed fast enough
// but not too small as to take over execution in the main thread
std::chrono::milliseconds const WorkScheduler::TRIGGER_PERIOD(50);

WorkScheduler::WorkScheduler(Application& app)
    : Work(app, "work-scheduler", BasicWork::RETRY_NEVER), mTriggerTimer(app)
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
            {
                auto fini =
                    gsl::finally([&]() { innerSelf->mScheduled = false; });
                // loop as to perform some meaningful amount of work
                do
                {
                    innerSelf->crankWork();
                } while (innerSelf->getState() == State::WORK_RUNNING &&
                         !innerSelf->mApp.getClock().shouldYield());
            }
            if (innerSelf->getState() == State::WORK_RUNNING)
            {
                scheduleOne(weak);
            }
        },
        "WorkScheduler");
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
