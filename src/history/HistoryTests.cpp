// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketManager.h"
#include "catchup/CatchupWorkTests.h"
#include "history/HistoryManager.h"
#include "history/HistoryTestsUtils.h"
#include "historywork/GetHistoryArchiveStateWork.h"
#include "historywork/GunzipFileWork.h"
#include "historywork/GzipFileWork.h"
#include "historywork/PutHistoryArchiveStateWork.h"
#include "ledger/LedgerManager.h"
#include "main/ExternalQueue.h"
#include "main/PersistentState.h"
#include "process/ProcessManager.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Fs.h"
#include "work/WorkManager.h"

#include <lib/catch.hpp>
#include <lib/util/format.h>

using namespace stellar;
using namespace historytestutils;

TEST_CASE("next checkpoint ledger", "[history]")
{
    CatchupSimulation catchupSimulation{};
    HistoryManager& hm = catchupSimulation.getApp().getHistoryManager();
    CHECK(hm.nextCheckpointLedger(0) == 64);
    CHECK(hm.nextCheckpointLedger(1) == 64);
    CHECK(hm.nextCheckpointLedger(32) == 64);
    CHECK(hm.nextCheckpointLedger(62) == 64);
    CHECK(hm.nextCheckpointLedger(63) == 64);
    CHECK(hm.nextCheckpointLedger(64) == 64);
    CHECK(hm.nextCheckpointLedger(65) == 128);
    CHECK(hm.nextCheckpointLedger(66) == 128);
    CHECK(hm.nextCheckpointLedger(126) == 128);
    CHECK(hm.nextCheckpointLedger(127) == 128);
    CHECK(hm.nextCheckpointLedger(128) == 128);
    CHECK(hm.nextCheckpointLedger(129) == 192);
    CHECK(hm.nextCheckpointLedger(130) == 192);
}

TEST_CASE("HistoryManager::compress", "[history]")
{
    CatchupSimulation catchupSimulation{};

    std::string s = "hello there";
    HistoryManager& hm = catchupSimulation.getApp().getHistoryManager();
    std::string fname = hm.localFilename("compressme");
    {
        std::ofstream out(fname, std::ofstream::binary);
        out.write(s.data(), s.size());
    }
    std::string compressed = fname + ".gz";
    auto& wm = catchupSimulation.getApp().getWorkManager();
    auto g = wm.executeWork<GzipFileWork>(fname);
    REQUIRE(g->getState() == Work::WORK_SUCCESS);
    REQUIRE(!fs::exists(fname));
    REQUIRE(fs::exists(compressed));

    auto u = wm.executeWork<GunzipFileWork>(compressed);
    REQUIRE(u->getState() == Work::WORK_SUCCESS);
    REQUIRE(fs::exists(fname));
    REQUIRE(!fs::exists(compressed));
}

TEST_CASE("HistoryArchiveState::get_put", "[history]")
{
    CatchupSimulation catchupSimulation{};

    HistoryArchiveState has;
    has.currentLedger = 0x1234;

    auto i = catchupSimulation.getApp().getConfig().HISTORY.find("test");
    REQUIRE(i != catchupSimulation.getApp().getConfig().HISTORY.end());
    auto archive = i->second;

    has.resolveAllFutures();

    auto& wm = catchupSimulation.getApp().getWorkManager();
    auto put = wm.executeWork<PutHistoryArchiveStateWork>(has, archive);
    REQUIRE(put->getState() == Work::WORK_SUCCESS);

    HistoryArchiveState has2;
    auto get = wm.executeWork<GetHistoryArchiveStateWork>(
        "get-history-archive-state", has2, 0, std::chrono::seconds(0), archive);
    REQUIRE(get->getState() == Work::WORK_SUCCESS);
    REQUIRE(has2.currentLedger == 0x1234);
}

TEST_CASE("History publish", "[history]")
{
    CatchupSimulation catchupSimulation{};

    catchupSimulation.generateAndPublishInitialHistory(1);
}

