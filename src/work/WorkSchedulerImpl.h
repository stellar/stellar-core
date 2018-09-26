// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "work/WorkScheduler.h"

namespace stellar
{

class WorkSchedulerImpl : public WorkScheduler
{
    bool mScheduled{false}; // Ensure one task is scheduled at a time

  public:
    WorkSchedulerImpl(Application& app);
    virtual ~WorkSchedulerImpl();
    void registerCallback();

  protected:
    State doWork() override;
};
}
