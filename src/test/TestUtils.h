#pragma once

// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketList.h"
#include "ledger/LedgerManagerImpl.h"
#include "main/ApplicationImpl.h"

namespace stellar
{

namespace testutil
{
void setCurrentLedgerVersion(LedgerManager& lm, uint32_t currentLedgerVersion);

class BucketListDepthModifier
{
    uint32_t const mPrevDepth;

  public:
    BucketListDepthModifier(uint32_t newDepth);

    ~BucketListDepthModifier();
};
}

template <typename T = ApplicationImpl>
std::shared_ptr<T>
createTestApplication(VirtualClock& clock, Config const& cfg)
{
    auto app = Application::create<T>(clock, cfg);
    auto& lm = app->getLedgerManager();
    lm.getCurrentLedgerHeader().baseFee = cfg.DESIRED_BASE_FEE;
    testutil::setCurrentLedgerVersion(lm, cfg.LEDGER_PROTOCOL_VERSION);
    return app;
}

time_t getTestDate(int day, int month, int year);
std::tm getTestDateTime(int day, int month, int year, int hour, int minute,
                        int second);
}
