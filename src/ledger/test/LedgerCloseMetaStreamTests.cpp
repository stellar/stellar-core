// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/Hex.h"
#include "crypto/Random.h"
#include "history/HistoryArchiveManager.h"
#include "history/test/HistoryTestsUtils.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/ApplicationUtils.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Logging.h"
#include "util/format.h"
#include "xdr/Stellar-ledger.h"
#include <fstream>

using namespace stellar;

TEST_CASE("LedgerCloseMetaStream file descriptor - LIVE_NODE",
          "[ledgerclosemetastreamlive]")
{
    // Step 1: open a writable file and pass it to config.
    TmpDirManager tdm(std::string("streamtmp-") + binToHex(randomBytes(8)));
    TmpDir td = tdm.tmpDir("streams");
    std::string path = td.getName() + "/stream.xdr";
    auto cfg = getTestConfig();
#ifdef _WIN32
    cfg.METADATA_OUTPUT_STREAM = path;
#else
    int fd = ::open(path.c_str(), O_CREAT | O_WRONLY, 0644);
    REQUIRE(fd != -1);
    cfg.METADATA_OUTPUT_STREAM = fmt::format("fd:{}", fd);
#endif

    // Step 2: pass it to an application and close some ledgers,
    // streaming ledgerCloseMeta to the file descriptor.
    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);
    app->start();
    while (app->getLedgerManager().getLastClosedLedgerNum() < 10)
    {
        clock.crank(true);
    }
    Hash hash = app->getLedgerManager().getLastClosedLedgerHeader().hash;
    app.reset();

    // Step 3: reopen the file as an XDR stream and read back the LCMs
    // and check they have the expected content.
    XDRInputFileStream stream;
    stream.open(path);
    LedgerCloseMeta lcm;
    size_t nLcm = 1;
    while (stream && stream.readOne(lcm))
    {
        ++nLcm;
    }
    REQUIRE(nLcm == 10);
    REQUIRE(lcm.v0().ledgerHeader.hash == hash);
}

TEST_CASE("LedgerCloseMetaStream file descriptor - REPLAY_IN_MEMORY",
          "[ledgerclosemetastreamreplay]")
{
    // Step 1: generate some history for replay.
    using namespace stellar::historytestutils;
    TmpDirHistoryConfigurator tCfg;
    {
        Config genCfg = getTestConfig(0);
        VirtualClock genClock;
        genCfg = tCfg.configure(genCfg, true);
        auto genApp = createTestApplication(genClock, genCfg);
        auto& genHam = genApp->getHistoryArchiveManager();
        genHam.initializeHistoryArchive(tCfg.getArchiveDirName());
        for (size_t i = 0; i < 100; ++i)
        {
            genClock.crank(false);
        }
        genApp->start();
        auto& genHm = genApp->getHistoryManager();
        while (genHm.getPublishSuccessCount() < 5)
        {
            genClock.crank(true);
        }
        while (genClock.cancelAllEvents() ||
               genApp->getProcessManager().getNumRunningProcesses() > 0)
        {
            genClock.crank(false);
        }
    }

    // Step 2: open a writable file descriptor.
    TmpDirManager tdm(std::string("streamtmp-") + binToHex(randomBytes(8)));
    TmpDir td = tdm.tmpDir("streams");
    std::string path = td.getName() + "/stream.xdr";
    auto cfg1 = getTestConfig(1);
#ifdef _WIN32
    cfg1.METADATA_OUTPUT_STREAM = path;
#else
    int fd = ::open(path.c_str(), O_CREAT | O_WRONLY, 0644);
    REQUIRE(fd != -1);
    cfg1.METADATA_OUTPUT_STREAM = fmt::format("fd:{}", fd);
#endif

    // Step 3: pass it to an application and have it catch up to the generated
    // history, streaming ledgerCloseMeta to the file descriptor.
    Hash hash;
    {
        auto cfg = tCfg.configure(cfg1, false);
        cfg.NODE_IS_VALIDATOR = false;
        cfg.FORCE_SCP = false;
        cfg.RUN_STANDALONE = true;
        VirtualClock clock;
        auto app =
            createTestApplication(clock, cfg, /*newdb=*/false,
                                  Application::AppMode::REPLAY_IN_MEMORY);

        CatchupConfiguration cc{CatchupConfiguration::CURRENT,
                                std::numeric_limits<uint32_t>::max(),
                                CatchupConfiguration::Mode::OFFLINE_COMPLETE};
        Json::Value catchupInfo;
        auto& ham = app->getHistoryArchiveManager();
        auto& lm = app->getLedgerManager();
        auto archive = ham.selectRandomReadableHistoryArchive();
        int res = catchup(app, cc, catchupInfo, archive);
        REQUIRE(res == 0);
        hash = lm.getLastClosedLedgerHeader().hash;
        while (clock.cancelAllEvents() ||
               app->getProcessManager().getNumRunningProcesses() > 0)
        {
            clock.crank(false);
        }
    }

    // Step 4: reopen the file as an XDR stream and read back the LCMs
    // and check they have the expected content.
    XDRInputFileStream stream;
    stream.open(path);
    LedgerCloseMeta lcm;
    size_t nLcm = 1;
    while (stream && stream.readOne(lcm))
    {
        ++nLcm;
    }
    // 5 checkpoints is ledger 0x13f
    REQUIRE(nLcm == 0x13f);
    REQUIRE(lcm.v0().ledgerHeader.hash == hash);
}
