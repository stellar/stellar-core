// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "work/Work.h"

namespace stellar
{

struct StateSnapshot;

class ResolveSnapshotWork : public Work
{
    std::shared_ptr<StateSnapshot> mSnapshot;

  public:
    ResolveSnapshotWork(Application& app, WorkParent& parent,
                        std::shared_ptr<StateSnapshot> snapshot);
    ~ResolveSnapshotWork();
    void onRun() override;
};
}
