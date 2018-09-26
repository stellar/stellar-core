// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "WorkSchedulerImpl.h"
#include "WorkScheduler.h"
#include "lib/util/format.h"
#include "util/Logging.h"

namespace stellar
{
WorkScheduler::WorkScheduler(Application& app)
    : Work(app, nullptr, "work-scheduler", BasicWork::RETRY_NEVER)
{
}

WorkScheduler::~WorkScheduler()
{
}

WorkSchedulerImpl::WorkSchedulerImpl(Application& app) : WorkScheduler(app)
{
}

WorkSchedulerImpl::~WorkSchedulerImpl()
{
    // TODO (mlo) With proper shutdown implementation, this call
    // won't be necessary as WorkScheduler will transition into
    // failure state and `reset` will take care of clearing children
    clearChildren();
}

std::shared_ptr<WorkScheduler>
WorkScheduler::create(Application& app)
{
    auto work = std::make_shared<WorkSchedulerImpl>(app);
    work->registerCallback();
    work->crankWork();
    return work;
};

void
WorkSchedulerImpl::registerCallback()
{
    std::weak_ptr<WorkSchedulerImpl> weak(
        std::static_pointer_cast<WorkSchedulerImpl>(shared_from_this()));
    mNotifyCallback = [weak]() {
        auto self = weak.lock();
        if (!self)
        {
            return;
        }

        if (self->getState() == BasicWork::WORK_RUNNING)
        {

            if (self->mScheduled)
            {
                return;
            }
            self->mScheduled = true;
            self->mApp.getClock().getIOService().post([weak]() {
                auto innerSelf = weak.lock();
                if (!innerSelf)
                {
                    return;
                }
                innerSelf->mScheduled = false;
                innerSelf->crankWork();
                innerSelf->mNotifyCallback();
            });
        }
        else if (self->getState() == BasicWork::WORK_WAITING &&
                 self->anyChildRunning())
        {
            CLOG(DEBUG, "Work") << "Pending work, waking up...";
            self->wakeUp();
        }
    };
}

BasicWork::State
WorkSchedulerImpl::doWork()
{
    if (anyChildRunning())
    {
        return BasicWork::WORK_RUNNING;
    }
    return BasicWork::WORK_WAITING;
}
}
