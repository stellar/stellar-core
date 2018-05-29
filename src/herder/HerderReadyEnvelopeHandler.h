#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/ReadyEnvelopeHandler.h"

namespace stellar
{

class Application;

class HerderReadyEnvelopeHandler : public ReadyEnvelopeHandler
{
  public:
    explicit HerderReadyEnvelopeHandler(Application& app);
    ~HerderReadyEnvelopeHandler() = default;

    void readyEnvelopeAvailable();

  private:
    Application& mApp;
};
}
