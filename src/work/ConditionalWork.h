// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
#pragma once

#include "work/BasicWork.h"

namespace stellar
{

using ConditionFn = std::function<bool()>;

class ConditionalWork : public BasicWork
{
    ConditionFn mCondition;
    std::shared_ptr<BasicWork> mConditionedWork;
    std::chrono::milliseconds const mSleepDelay;
    std::unique_ptr<VirtualTimer> mSleepTimer;
    bool mWorkStarted{false};

  public:
    ConditionalWork(
        Application& app, std::string name, ConditionFn condition,
        std::shared_ptr<BasicWork> conditionedWork,
        std::chrono::milliseconds sleepTime = std::chrono::milliseconds(100));
    void shutdown() override;

  protected:
    BasicWork::State onRun() override;
    bool onAbort() override;
    void onReset() override;
};
}
