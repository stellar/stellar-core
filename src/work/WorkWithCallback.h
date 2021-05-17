// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
#pragma once

#include "work/BasicWork.h"

namespace stellar
{

class WorkWithCallback : public BasicWork
{
    std::function<bool(Application& app)> const mCallback;

  public:
    WorkWithCallback(Application& app, std::string name,
                     std::function<bool(Application& app)> callback);

  protected:
    BasicWork::State onRun() override;
    bool
    onAbort() override
    {
        return true;
    };
};
}