static std::string
resumeModeName(uint32_t count)
{
    switch (count)
    {
    case 0:
        return "CATCHUP_MINIMAL";
    case std::numeric_limits<uint32_t>::max():
        return "CATCHUP_COMPLETE";
    default:
        return "CATCHUP_RECENT";
    }
}

static std::string
dbModeName(Config::TestDbMode mode)
{
    switch (mode)
    {
    case Config::TESTDB_IN_MEMORY_SQLITE:
        return "TESTDB_IN_MEMORY_SQLITE";
    case Config::TESTDB_ON_DISK_SQLITE:
        return "TESTDB_ON_DISK_SQLITE";
#ifdef USE_POSTGRES
    case Config::TESTDB_POSTGRESQL:
        return "TESTDB_POSTGRESQL";
#endif
    default:
        abort();
    }
}

TEST_CASE("Full history catchup", "[history][historycatchup]")
{
    CatchupSimulation catchupSimulation{};

    catchupSimulation.generateAndPublishInitialHistory(3);

    uint32_t initLedger =
        catchupSimulation.getApp().getLedgerManager().getLastClosedLedgerNum() -
        1;

    std::vector<Application::pointer> apps;

    std::vector<uint32_t> counts = {0, std::numeric_limits<uint32_t>::max(),
                                    60};

    std::vector<Config::TestDbMode> dbModes = {Config::TESTDB_IN_MEMORY_SQLITE,
                                               Config::TESTDB_ON_DISK_SQLITE};
#ifdef USE_POSTGRES
    if (!force_sqlite)
        dbModes.push_back(Config::TESTDB_POSTGRESQL);
#endif

    for (auto dbMode : dbModes)
    {
        for (auto count : counts)
        {
            auto a = catchupSimulation.catchupNewApplication(
                initLedger, count, false, dbMode,
                std::string("full, ") + resumeModeName(count) + ", " +
                    dbModeName(dbMode));
            apps.push_back(a);
        }
    }
}

TEST_CASE("History publish queueing", "[history][historydelay][historycatchup]")
{
    CatchupSimulation catchupSimulation{};

    catchupSimulation.generateAndPublishInitialHistory(1);

    auto& hm = catchupSimulation.getApp().getHistoryManager();

    while (hm.getPublishQueueCount() < 4)
    {
        catchupSimulation.generateRandomLedger();
    }
    // One more ledger is needed to close as stellar-core only publishes to
    // just-before-LCL
    catchupSimulation.generateRandomLedger();

    CLOG(INFO, "History") << "publish-delay count: "
                          << hm.getPublishDelayCount();

    while (hm.getPublishSuccessCount() < hm.getPublishQueueCount())
    {
        CHECK(hm.getPublishFailureCount() == 0);
        catchupSimulation.getApp().getClock().crank(true);
    }

    auto initLedger =
        catchupSimulation.getApp().getLedgerManager().getLastClosedLedgerNum() -
        1;
    auto app2 = catchupSimulation.catchupNewApplication(
        initLedger, std::numeric_limits<uint32_t>::max(), false,
        Config::TESTDB_IN_MEMORY_SQLITE,
        std::string("Catchup to delayed history"));
    CHECK(app2->getLedgerManager().getLedgerNum() ==
          catchupSimulation.getApp().getLedgerManager().getLedgerNum());
}

