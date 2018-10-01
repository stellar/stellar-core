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
#include "work/WorkScheduler.h"

#include "historywork/RunCommandWork.h"
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

class TestWork : public Work
{
    bool mRetry;

  public:
    int mSteps{3};
    int mRunningCount{0};
    int mSuccessCount{0};
    int mFailureCount{0};
    int mRetryCount{0};

    TestWork(Application& app, std::function<void()> callback, std::string name,
             bool retry = false)
        : Work(app, callback, name, RETRY_ONCE), mRetry(retry)
    {
    }

    BasicWork::State
    doWork() override
    {
        if (mSteps-- > 0)
        {
            mRunningCount++;
            return BasicWork::WORK_RUNNING;
        }
        return mRetry ? BasicWork::WORK_FAILURE_RETRY : BasicWork::WORK_SUCCESS;
    }

    void
    onSuccess() override
    {
        mSuccessCount++;
    }

    void
    onFailureRaise() override
    {
        mFailureCount++;
    }

    void
    onFailureRetry() override
    {
        mRetryCount++;
    }

    void
    doReset() override
    {
        mSteps = 3;
    }
};

class TestWorkWaitForChildren : public Work
{
  public:
    TestWorkWaitForChildren(Application& app, std::function<void()> callback,
                            std::string name)
        : Work(app, callback, name, RETRY_ONCE)
    {
    }

    BasicWork::State
    doWork() override
    {
        if (allChildrenSuccessful())
        {
            return BasicWork::WORK_SUCCESS;
        }
        return BasicWork::WORK_RUNNING;
    }
};

TEST_CASE("Work scheduling", "[work]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application::pointer appPtr = createTestApplication(clock, cfg);
    auto& wm = appPtr->getWorkScheduler();

    auto checkSuccess = [](TestWork const& w) {
        REQUIRE(w.mRunningCount == w.mSteps);
        REQUIRE(w.mSuccessCount == 1);
        REQUIRE(w.mFailureCount == 0);
        REQUIRE(w.mRetryCount == 0);
    };

    SECTION("basic one work")
    {
        auto w = wm.addWork<TestWork>("test-work");
        while (!wm.allChildrenSuccessful())
        {
            clock.crank();
        }
        checkSuccess(*w);
    }

    SECTION("2 works round robin")
    {
        auto w1 = wm.addWork<TestWork>("test-work1");
        auto w2 = wm.addWork<TestWork>("test-work2");
        while (!wm.allChildrenSuccessful())
        {
            clock.crank();
        }
        checkSuccess(*w1);
        checkSuccess(*w2);
    }

    SECTION("another work added midway")
    {
        int crankCount = 0;
        auto w1 = wm.addWork<TestWork>("test-work1");
        auto w2 = wm.addWork<TestWork>("test-work2");
        while (!wm.allChildrenSuccessful())
        {
            if (++crankCount > 1)
            {
                wm.addWork<TestWork>("test-work3");
            };
            clock.crank();
        }
        checkSuccess(*w1);
        checkSuccess(*w2);
    }

    SECTION("new work wakes up scheduler")
    {
        // Scheduler wakes up to execute work,
        // and goes back into a waiting state once child is done
        while (wm.getState() != BasicWork::WORK_WAITING)
        {
            clock.crank();
        }

        auto w = wm.addWork<TestWork>("test-work");
        REQUIRE(wm.getState() == BasicWork::WORK_RUNNING);
        while (!wm.allChildrenSuccessful())
        {
            clock.crank();
        }
        checkSuccess(*w);
    }

    SECTION("work retries and fails")
    {
        auto w = wm.addWork<TestWork>("test-work", true);
        while (!wm.allChildrenDone())
        {
            clock.crank();
        }
        REQUIRE(w->getState() == BasicWork::WORK_FAILURE_RAISE);

        // Work of 3 steps retried once
        REQUIRE(w->mRunningCount == w->mSteps * 2);
        REQUIRE(w->mSuccessCount == 0);
        REQUIRE(w->mFailureCount == 1);
        REQUIRE(w->mRetryCount == 1);
    }
}

TEST_CASE("work with children scheduling", "[work]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application::pointer appPtr = createTestApplication(clock, cfg);
    auto& wm = appPtr->getWorkScheduler();

    auto w1 = wm.addWork<TestWorkWaitForChildren>("test-work1");
    auto w2 = wm.addWork<TestWorkWaitForChildren>("test-work2");
    auto w3 = w1->addWork<TestWork>("test-work3");

    while (!wm.allChildrenSuccessful())
    {
        clock.crank();
    }
}

class TestRunCommandWork : public RunCommandWork
{
    std::string mCommand;

  public:
    TestRunCommandWork(Application& app, std::function<void()> callback,
                       std::string name, std::string command)
        : RunCommandWork(app, callback, name), mCommand(command)
    {
    }
    ~TestRunCommandWork() override = default;

    RunCommandInfo
    getCommand() override
    {
        return RunCommandInfo(mCommand, std::string());
    }
};

TEST_CASE("RunCommandWork test", "[work]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application::pointer appPtr = createTestApplication(clock, cfg);
    auto& wm = appPtr->getWorkScheduler();

    SECTION("one run command work")
    {
        wm.addWork<TestRunCommandWork>("test-run-command", "date");
        while (!wm.allChildrenSuccessful())
        {
            clock.crank();
        }
    }
    SECTION("round robin with other work")
    {
        wm.addWork<TestRunCommandWork>("test-run-command", "date");
        wm.addWork<TestWork>("test-work");
        while (!wm.allChildrenSuccessful())
        {
            clock.crank();
        }
    }
    SECTION("invalid run command")
    {
        auto w =
            wm.addWork<TestRunCommandWork>("test-run-command", "_invalid_");
        while (!wm.allChildrenDone())
        {
            clock.crank();
        }
        REQUIRE(w->getState() == BasicWork::WORK_FAILURE_RAISE);
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

TEST_CASE("sub-subwork items succed at the same time", "[work]")
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
