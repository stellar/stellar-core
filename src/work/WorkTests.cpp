// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/BatchDownloadWork.h"
#include "lib/catch.hpp"
#include "lib/util/format.h"
#include "main/Application.h"
#include "main/Config.h"
#include "process/ProcessManager.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Fs.h"
#include "work/WorkScheduler.h"

#include "historywork/RunCommandWork.h"
#include <cstdio>
#include <fstream>
#include <random>
#include <xdrpp/autocheck.h>

using namespace stellar;

class TestBasicWork : public BasicWork
{
    bool mFail;

  public:
    int const mNumSteps;
    int mCount;
    int mRunningCount{0};
    int mSuccessCount{0};
    int mFailureCount{0};
    int mRetryCount{0};

    TestBasicWork(Application& app, std::string name, bool fail = false,
                  int steps = 3, size_t retries = RETRY_ONCE)
        : BasicWork(app, std::move(name), retries)
        , mFail(fail)
        , mNumSteps(steps)
        , mCount(steps)
    {
    }

    void
    forceWakeUp()
    {
        wakeUp();
    }

  protected:
    BasicWork::State
    onRun() override
    {
        CLOG(DEBUG, "Work") << "Running " << getName();
        mRunningCount++;
        if (mCount-- > 0)
        {
            return State::WORK_RUNNING;
        }
        return mFail ? State::WORK_FAILURE : State::WORK_SUCCESS;
    }

    bool
    onAbort() override
    {
        CLOG(DEBUG, "Work") << "Aborting " << getName();
        return true;
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
    onReset() override
    {
        mCount = mNumSteps;
    }
};

TEST_CASE("BasicWork test", "[work]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application::pointer appPtr = createTestApplication(clock, cfg);
    auto& wm = appPtr->getWorkScheduler();

    auto checkSuccess = [](TestBasicWork const& w) {
        REQUIRE(w.mRunningCount == w.mNumSteps + 1);
        REQUIRE(w.mSuccessCount == 1);
        REQUIRE(w.mFailureCount == 0);
        REQUIRE(w.mRetryCount == 0);
    };

    SECTION("basic one work")
    {
        auto w = wm.scheduleWork<TestBasicWork>("test-work");
        while (!wm.allChildrenSuccessful())
        {
            clock.crank();
        }
        checkSuccess(*w);
    }

    SECTION("2 works round robin")
    {
        auto w1 = wm.scheduleWork<TestBasicWork>("test-work1");
        auto w2 = wm.scheduleWork<TestBasicWork>("test-work2");
        while (!wm.allChildrenSuccessful())
        {
            clock.crank();
        }
        checkSuccess(*w1);
        checkSuccess(*w2);
    }

    SECTION("another work added midway")
    {
        auto w1 = wm.scheduleWork<TestBasicWork>("test-work1");
        auto w2 = wm.scheduleWork<TestBasicWork>("test-work2");
        std::shared_ptr<TestBasicWork> w3;
        while (!wm.allChildrenSuccessful())
        {
            clock.crank();
            if (!w3)
            {
                w3 = wm.scheduleWork<TestBasicWork>("test-work3");
            };
        }
        checkSuccess(*w1);
        checkSuccess(*w2);
        checkSuccess(*w3);
    }

    SECTION("new work wakes up scheduler")
    {
        // Scheduler wakes up to execute work,
        // and goes back into a waiting state once child is done
        while (wm.getState() != TestBasicWork::State::WORK_WAITING)
        {
            clock.crank();
        }

        auto w = wm.scheduleWork<TestBasicWork>("test-work");
        REQUIRE(wm.getState() == TestBasicWork::State::WORK_RUNNING);
        while (!wm.allChildrenSuccessful())
        {
            clock.crank();
        }
        checkSuccess(*w);
    }

    SECTION("work retries and fails")
    {
        auto w = wm.scheduleWork<TestBasicWork>("test-work", true);
        while (!wm.allChildrenDone())
        {
            clock.crank();
        }
        REQUIRE(w->getState() == TestBasicWork::State::WORK_FAILURE);

        // Work of 3 steps retried once
        REQUIRE(w->mRunningCount ==
                (w->mNumSteps + 1) * (BasicWork::RETRY_ONCE + 1));
        REQUIRE(w->mSuccessCount == 0);
        REQUIRE(w->mFailureCount == 1);
        REQUIRE(w->mRetryCount == BasicWork::RETRY_ONCE);
    }
}

// Test work to allow flexibility of adding work trees on individual test level
// Note RETRY_NEVER setting, as this work will not properly retry since no work
// is added in `doWork`
class TestWork : public Work
{
  public:
    int mRunningCount{0};
    int mSuccessCount{0};
    int mFailureCount{0};
    int mRetryCount{0};

