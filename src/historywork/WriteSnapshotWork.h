// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "work/Work.h"

namespace stellar
{

struct StateSnapshot;

class WriteSnapshotWork : public BasicWork
{
    std::shared_ptr<StateSnapshot> mSnapshot;
    bool mDone{false};
    bool mSuccess{true};

  public:
    WriteSnapshotWork(Application& app,
                      std::shared_ptr<StateSnapshot> snapshot);
    ~WriteSnapshotWork() = default;

  protected:
    State onRun() override;
    bool
    onAbort() override
    {
        return true;
    };
};
}
