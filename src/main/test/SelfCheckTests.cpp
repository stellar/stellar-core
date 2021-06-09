// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "history/HistoryArchiveManager.h"
#include "history/test/HistoryTestsUtils.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/ApplicationUtils.h"
#include "main/Config.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Logging.h"
#include "work/WorkScheduler.h"

using namespace stellar;
using namespace stellar::historytestutils;

// More extensive self-check tests (that check for caught damage) are in the
// ApplicationUtilsTests.cpp file, since they run the more extensive _offline_
// self-check mode. This file just tests that self-check mode runs periodically.

class SelfCheckMultiArchiveHistoryConfigurator
    : public TmpDirHistoryConfigurator
{
  public:
    Config&
    configure(Config& cfg, bool writable) const override
    {
        REQUIRE(writable);
        TmpDirHistoryConfigurator::configure(cfg, writable);
        // In accelerated-time mode we close every 1s and checkpoint every 8s.
        cfg.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = true;
        // In this test config we'll run self-check every 4 checkpoints (32s).
        cfg.AUTOMATIC_SELF_CHECK_PERIOD = std::chrono::seconds(32);
        return cfg;
    }
};

TEST_CASE("online self-check runs on a schedule", "[selfcheck]")
{
    Config chkConfig;
    size_t n = 5;
    auto configurator =
        std::make_shared<SelfCheckMultiArchiveHistoryConfigurator>();
    CatchupSimulation catchupSimulation{VirtualClock::VIRTUAL_TIME,
                                        configurator};
    auto& app = catchupSimulation.getApp();
    auto& clock = app.getClock();
    auto last = clock.now();
    auto& meter =
        app.getMetrics().NewMeter({"history", "check", "success"}, "event");
    while (meter.count() < n)
    {
        catchupSimulation.getClock().crank(false);
        if ((clock.now() - last) > std::chrono::seconds(1))
        {
            catchupSimulation.generateRandomLedger();
            last = clock.now();
        }
    }
    REQUIRE(meter.count() == n);
}