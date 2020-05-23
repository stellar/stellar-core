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
#include "simulation/Simulation.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Logging.h"
#include "xdr/Stellar-ledger.h"
#include <fmt/format.h>
#include <fstream>

using namespace stellar;

TEST_CASE("LedgerCloseMetaStream file descriptor - LIVE_NODE",
          "[ledgerclosemetastreamlive]")
{
    // Live reqires a multinode simulation, as we're not allowed to run a
    // validator and record metadata streams at the same time (to avoid the
    // unbounded-latency stream-write step): N nodes participating in consensus,
    // and one watching and streamnig metadata.

    Hash hash;
    TmpDirManager tdm(std::string("streamtmp-") + binToHex(randomBytes(8)));
    TmpDir td = tdm.tmpDir("streams");
    std::string path = td.getName() + "/stream.xdr";

    {
        // Step 1: Set up a 4 node simulation with 3 validators and 1 watcher.
        auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
        auto simulation =
            std::make_shared<Simulation>(Simulation::OVER_LOOPBACK, networkID);

        SIMULATION_CREATE_NODE(Node1); // Validator
        SIMULATION_CREATE_NODE(Node2); // Validator
        SIMULATION_CREATE_NODE(Node3); // Validator

        SIMULATION_CREATE_NODE(Node4); // Watcher

        SCPQuorumSet qSet;
        qSet.threshold = 3;
        qSet.validators.push_back(vNode1NodeID);
        qSet.validators.push_back(vNode2NodeID);
        qSet.validators.push_back(vNode3NodeID);

        Config const& cfg1 = getTestConfig(1);
        Config const& cfg2 = getTestConfig(2);
        Config const& cfg3 = getTestConfig(3);
        Config cfg4 = getTestConfig(4);

        // Step 2: open a writable file and pass it to config 4 (watcher).
        cfg4.NODE_IS_VALIDATOR = false;
        cfg4.FORCE_SCP = false;
#ifdef _WIN32
        cfg4.METADATA_OUTPUT_STREAM = path;
#else
        int fd = ::open(path.c_str(), O_CREAT | O_WRONLY, 0644);
        REQUIRE(fd != -1);
        cfg4.METADATA_OUTPUT_STREAM = fmt::format("fd:{}", fd);
#endif

        // Step 3: Run simulation a few steps to stream metadata.
        auto app1 = simulation->addNode(vNode1SecretKey, qSet, &cfg1);
        auto app2 = simulation->addNode(vNode2SecretKey, qSet, &cfg2);
        auto app3 = simulation->addNode(vNode3SecretKey, qSet, &cfg3);
        auto app4 = simulation->addNode(vNode4SecretKey, qSet, &cfg4);

        simulation->addPendingConnection(vNode1NodeID, vNode2NodeID);
        simulation->addPendingConnection(vNode1NodeID, vNode3NodeID);
        simulation->addPendingConnection(vNode1NodeID, vNode4NodeID);

        simulation->startAllNodes();
        simulation->crankUntil(
            [&]() {
                return app4->getLedgerManager().getLastClosedLedgerNum() == 10;
            },
            std::chrono::seconds{200}, false);

        REQUIRE(app4->getLedgerManager().getLastClosedLedgerNum() == 10);
        hash = app4->getLedgerManager().getLastClosedLedgerHeader().hash;
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
        Config genCfg = getTestConfig(0, Config::TESTDB_DEFAULT);
        genCfg.MANUAL_CLOSE = false;
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
        // Replay-in-memory configs
        cfg.DISABLE_XDR_FSYNC = true;
        cfg.DATABASE = SecretValue{"sqlite3://:memory:"};
        cfg.MODE_STORES_HISTORY = false;
        cfg.MODE_USES_IN_MEMORY_LEDGER = true;
        cfg.MODE_ENABLES_BUCKETLIST = true;
        VirtualClock clock;
        auto app = createTestApplication(clock, cfg, /*newdb=*/false);

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