TEST_CASE("History prefix catchup", "[history][historycatchup][prefixcatchup]")
{
    CatchupSimulation catchupSimulation{};

    catchupSimulation.generateAndPublishInitialHistory(3);
    std::vector<Application::pointer> apps;

    // First attempt catchup to 10, prefix of 64. Should round up to 64.
    // Should replay the 64th (since it gets externalized) and land on 65.
    auto a = catchupSimulation.catchupNewApplication(
        10, std::numeric_limits<uint32_t>::max(), false,
        Config::TESTDB_IN_MEMORY_SQLITE,
        std::string("Catchup to prefix of published history"));
    apps.push_back(a);
    uint32_t freq = apps.back()->getHistoryManager().getCheckpointFrequency();
    CHECK(apps.back()->getLedgerManager().getLedgerNum() == freq + 1);

    // Then attempt catchup to 74, prefix of 128. Should round up to 128.
    // Should replay the 64th (since it gets externalized) and land on 129.
    a = catchupSimulation.catchupNewApplication(
        freq + 10, std::numeric_limits<uint32_t>::max(), false,
        Config::TESTDB_IN_MEMORY_SQLITE,
        std::string("Catchup to second prefix of published history"));
    apps.push_back(a);
    CHECK(apps.back()->getLedgerManager().getLedgerNum() == 2 * freq + 1);
}

TEST_CASE("Publish/catchup alternation, with stall",
          "[history][historycatchup][catchupalternation]")
{
    CatchupSimulation catchupSimulation{};

    // Publish in app, catch up in app2 and app3.
    // App2 will catch up using CATCHUP_COMPLETE, app3 will use
    // CATCHUP_MINIMAL.
    catchupSimulation.generateAndPublishInitialHistory(3);

    Application::pointer app2, app3;

    auto& lm = catchupSimulation.getApp().getLedgerManager();

    uint32_t initLedger = lm.getLastClosedLedgerNum() - 1;

    app2 = catchupSimulation.catchupNewApplication(
        initLedger, std::numeric_limits<uint32_t>::max(), false,
        Config::TESTDB_IN_MEMORY_SQLITE, std::string("app2"));

    app3 = catchupSimulation.catchupNewApplication(
        initLedger, 0, false, Config::TESTDB_IN_MEMORY_SQLITE,
        std::string("app3"));

    CHECK(app2->getLedgerManager().getLedgerNum() == lm.getLedgerNum());
    CHECK(app3->getLedgerManager().getLedgerNum() == lm.getLedgerNum());

    for (size_t i = 1; i < 4; ++i)
    {
        // Now alternate between publishing new stuff and catching up to it.
        catchupSimulation.generateAndPublishHistory(i);

        initLedger = lm.getLastClosedLedgerNum() - 1;

        catchupSimulation.catchupApplication(
            initLedger, std::numeric_limits<uint32_t>::max(), false, app2);
        catchupSimulation.catchupApplication(initLedger, 0, false, app3);

        CHECK(app2->getLedgerManager().getLedgerNum() == lm.getLedgerNum());
        CHECK(app3->getLedgerManager().getLedgerNum() == lm.getLedgerNum());
    }

    // By now we should have had 3 + 1 + 2 + 3 = 9 publishes, and should
    // have advanced 1 ledger in to the 9th block.
    uint32_t freq = app2->getHistoryManager().getCheckpointFrequency();
    CHECK(app2->getLedgerManager().getLedgerNum() == 9 * freq + 1);
    CHECK(app3->getLedgerManager().getLedgerNum() == 9 * freq + 1);

    // Finally, publish a little more history than the last publish-point
    // but not enough to get to the _next_ publish-point:

    catchupSimulation.generateRandomLedger();
    catchupSimulation.generateRandomLedger();
    catchupSimulation.generateRandomLedger();

    // Attempting to catch up here should _stall_. We evaluate stalling
    // by providing 30 cranks of the event loop and assuming that failure
    // to catch up within that time means 'stalled'.

    initLedger = lm.getLastClosedLedgerNum() - 1;

    REQUIRE(!catchupSimulation.catchupApplication(
        initLedger, std::numeric_limits<uint32_t>::max(), false, app2));
    REQUIRE(!catchupSimulation.catchupApplication(initLedger, 0, false, app3));

    // Now complete this publish cycle and confirm that the stalled apps
    // will catch up.
    catchupSimulation.generateAndPublishHistory(1);
    REQUIRE(catchupSimulation.catchupApplication(
        initLedger, std::numeric_limits<uint32_t>::max(), false, app2, false));
    REQUIRE(catchupSimulation.catchupApplication(initLedger, 0, false, app3,
                                                 false));
}