    TestWork(Application& app, std::string name)
        : Work(app, std::move(name), RETRY_NEVER)
    {
    }

    BasicWork::State
    doWork() override
    {
        ++mRunningCount;
        return WorkUtils::checkChildrenStatus(*this);
    }

    template <typename T, typename... Args>
    std::shared_ptr<T>
    addTestWork(Args&&... args)
    {
        return addWork<T>(std::forward<Args>(args)...);
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
};

TEST_CASE("work with children", "[work]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application::pointer appPtr = createTestApplication(clock, cfg);
    auto& wm = appPtr->getWorkScheduler();

    auto w1 = wm.scheduleWork<TestWork>("test-work1");
    auto w2 = wm.scheduleWork<TestWork>("test-work2");
    auto l1 = w1->addTestWork<TestBasicWork>("leaf-work");
    auto l2 = w1->addTestWork<TestBasicWork>("leaf-work-2");

    SECTION("success")
    {
        while (!wm.allChildrenSuccessful())
        {
            clock.crank();
        }
        REQUIRE(l1->getState() == TestBasicWork::State::WORK_SUCCESS);
        REQUIRE(l2->getState() == TestBasicWork::State::WORK_SUCCESS);
        REQUIRE(w1->getState() == TestBasicWork::State::WORK_SUCCESS);
        REQUIRE(w2->getState() == TestBasicWork::State::WORK_SUCCESS);
        REQUIRE(wm.getState() == TestBasicWork::State::WORK_WAITING);
    }
    SECTION("child failed")
    {
        auto l3 = w2->addTestWork<TestBasicWork>("leaf-work3", true);
        while (!wm.allChildrenDone())
        {
            clock.crank();
        }
        REQUIRE(l1->getState() == TestBasicWork::State::WORK_SUCCESS);
        REQUIRE(l2->getState() == TestBasicWork::State::WORK_SUCCESS);
        REQUIRE(w1->getState() == TestBasicWork::State::WORK_SUCCESS);
        REQUIRE(w2->getState() == TestBasicWork::State::WORK_FAILURE);
        REQUIRE(l3->getState() == TestBasicWork::State::WORK_FAILURE);
        REQUIRE(wm.getState() == TestBasicWork::State::WORK_WAITING);
    }
    SECTION("child failed so parent aborts other child")
    {
        auto l3 = w2->addTestWork<TestBasicWork>("leaf-work3", true, 3,
                                                 TestBasicWork::RETRY_NEVER);
        auto l4 = w2->addTestWork<TestBasicWork>("leaf-work4", false, 100);
        while (!wm.allChildrenDone())
        {
            clock.crank();
        }
        REQUIRE(l1->getState() == TestBasicWork::State::WORK_SUCCESS);
        REQUIRE(l2->getState() == TestBasicWork::State::WORK_SUCCESS);
        REQUIRE(w1->getState() == TestBasicWork::State::WORK_SUCCESS);
        REQUIRE(w2->getState() == TestBasicWork::State::WORK_FAILURE);
        REQUIRE(l3->getState() == TestBasicWork::State::WORK_FAILURE);
        REQUIRE(l4->getState() == TestBasicWork::State::WORK_ABORTED);
        REQUIRE(wm.getState() == TestBasicWork::State::WORK_WAITING);
    }
    SECTION("work scheduler shutdown")
    {
        wm.shutdown();
        // on shutdown, work goes into ABORTING state, wakeUp
        // should be a no-op
        REQUIRE_NOTHROW(l1->forceWakeUp());
        REQUIRE_NOTHROW(l2->forceWakeUp());

        while (!wm.allChildrenDone())
        {
            clock.crank();
        }

        REQUIRE(wm.getState() == TestBasicWork::State::WORK_ABORTED);
        REQUIRE(w1->getState() == TestBasicWork::State::WORK_ABORTED);
        REQUIRE(w2->getState() == TestBasicWork::State::WORK_ABORTED);
        REQUIRE(l1->getState() == TestBasicWork::State::WORK_ABORTED);
        REQUIRE(l2->getState() == TestBasicWork::State::WORK_ABORTED);
    }
}

TEST_CASE("work scheduling and run count", "[work]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application::pointer appPtr = createTestApplication(clock, cfg);

    // Level 0
    auto& wm = appPtr->getWorkScheduler();
    auto mainWork = wm.scheduleWork<TestWork>("main-work");

    SECTION("multi-level tree")
    {
        // Level 1
        auto w1 = mainWork->addTestWork<TestBasicWork>("3-step-work");
        auto w2 = mainWork->addTestWork<TestWork>("wait-for-children-work");

        // Level 2
        auto c1 = w2->addTestWork<TestBasicWork>("child-3-step-work");
        auto c2 = w2->addTestWork<TestBasicWork>("other-child-3-step-work");

        // There are three levels of work that look like this
        //     mainWork
        //       /\
        //     w1  w2
        //         /\
        //       c1 c2
        while (!wm.allChildrenSuccessful())
        {
            clock.crank();
        }
        REQUIRE(w1->mRunningCount == w1->mNumSteps + 1);
        REQUIRE(w2->mRunningCount == c1->mNumSteps + 1);
        REQUIRE(c1->mRunningCount == c1->mNumSteps + 1);
        REQUIRE(c2->mRunningCount == c2->mNumSteps + 1);

        // mainWork has to execute however many times the children executed
        REQUIRE(mainWork->mRunningCount ==
                (c1->mRunningCount * 2 + w2->mRunningCount));
    }
    SECTION("multi-level tree more leaves do not affect scheduling")
    {
        // There are three levels of work that look like this
        //     mainWork
        //       / \
        //     w1   w2
        //     /\   /\
        //   c1 c2 c3 c4
        auto w1 = mainWork->addTestWork<TestWork>("test-work-1");
        auto w2 = mainWork->addTestWork<TestWork>("test-work-2");

        auto c1 = w1->addTestWork<TestBasicWork>("child-3-step-work");
        auto c2 = w1->addTestWork<TestBasicWork>("other-child-3-step-work");
        auto c3 = w2->addTestWork<TestBasicWork>("2-child-3-step-work");
        auto c4 = w2->addTestWork<TestBasicWork>("2-other-child-3-step-work");

        while (!wm.allChildrenSuccessful())
        {
            clock.crank();
        }

        REQUIRE(w2->mRunningCount == c1->mNumSteps + 1);
        REQUIRE(c1->mRunningCount == c1->mNumSteps + 1);
        REQUIRE(c2->mRunningCount == c2->mNumSteps + 1);
        REQUIRE(mainWork->mRunningCount ==
                (c2->mRunningCount * 2 + w2->mRunningCount));
    }
}

TEST_CASE("work scheduling compare trees", "[work]")
{
    // This test is mainly to show the worst case scenario with
    // work trees, and exhibit the overhead that is possible to create

    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application::pointer appPtr = createTestApplication(clock, cfg);

    auto& wm = appPtr->getWorkScheduler();

    SECTION("linked list structure")
    {
        // w2 and w1 have to wait for w3 to execute
        //       WS (Work Scheduler)
        //      /
        //     w1
        //    /
        //   w2
        //  /
        // w3
        auto w1 = wm.scheduleWork<TestWork>("work-1");
        auto w2 = w1->addTestWork<TestWork>("work-2");
        auto w3 = w2->addTestWork<TestBasicWork>("work-3");

        while (!wm.allChildrenSuccessful())
        {
            clock.crank();
        }
        REQUIRE(w3->mRunningCount == w3->mNumSteps + 1);
        REQUIRE(w2->mRunningCount == w3->mRunningCount);
        REQUIRE(w1->mRunningCount == w2->mRunningCount + w3->mRunningCount);
    }

    SECTION("flat work sequence")
    {
        // w2 and w1 still need to wait for w3, however now
        // redundant runs are avoided due to WorkSequence utilization
        //       WS (Work Scheduler)
        //       |
        //      seq
        //     / | \
        //   w3  w2 w1 (use work sequence)

        auto w1 = std::make_shared<TestWork>(*appPtr, "work-1");
        auto w2 = std::make_shared<TestWork>(*appPtr, "work-2");
        auto w3 = std::make_shared<TestBasicWork>(*appPtr, "work-3");

        // Same way to enforce sequence of works:
        std::vector<std::shared_ptr<BasicWork>> seq{w3, w2, w1};

        auto sw = wm.scheduleWork<WorkSequence>("test-work-sequence", seq);
        while (!wm.allChildrenSuccessful())
        {
            clock.crank();
        }

        REQUIRE(w3->mRunningCount == w3->mNumSteps + 1);
        REQUIRE(w2->mRunningCount == 1);
        REQUIRE(w1->mRunningCount == 1);
    }
}

class TestRunCommandWork : public RunCommandWork
{
    std::string mCommand;

