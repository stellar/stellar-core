// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "process/ProcessManager.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Fs.h"
#include "work/WorkManager.h"

#include <cstdio>
#include <fstream>
#include <random>
#include <xdrpp/autocheck.h>

using namespace stellar;

class CallCmdWork : public Work
{
    std::string mCommand;

  public:
    CallCmdWork(Application& app, WorkParent& parent, std::string command)
        : Work(app, parent, std::string("call-") + command), mCommand(command)
    {
    }
    virtual void
    onRun() override
    {
        auto evt = mApp.getProcessManager().runProcess(mCommand);
        evt.async_wait(callComplete());
    }
};

TEST_CASE("work manager", "[work]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application::pointer appPtr = createTestApplication(clock, cfg);
    auto& wm = appPtr->getWorkManager();

    auto w = wm.addWork<CallCmdWork>("hostname");
    w->addWork<CallCmdWork>("date");
    w->addWork<CallCmdWork>("uname");
    wm.advanceChildren();
    while (!wm.allChildrenSuccessful())
    {
        clock.crank();
    }
}

class CountDownWork : public Work
{
    size_t mCount;

  public:
    CountDownWork(Application& app, WorkParent& parent, size_t count)
        : Work(app, parent, std::string("countdown-") + std::to_string(count))
        , mCount(count)
    {
    }

    virtual void
    onRun() override
    {
        LOG(INFO) << "CountDown " << getUniqueName() << " : " << mCount;
        scheduleComplete();
    }

    virtual Work::State
    onSuccess() override
    {
        if (mCount > 0)
        {
            mCount--;
            return WORK_RUNNING;
        }
        return WORK_SUCCESS;
    }
};

TEST_CASE("work steps", "[work]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application::pointer appPtr = createTestApplication(clock, cfg);
    auto& wm = appPtr->getWorkManager();
    auto w = wm.addWork<CountDownWork>(10);
    w->addWork<CountDownWork>(5);
    w->addWork<CountDownWork>(7);
    wm.advanceChildren();
    while (!wm.allChildrenSuccessful())
    {
        clock.crank();
    }
}

class FailingWork : public Work
{
  public:
    FailingWork(Application& app, WorkParent& parent,
                std::string const& uniqueName)
        : Work(app, parent, uniqueName, 0)
    {
    }

    virtual void
    onRun() override
    {
        scheduleFailure();
    }
};

class WorkCountingOnFailureRaise : public Work
{
  public:
    int mCount{0};

    WorkCountingOnFailureRaise(Application& app, WorkParent& parent)
        : Work(app, parent, std::string("counting-on-complete"), 0)
    {
    }

    virtual void
    onRun() override
    {
        mCount++;
        Work::onRun();
    }

    virtual void
    onFailureRaise() override
    {
        mCount++;
        Work::onFailureRaise();
    }
};

TEST_CASE("subwork items fail at the same time", "[work]")
{
    VirtualClock clock;
    auto const& cfg = getTestConfig();
    auto app = createTestApplication(clock, cfg);
    auto& wm = app->getWorkManager();
    auto w = wm.addWork<WorkCountingOnFailureRaise>();
    w->addWork<FailingWork>("work-1");
    w->addWork<FailingWork>("work-2");
    wm.advanceChildren();
    while (!wm.allChildrenDone())
    {
        clock.crank();
    }

    REQUIRE(w->mCount == 1);
}

class WorkDoNothing : public Work
{
  public:
    WorkDoNothing(Application& app, WorkParent& parent,
                  std::string const& uniqueName)
        : Work(app, parent, uniqueName, 0)
    {
    }

    virtual void
    onRun() override
    {
    }

    void
    forceSuccess()
    {
        callComplete()({});
    }
};

class WorkWith2Subworks : public Work
{
  public:
    WorkWith2Subworks(Application& app, WorkParent& parent,
                      std::string const& uniqueName)
        : Work(app, parent, uniqueName, 0)
    {
    }

    virtual void
    onReset() override
    {
        clearChildren();
        mFirstSubwork.reset();
        mSecondSubwork.reset();

        mFirstSubwork =
            addWork<WorkDoNothing>("first-subwork-of-" + getUniqueName());
    }

    virtual Work::State
    onSuccess() override
    {
        CLOG(DEBUG, "History") << "onSuccess() for " << getUniqueName();

        if (mSecondSubwork)
        {
            mCalledSuccessWithPendingSubwork =
                mCalledSuccessWithPendingSubwork ||
                mSecondSubwork->getState() == WORK_PENDING;
            return WORK_SUCCESS;
        }

        mSecondSubwork =
            addWork<WorkDoNothing>("second-subwork-of-" + getUniqueName());
        return WORK_PENDING;
    }

    std::shared_ptr<WorkDoNothing> mFirstSubwork;
    std::shared_ptr<WorkDoNothing> mSecondSubwork;
    bool mCalledSuccessWithPendingSubwork{false};
};