TEST_CASE("Repair missing buckets via history",
          "[history][historybucketrepair]")
{
    CatchupSimulation catchupSimulation{};

    catchupSimulation.generateAndPublishInitialHistory(1);

    // Forcibly resolve any merges in progress, so we have a calm state to
    // repair;
    // NB: we cannot repair lost buckets from merges-in-progress, as they're
    // not
    // necessarily _published_ anywhere.
    HistoryArchiveState has(
        catchupSimulation.getApp().getLedgerManager().getLastClosedLedgerNum() -
            1,
        catchupSimulation.getBucketListAtLastPublish());
    has.resolveAllFutures();
    auto state = has.toString();

    auto cfg2 = getTestConfig(1);
    cfg2.BUCKET_DIR_PATH += "2";
    auto app2 = createTestApplication(
        catchupSimulation.getClock(),
        catchupSimulation.getHistoryConfigurator().configure(cfg2, false));
    app2->getPersistentState().setState(PersistentState::kHistoryArchiveState,
                                        state);

    app2->start();

    auto hash1 = catchupSimulation.getBucketListAtLastPublish().getHash();
    auto hash2 = app2->getBucketManager().getBucketList().getHash();
    CHECK(hash1 == hash2);
}

TEST_CASE("Repair missing buckets fails", "[history][historybucketrepair]")
{
    CatchupSimulation catchupSimulation{};

    catchupSimulation.generateAndPublishInitialHistory(1);

    // Forcibly resolve any merges in progress, so we have a calm state to
    // repair;
    // NB: we cannot repair lost buckets from merges-in-progress, as they're
    // not
    // necessarily _published_ anywhere.
    HistoryArchiveState has(
        catchupSimulation.getApp().getLedgerManager().getLastClosedLedgerNum() -
            1,
        catchupSimulation.getBucketListAtLastPublish());
    has.resolveAllFutures();
    auto state = has.toString();

    // Delete buckets from the archive before proceding.
    // This means startup will fail.
    auto dir = catchupSimulation.getHistoryConfigurator().getArchiveDirName();
    REQUIRE(!dir.empty());
    fs::deltree(dir + "/bucket");

    auto cfg2 = getTestConfig(1);
    cfg2.BUCKET_DIR_PATH += "2";
    auto app2 = createTestApplication(
        catchupSimulation.getClock(),
        catchupSimulation.getHistoryConfigurator().configure(cfg2, false));
    app2->getPersistentState().setState(PersistentState::kHistoryArchiveState,
                                        state);

    REQUIRE_THROWS(app2->start());

    while (app2->getProcessManager().getNumRunningProcesses() != 0)
    {
        try
        {
            app2->getClock().crank(false);
        }
        catch (...)
        {
            // see https://github.com/stellar/stellar-core/issues/1250
            // we expect to get "Unable to restore last-known ledger state"
            // several more times
        }
    }
}

TEST_CASE("Publish/catchup via s3", "[hide][s3]")
{
    CatchupSimulation catchupSimulation{
        std::make_shared<S3HistoryConfigurator>()};

    catchupSimulation.generateAndPublishInitialHistory(3);
    auto app2 = catchupSimulation.catchupNewApplication(
        catchupSimulation.getApp()
            .getLedgerManager()
            .getCurrentLedgerHeader()
            .ledgerSeq,
        std::numeric_limits<uint32_t>::max(), false,
        Config::TESTDB_IN_MEMORY_SQLITE, "s3");
}

