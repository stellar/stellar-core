// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "work/WorkScheduler.h"
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

    // Note: at the moment we're using a timer (when not in STANDALONE mode) to
    // throttle _down_ the amount of "work" we do statically: we set a yield
    // timer to 1ms so that in case we're a long-running step, we don't hog the
    // CPU and incur IO debt.
    //
    // Long-running work steps are a problem in practice: specifically the
    // bucket-apply work tends to have a long pause when it commits -- longer
    // than the presumed quantum in the central IO-vs-RR-queues time-slicing
    // scheme -- and this can cause a node that is catching up to starve IO and
    // desync.
    //
    // In other words, while we _could_ avoid trying to explicitly time-slice
    // ourselves here, and instead post ourselves to the RR queues, the problem
    // with this is that the RR queues do not currently account for IO debt very
    // well and we may well go "over budget" every time we get scheduled. The RR
    // queues really need to track how much a callback exceeds its yield-timer
    // loop and treat that as over-budget debt to pay off in subsequent
    // iterations, to maintain a static schedule. This is TBD.
    //
    // See
    // https://github.com/stellar/stellar-core/issues/2304#issuecomment-614953677

    auto cb = [weak]() {
        auto innerSelf = weak.lock();
        if (!innerSelf)
        {
            return;
        }

        try
        {
            // loop as to perform some meaningful amount of work
            YieldTimer yt(innerSelf->mApp.getClock(),
                          std::chrono::milliseconds(1));
            do
            {
                innerSelf->crankWork();
            } while (innerSelf->getState() == State::WORK_RUNNING &&
                     yt.shouldKeepGoing());
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
    };

    // For the tine being -- until we have a better overall scheduling strategy
    // -- we only do the time-slicing discussed above in non-STANDALONE modes;
    // when running STANDALONE (such as command-line catchup) we do not care
    // about yielding to network IO and want to maximize frequency of
    // work-execution.
    self->mScheduled = true;
    if (self->mApp.getConfig().RUN_STANDALONE)
    {
        self->mApp.postOnMainThread(
            cb, {VirtualClock::ExecutionCategory::Type::NORMAL_EVENT,
                 "WorkScheduler"});
    }
    else
    {
        self->mTriggerTimer.expires_from_now(TRIGGER_PERIOD);
        self->mTriggerTimer.async_wait([cb = std::move(cb)](asio::error_code) {
            cb();
        });
    }
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
