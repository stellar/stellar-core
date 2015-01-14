// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC
#include <asio.hpp>
#include "main/Application.h"
#include "history/HistoryMaster.h"
#include "history/HistoryArchive.h"
#include "main/test.h"
#include "main/Config.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "util/Timer.h"
#include "util/TempDir.h"
#include <cstdio>
#include <xdrpp/autocheck.h>
#include <fstream>

using namespace stellar;


TEST_CASE("WriteLedgerHistoryToFile", "[history]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application app(clock, cfg);
    autocheck::generator<History> gen;
    HistoryMaster hm(app);
    auto h1 = gen(10);
    auto fname = hm.writeLedgerHistoryToFile(h1);
    History h2;
    hm.readLedgerHistoryFromFile(fname, h2);
    CHECK(h1.fromLedger == h2.fromLedger);
    LOG(DEBUG) << "removing " << fname;
    std::remove(fname.c_str());
}


TEST_CASE("HistoryArchiveParams::save", "[history]")
{
    TempDir dir("historytest");
    HistoryArchiveParams hap;
    auto fname = dir.getName() + "/stellar-history.json";
    hap.save(fname);
    std::ifstream in(fname);
    LOG(DEBUG) << "re-reading " << fname;
    char buf[128];
    while (in.getline(buf, sizeof(buf)))
    {
        LOG(DEBUG) << buf;
    }
}


TEST_CASE("HistoryArchive", "[history]")
{
    Config cfg = getTestConfig();
    cfg.HISTORY["test"] = std::make_shared<HistoryArchive>("test", "cp {0} {1}", "cp {0} {1}");
    VirtualClock clock;
    Application app(clock, cfg);
}