TEST_CASE("persist publish queue", "[history]")
{
    Config cfg(getTestConfig(0, Config::TESTDB_ON_DISK_SQLITE));
    cfg.MAX_CONCURRENT_SUBPROCESSES = 0;
    cfg.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = true;
    TmpDirHistoryConfigurator tcfg;
    cfg = tcfg.configure(cfg, true);

    {
        VirtualClock clock;
        Application::pointer app0 = createTestApplication(clock, cfg);
        app0->start();
        auto& hm0 = app0->getHistoryManager();
        while (hm0.getPublishQueueCount() < 5)
        {
            clock.crank(true);
        }
        // We should have published nothing and have the first
        // checkpoint still queued.
        CHECK(hm0.getPublishSuccessCount() == 0);
        CHECK(hm0.getMinLedgerQueuedToPublish() == 7);
        while (clock.cancelAllEvents() ||
               app0->getProcessManager().getNumRunningProcesses() > 0)
        {
            clock.crank(true);
        }
        LOG(INFO) << app0->isStopping();

        // Trim history after publishing.
        ExternalQueue ps(*app0);
        ps.deleteOldEntries(50000);
    }

    cfg.MAX_CONCURRENT_SUBPROCESSES = 32;

    {
        VirtualClock clock;
        Application::pointer app1 = Application::create(clock, cfg, false);
        HistoryManager::initializeHistoryArchive(*app1, "test");
        for (size_t i = 0; i < 100; ++i)
            clock.crank(false);
        app1->start();
        auto& hm1 = app1->getHistoryManager();
        while (hm1.getPublishSuccessCount() < 5)
        {
            clock.crank(true);

            // Trim history after publishing whenever possible.
            ExternalQueue ps(*app1);
            ps.deleteOldEntries(50000);
        }
        // We should have either an empty publish queue or a
        // ledger sometime after the 5th checkpoint
        auto minLedger = hm1.getMinLedgerQueuedToPublish();
        LOG(INFO) << "minLedger " << minLedger;
        bool okQueue = minLedger == 0 || minLedger >= 35;
        CHECK(okQueue);
        clock.cancelAllEvents();
        while (clock.cancelAllEvents() ||
               app1->getProcessManager().getNumRunningProcesses() > 0)
        {
            clock.crank(true);
        }
        LOG(INFO) << app1->isStopping();
    }
}

// The idea with this test is that we join a network and somehow get a gap
// in the SCP voting sequence while we're trying to catchup. This will let
// system catchup just before the gap.
TEST_CASE("too far behind / catchup restart", "[history][catchupstall]")
{
    CatchupSimulation catchupSimulation{};

    catchupSimulation.generateAndPublishInitialHistory(1);

    // Catch up successfully the first time
    auto app2 = catchupSimulation.catchupNewApplication(
        catchupSimulation.getApp().getLedgerManager().getLastClosedLedgerNum() -
            1,
        std::numeric_limits<uint32_t>::max(), false,
        Config::TESTDB_IN_MEMORY_SQLITE, "app2");

    // Now generate a little more history
    catchupSimulation.generateAndPublishHistory(1);

    auto init = app2->getLedgerManager().getLastClosedLedgerNum() + 2;
    REQUIRE(init == 66);

    // Now start a catchup on that catchups as far as it can due to gap
    LOG(INFO) << "Starting catchup (with gap) from " << init;
    REQUIRE(catchupSimulation.catchupApplication(
        init, std::numeric_limits<uint32_t>::max(), false, app2, true,
        init + 10));
    REQUIRE(app2->getLedgerManager().getLastClosedLedgerNum() == 75);

    app2->getWorkManager().clearChildren();

    // Now generate a little more history
    catchupSimulation.generateAndPublishHistory(1);

    // And catchup successfully
    init =
        catchupSimulation.getApp().getLedgerManager().getLastClosedLedgerNum() -
        1;
    REQUIRE(catchupSimulation.catchupApplication(
        init, std::numeric_limits<uint32_t>::max(), false, app2));
    REQUIRE(app2->getLedgerManager().getLastClosedLedgerNum() == 192);
}

/*
 * Test a variety of orderings of CATCHUP_RECENT mode, to shake out boundary
 * cases.
 */