  public:
    TestRunCommandWork(Application& app, std::string name, std::string command)
        : RunCommandWork(app, std::move(name)), mCommand(std::move(command))
    {
    }
    ~TestRunCommandWork() override = default;

    CommandInfo
    getCommand() override
    {
        return CommandInfo{mCommand, std::string()};
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
        wm.scheduleWork<TestRunCommandWork>("test-run-command", "date");
        while (!wm.allChildrenSuccessful())
        {
            clock.crank();
        }
    }
    SECTION("round robin with other work")
    {
        wm.scheduleWork<TestRunCommandWork>("test-run-command", "date");
        wm.scheduleWork<TestBasicWork>("test-work");
        while (!wm.allChildrenSuccessful())
        {
            clock.crank();
        }
    }
    SECTION("invalid run command")
    {
        auto w = wm.scheduleWork<TestRunCommandWork>("test-run-command",
                                                     "_invalid_");
        while (!wm.allChildrenDone())
        {
            clock.crank();
        }
        REQUIRE(w->getState() == TestBasicWork::State::WORK_FAILURE);
    }

    SECTION("run command aborted")
    {
#ifdef _WIN32
        std::string command = "waitfor /T 10 pause";
#else
        std::string command = "sleep 10";
#endif
        auto w =
            wm.scheduleWork<TestRunCommandWork>("test-run-command", command);
        while (w->getState() != TestBasicWork::State::WORK_WAITING)
        {
            clock.crank();
        }

        REQUIRE(appPtr->getProcessManager().getNumRunningProcesses());
        wm.shutdown();

        while (wm.getState() != TestBasicWork::State::WORK_ABORTED)
        {
            clock.crank();
        }

        REQUIRE(w->getState() == TestBasicWork::State::WORK_ABORTED);
        REQUIRE(!appPtr->getProcessManager().getNumRunningProcesses());
    }
}

TEST_CASE("WorkSequence test", "[work]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application::pointer appPtr = createTestApplication(clock, cfg);
    auto& wm = appPtr->getWorkScheduler();

    SECTION("basic")
    {
        auto w1 = std::make_shared<TestBasicWork>(*appPtr, "test-work-1");
        auto w2 = std::make_shared<TestBasicWork>(*appPtr, "test-work-2");
        auto w3 = std::make_shared<TestBasicWork>(*appPtr, "test-work-3");
        std::vector<std::shared_ptr<BasicWork>> seq{w1, w2, w3};

        auto work = wm.scheduleWork<WorkSequence>("test-work-sequence", seq);
        while (!wm.allChildrenSuccessful())
        {
            if (!w1->mSuccessCount)
            {
                REQUIRE(!w2->mRunningCount);
                REQUIRE(!w3->mRunningCount);
            }
            else if (!w2->mSuccessCount)
            {
                REQUIRE(w1->mSuccessCount);
                REQUIRE(!w3->mRunningCount);
            }
            else
            {
                REQUIRE(w1->mSuccessCount);
                REQUIRE(w2->mSuccessCount);
            }
            clock.crank();
        }
    }
    SECTION("WorkSequence is empty")
    {
        std::vector<std::shared_ptr<BasicWork>> seq{};
        auto work2 = wm.scheduleWork<WorkSequence>("test-work-sequence-2", seq);
        while (!wm.allChildrenSuccessful())
        {
            clock.crank();
        }
        REQUIRE(work2->getState() == TestBasicWork::State::WORK_SUCCESS);
    }
    SECTION("first work fails")
    {
        auto w1 = std::make_shared<TestBasicWork>(*appPtr, "test-work-1", true);
        auto w2 = std::make_shared<TestBasicWork>(*appPtr, "test-work-2");
        auto w3 = std::make_shared<TestBasicWork>(*appPtr, "test-work-3");
        std::vector<std::shared_ptr<BasicWork>> seq{w1, w2, w3};

        auto work = wm.executeWork<WorkSequence>("test-work-sequence", seq);

        // w1 should run and retry
        auto w1RunCount = (w1->mNumSteps + 1) * (BasicWork::RETRY_ONCE + 1);
        REQUIRE(w1->mRunningCount == w1RunCount * (BasicWork::RETRY_A_FEW + 1));
        REQUIRE(w1->getState() == TestBasicWork::State::WORK_FAILURE);

        // Ensure the rest of sequence is NOT run.
        REQUIRE_FALSE(w2->mRunningCount);
        REQUIRE_FALSE(w2->mSuccessCount);
        REQUIRE_FALSE(w2->mFailureCount);
        REQUIRE_FALSE(w2->mRetryCount);
        REQUIRE_FALSE(w3->mRunningCount);
        REQUIRE_FALSE(w3->mSuccessCount);
        REQUIRE_FALSE(w3->mFailureCount);
        REQUIRE_FALSE(w3->mRetryCount);
        REQUIRE(work->getState() == TestBasicWork::State::WORK_FAILURE);
    }
    SECTION("fails midway")
    {
        auto w1 = std::make_shared<TestBasicWork>(*appPtr, "test-work-1");
        auto w2 = std::make_shared<TestBasicWork>(*appPtr, "test-work-2", true);
        auto w3 = std::make_shared<TestBasicWork>(*appPtr, "test-work-3");
        std::vector<std::shared_ptr<BasicWork>> seq{w1, w2, w3};

        auto work = wm.executeWork<WorkSequence>("test-work-sequence", seq);

        // w2 should run and retry
        auto w2RunCount = (w2->mNumSteps + 1) * (BasicWork::RETRY_ONCE + 1);
        REQUIRE(w2->mRunningCount == w2RunCount * (BasicWork::RETRY_A_FEW + 1));
        REQUIRE(w2->getState() == BasicWork::State::WORK_FAILURE);

        REQUIRE(w1->getState() == TestBasicWork::State::WORK_SUCCESS);
        REQUIRE(w1->mRunningCount);
        REQUIRE(w1->mSuccessCount);
        REQUIRE_FALSE(w1->mFailureCount);
        REQUIRE_FALSE(w1->mRetryCount);
        REQUIRE_FALSE(w3->mRunningCount);
        REQUIRE_FALSE(w3->mSuccessCount);
        REQUIRE_FALSE(w3->mFailureCount);
        REQUIRE_FALSE(w3->mRetryCount);
        REQUIRE(work->getState() == TestBasicWork::State::WORK_FAILURE);
    }
    SECTION("work scheduler shutdown")
    {
        // Queue up some long-running works to span over multiple cranks
        auto w1 =
            std::make_shared<TestBasicWork>(*appPtr, "test-work-1", false, 100);
        auto w2 =
            std::make_shared<TestBasicWork>(*appPtr, "test-work-2", false, 100);
        auto w3 =
            std::make_shared<TestBasicWork>(*appPtr, "test-work-3", false, 100);
        std::vector<std::shared_ptr<BasicWork>> seq{w1, w2, w3};

        auto work = wm.scheduleWork<WorkSequence>("test-work-sequence", seq);
        clock.crank();
        CHECK(!wm.allChildrenDone());
        wm.shutdown();

        while (!wm.allChildrenDone())
        {
            clock.crank();
        }

        REQUIRE(work->getState() == TestBasicWork::State::WORK_ABORTED);
        REQUIRE(wm.getState() == TestBasicWork::State::WORK_ABORTED);
    }
}

class TestBatchWork : public BatchWork
{
  public:
    int mCount{0};
    TestBatchWork(Application& app, std::string const& name)
        : BatchWork(app, name)
    {
    }

  protected:
    bool
    hasNext() const override
    {
        return mCount < mApp.getConfig().MAX_CONCURRENT_SUBPROCESSES * 2;
    }

    void
    resetIter() override
    {
        mCount = 0;
    }

    std::shared_ptr<BasicWork>
    yieldMoreWork() override
    {
        return addWork<TestBasicWork>(fmt::format("child-{:d}", mCount++));
    }
};

TEST_CASE("Work batching", "[batching][work]")
{
    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, getTestConfig());
    auto& wm = app->getWorkScheduler();

    auto testBatch = wm.scheduleWork<TestBatchWork>("test-batch");
    while (!clock.getIOService().stopped() && !wm.allChildrenDone())
    {
        clock.crank(true);
        REQUIRE(testBatch->getNumWorksInBatch() <=
                app->getConfig().MAX_CONCURRENT_SUBPROCESSES);
    }
    REQUIRE(testBatch->getState() == TestBasicWork::State::WORK_SUCCESS);
}
