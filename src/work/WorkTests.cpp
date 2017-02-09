// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "process/ProcessManager.h"
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
    Application::pointer appPtr = Application::create(clock, cfg);
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
    Application::pointer appPtr = Application::create(clock, cfg);
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
