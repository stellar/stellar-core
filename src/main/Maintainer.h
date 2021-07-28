#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Timer.h"

#include <cstdint>

namespace stellar
{

class Application;

class Maintainer
{
  public:
    explicit Maintainer(Application& app);

    // start automatic maintenance according to app.getConfig()
    void start();

    // removes maximum count entries from tables like txhistory or scphistory
    void performMaintenance(uint32_t count);

    void shutdown();

  private:
    Application& mApp;
    VirtualTimer mTimer;
    bool mShuttingDown;

    void scheduleMaintenance();
    void tick();
};
}