TEST_CASE("sub-subwork items succeed at the same time", "[work]")
{
    VirtualClock clock;
    auto const& cfg = getTestConfig();
    auto app = createTestApplication(clock, cfg);
    auto& wm = app->getWorkManager();
    auto w = wm.addWork<Work>("parent-of-many");
    auto work1 = w->addWork<WorkWith2Subworks>("work-1");
    auto work2 = w->addWork<WorkDoNothing>("work-2");
    auto work3 = w->addWork<WorkDoNothing>("work-3");

    auto i = 0;

    wm.advanceChildren();
    while (!wm.allChildrenDone())
    {
        clock.crank(false);

        switch (i++)
        {
        case 1:
            work2->forceSuccess();
            work1->mFirstSubwork->forceSuccess();
            work3->forceSuccess();
            break;
        case 2:
            work1->mSecondSubwork->forceSuccess();
            break;
        }
    }

    REQUIRE(!work1->mCalledSuccessWithPendingSubwork);
}

TEST_CASE("subwork triggers abort", "[work][workabort]")
{
    VirtualClock clock;
    auto const& cfg = getTestConfig();
    auto app = createTestApplication(clock, cfg);
    auto& wm = app->getWorkManager();
    auto w = wm.addWork<Work>("main-work");

    SECTION("other child pending")
    {
        auto failWork = w->addWork<FailingWork>("work-1");
        auto normalWork = w->addWork<WorkDoNothing>("work-2");
        failWork->advance();
        while (!wm.allChildrenDone())
        {
            clock.crank();
        }

        REQUIRE(normalWork->getState() == Work::WORK_FAILURE_ABORTED);
        REQUIRE(w->getState() == Work::WORK_FAILURE_RAISE);
    }
    SECTION("other child already succeeded")
    {
        auto failWork = w->addWork<FailingWork>("work-1");
        auto normalWork = w->addWork<WorkDoNothing>("work-2");
        normalWork->forceSuccess();

        while (!wm.allChildrenDone())
        {
            clock.crank();
        }

        REQUIRE(normalWork->getState() == Work::WORK_SUCCESS);
        REQUIRE(w->getState() == Work::WORK_FAILURE_RAISE);
    }
    SECTION("other child running")
    {
        auto work1 = w->addWork<CountDownWork>(10);
        wm.advanceChildren();
        auto work2 = w->addWork<FailingWork>("work-2");
        wm.advanceChildren();

        while (!wm.allChildrenDone())
        {
            clock.crank();
        }

        REQUIRE(work1->getState() == Work::WORK_FAILURE_ABORTED);
        REQUIRE(work2->getState() == Work::WORK_FAILURE_RAISE);
        REQUIRE(w->getState() == Work::WORK_FAILURE_RAISE);
    }
}

TEST_CASE("work manual abort", "[work][workabort]")
{
    VirtualClock clock;
    auto const& cfg = getTestConfig();
    auto app = createTestApplication(clock, cfg);
    auto& wm = app->getWorkManager();
    auto w = wm.addWork<Work>("main-work");

    auto work1 = w->addWork<WorkDoNothing>("work-1");
    auto work2 = w->addWork<WorkDoNothing>("work-2");

    SECTION("main work aborts")
    {
        w->abort();
        while (!wm.allChildrenDone())
        {
            clock.crank();
        }

        REQUIRE(work1->getState() == Work::WORK_FAILURE_ABORTED);
        REQUIRE(work2->getState() == Work::WORK_FAILURE_ABORTED);
        REQUIRE(w->getState() == Work::WORK_FAILURE_ABORTED);
    }
    SECTION("abort propagates to the top")
    {
        work2->abort();
        while (!wm.allChildrenDone())
        {
            clock.crank();
        }

        REQUIRE(work1->getState() == Work::WORK_FAILURE_ABORTED);
        REQUIRE(work2->getState() == Work::WORK_FAILURE_ABORTED);
        REQUIRE(w->getState() == Work::WORK_FAILURE_ABORTED);
    }
}

TEST_CASE("work abort with complex structure", "[work][workabort]")
{
    VirtualClock clock;
    auto const& cfg = getTestConfig();
    auto app = createTestApplication(clock, cfg);
    auto& wm = app->getWorkManager();
    auto w = wm.addWork<Work>("main-work");

    auto work1 = w->addWork<Work>("work-1");
    auto work2 = w->addWork<Work>("work-2");
    auto work3 = w->addWork<Work>("work-3");

    auto w1_work4 = work1->addWork<Work>("work-4");
    auto w1_work5 = work1->addWork<Work>("work-5");

    auto w2_work6 = work2->addWork<FailingWork>("work-6");

    work2->advance();
    while (!wm.allChildrenDone())
    {
        clock.crank();
    }

    REQUIRE(work1->getState() == Work::WORK_FAILURE_ABORTED);
    REQUIRE(w1_work4->getState() == Work::WORK_FAILURE_ABORTED);
    REQUIRE(w1_work5->getState() == Work::WORK_FAILURE_ABORTED);
    REQUIRE(work2->getState() == Work::WORK_FAILURE_RAISE);
    REQUIRE(w2_work6->getState() == Work::WORK_FAILURE_RAISE);
    REQUIRE(work3->getState() == Work::WORK_FAILURE_ABORTED);
}
