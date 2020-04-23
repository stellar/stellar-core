// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Scheduler.h"

#include "lib/catch.hpp"
#include "util/Logging.h"
#include "util/Timer.h"
#include <chrono>

using namespace stellar;

TEST_CASE("scheduler basic functionality", "[scheduler]")
{
    std::chrono::seconds window(10);
    VirtualClock clock;
    Scheduler sched(clock, window);

    std::string A("a"), B("b"), C("c");

    size_t nEvents{0};
    auto step = std::chrono::microseconds(1);
    auto microsleep = [&] {
        clock.sleep_for(step);
        ++nEvents;
    };

    sched.enqueue(std::string(A), microsleep,
                  Scheduler::ActionType::NORMAL_ACTION);

    CHECK(sched.size() == 1);
    CHECK(sched.nextQueueToRun() == A);
    CHECK(sched.totalService(A).count() == 0);
    CHECK(sched.queueLength(A) == 1);
    CHECK(sched.stats().mActionsEnqueued == 1);
    CHECK(sched.stats().mActionsDequeued == 0);
    CHECK(sched.stats().mActionsDroppedDueToOverload == 0);
    CHECK(sched.stats().mQueuesActivatedFromFresh == 1);
    CHECK(sched.stats().mQueuesActivatedFromIdle == 0);
    CHECK(sched.stats().mQueuesSuspended == 0);

    CHECK(sched.runOne() == 1); // run A
    CHECK(nEvents == 1);
    CHECK(sched.totalService(A).count() != 0);
    CHECK(sched.stats().mActionsDequeued == 1);
    CHECK(sched.stats().mQueuesSuspended == 1);

    sched.enqueue(std::string(A), microsleep,
                  Scheduler::ActionType::NORMAL_ACTION);
    sched.enqueue(std::string(B), microsleep,
                  Scheduler::ActionType::NORMAL_ACTION);
    sched.enqueue(std::string(C), microsleep,
                  Scheduler::ActionType::NORMAL_ACTION);

    CHECK(sched.size() == 3);
    CHECK(sched.nextQueueToRun() != A);
    CHECK(sched.totalService(A).count() != 0);
    CHECK(sched.totalService(B).count() == 0);
    CHECK(sched.totalService(C).count() == 0);
    CHECK(sched.queueLength(A) == 1);
    CHECK(sched.queueLength(B) == 1);
    CHECK(sched.queueLength(C) == 1);
    CHECK(sched.stats().mActionsEnqueued == 4);
    CHECK(sched.stats().mActionsDroppedDueToOverload == 0);
    CHECK(sched.stats().mQueuesActivatedFromFresh == 3);
    CHECK(sched.stats().mQueuesActivatedFromIdle == 1);

    auto aruntime = sched.totalService(A).count();
    CHECK(sched.runOne() == 1); // run B or C
    CHECK(sched.runOne() == 1); // run B or C
    CHECK(nEvents == 3);
    CHECK(sched.totalService(A).count() == aruntime);
    CHECK(sched.totalService(B).count() != 0);
    CHECK(sched.totalService(C).count() != 0);
    CHECK(sched.queueLength(A) == 1);
    CHECK(sched.queueLength(B) == 0);
    CHECK(sched.queueLength(C) == 0);
    CHECK(sched.stats().mActionsDequeued == 3);
    CHECK(sched.stats().mActionsDroppedDueToOverload == 0);
    CHECK(sched.stats().mQueuesSuspended == 3);

    CHECK(sched.runOne() == 1); // run A
    CHECK(nEvents == 4);
    CHECK(sched.queueLength(A) == 0);
    CHECK(sched.queueLength(B) == 0);
    CHECK(sched.queueLength(C) == 0);
    CHECK(sched.stats().mActionsDequeued == 4);
    CHECK(sched.stats().mQueuesSuspended == 4);
}

TEST_CASE("scheduler load shedding -- overload", "[scheduler]")
{
    std::chrono::microseconds window(100);
    VirtualClock clock;
    Scheduler sched(clock, window);

    std::string A("a"), B("b"), C("c");

    size_t nEvents{0};
    auto step = std::chrono::microseconds(1);
    auto microsleep = [&] {
        clock.sleep_for(step);
        ++nEvents;
    };

    for (size_t i = 0; i < 1000; ++i)
    {
        sched.enqueue(std::string(A), microsleep,
                      Scheduler::ActionType::DROPPABLE_ACTION);
        sched.enqueue(std::string(B), microsleep,
                      Scheduler::ActionType::DROPPABLE_ACTION);
        sched.enqueue(std::string(C), microsleep,
                      Scheduler::ActionType::DROPPABLE_ACTION);
        sched.runOne();
        sched.runOne();
    }
    while (sched.size() != 0)
    {
        sched.runOne();
    }
    CHECK(sched.stats().mActionsDroppedDueToOverload == 901);
    CHECK(sched.stats().mActionsDequeued == 2099);
    auto tot = sched.stats().mActionsDequeued +
               sched.stats().mActionsDroppedDueToOverload;
    CHECK(sched.stats().mActionsEnqueued == tot);
}