TEST_CASE("Catchup recent", "[history][catchuprecent]")
{
    CatchupSimulation catchupSimulation{};

    auto dbMode = Config::TESTDB_IN_MEMORY_SQLITE;
    std::vector<Application::pointer> apps;

    catchupSimulation.generateAndPublishInitialHistory(3);

    // Network has published 0x3f (63), 0x7f (127) and 0xbf (191)
    // Network is currently sitting on ledger 0xc0 (192)
    uint32_t initLedger =
        catchupSimulation.getApp().getLedgerManager().getLastClosedLedgerNum() -
        1;

    // Check that isolated catchups work at a variety of boundary
    // conditions relative to the size of a checkpoint:
    std::vector<uint32_t> recents = {0,   1,   2,   31,  32,  33,  62,  63,
                                     64,  65,  66,  126, 127, 128, 129, 130,
                                     190, 191, 192, 193, 194, 1000};

    for (auto r : recents)
    {
        auto name = std::string("catchup-recent-") + std::to_string(r);
        apps.push_back(catchupSimulation.catchupNewApplication(
            initLedger, r, false, dbMode, name));
    }

    // Now push network along a little bit and see that they can all still
    // catch up properly.
    catchupSimulation.generateAndPublishHistory(2);
    initLedger =
        catchupSimulation.getApp().getLedgerManager().getLastClosedLedgerNum() -
        1;

    for (auto a : apps)
    {
        catchupSimulation.catchupApplication(initLedger, 80, false, a);
    }

    // Now push network along a _lot_ futher along see that they can all
    // still
    // catch up properly.
    catchupSimulation.generateAndPublishHistory(25);
    initLedger =
        catchupSimulation.getApp().getLedgerManager().getLastClosedLedgerNum() -
        1;

    for (auto a : apps)
    {
        catchupSimulation.catchupApplication(initLedger, 80, false, a);
    }
}

/*
 * Test a variety of LCL/initLedger/count modes.
 */
TEST_CASE("Catchup manual", "[history][catchupmanual]")
{
    CatchupSimulation catchupSimulation{};

    auto dbMode = Config::TESTDB_IN_MEMORY_SQLITE;
    std::vector<Application::pointer> apps;

    catchupSimulation.generateAndPublishInitialHistory(6);
    auto initLedger =
        catchupSimulation.getApp().getLedgerManager().getLastClosedLedgerNum() -
        1;
    REQUIRE(initLedger == 383);

    for (auto const& test : stellar::gCatchupRangeCases)
    {
        auto lastClosedLedger = test.first;
        auto configuration = test.second;
        auto name = fmt::format("lcl = {}, to ledger = {}, count = {}",
                                lastClosedLedger, configuration.toLedger(),
                                configuration.count());
        // manual catchup-recent
        auto a = catchupSimulation.catchupNewApplication(
            configuration.toLedger(), configuration.count(), true, dbMode,
            name);
        // manual catchup-complete
        catchupSimulation.catchupApplication(
            initLedger, std::numeric_limits<uint32_t>::max(), true, a);
        apps.push_back(a);
    }

    // Now push network along a little bit and see that they can all still
    // catch up properly.
    catchupSimulation.generateAndPublishHistory(2);
    initLedger =
        catchupSimulation.getApp().getLedgerManager().getLastClosedLedgerNum() -
        1;

    for (auto a : apps)
    {
        catchupSimulation.catchupApplication(initLedger, 80, false, a);
    }
}

// Check that initializing a history store that already exists, fails.
TEST_CASE("initialize existing history store fails", "[history]")
{
    Config cfg(getTestConfig(0, Config::TESTDB_ON_DISK_SQLITE));
    TmpDirHistoryConfigurator tcfg;
    cfg = tcfg.configure(cfg, true);

    {
        VirtualClock clock;
        Application::pointer app = createTestApplication(clock, cfg);
        REQUIRE(HistoryManager::initializeHistoryArchive(*app, "test"));
    }

    {
        VirtualClock clock;
        Application::pointer app = createTestApplication(clock, cfg);
        REQUIRE(!HistoryManager::initializeHistoryArchive(*app, "test"));
    }
}
