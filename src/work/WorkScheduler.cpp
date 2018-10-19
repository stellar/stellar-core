// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "work/WorkScheduler.h"
#include "lib/util/format.h"
#include "util/Logging.h"

namespace stellar
{
WorkScheduler::WorkScheduler(Application& app)
    : Work(app, "work-scheduler", BasicWork::RETRY_NEVER)
{
}

WorkScheduler::~WorkScheduler()
{
}

std::shared_ptr<WorkScheduler>
WorkScheduler::create(Application& app, std::function<void()> wakeUpCallback)
{
    auto work = std::shared_ptr<WorkScheduler>(new WorkScheduler(app));
    work->restartWork(wakeUpCallback);
    work->crankWork();
    return work;
};

void
WorkScheduler::onWakeUp()
{
    if (mScheduled)
    {
        return;
    }

    std::weak_ptr<WorkScheduler> weak(
        std::static_pointer_cast<WorkScheduler>(shared_from_this()));
    mScheduled = true;
    mApp.getClock().getIOService().post([weak]() {
        auto self = weak.lock();
        if (!self)
        {
            return;
        }
        self->mScheduled = false;
        self->crankWork();
        if (self->getState() == WORK_RUNNING)
        {
            self->onWakeUp();
        }
    });
}

BasicWork::State
WorkScheduler::doWork()
{
    if (anyChildRunning())
    {
        return BasicWork::WORK_RUNNING;
    }
    return BasicWork::WORK_WAITING;
}
}