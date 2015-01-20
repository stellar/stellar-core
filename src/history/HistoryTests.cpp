// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC
#include "util/asio.h"
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

namespace stellar {
using xdr::operator==;
};

TEST_CASE("Archive and reload history", "[history]")
{
    VirtualClock clock;
    Config cfg = getTestConfig();
    TempDir dir("historytest");
    std::string d = dir.getName();
    cfg.HISTORY["test"] = std::make_shared<HistoryArchive>(
        "test",
        "cp " + d + "/{0} {1}",
        "cp {0} " + d + "/{1}");

    Application app(clock, cfg);
    autocheck::generator<History> gen;
    HistoryMaster hm(app);
    auto h1 = std::make_shared<History>(gen(10));
    LOG(DEBUG) << "archiving " << h1->entries.size() << " history entries";
    bool done = false;
    hm.archiveHistory(
        h1,
        [&hm, &done, h1](std::string const& filename)
        {
            LOG(DEBUG) << "archived as " << filename << ", reloading";
            hm.acquireHistory(
                filename,
                [h1, &done](std::shared_ptr<History> h2)
                {
                    LOG(DEBUG) << "reloaded history";
                    CHECK(*h1 == *h2);
                    done = true;
                });
        });

    while (!done && !app.getMainIOService().stopped())
    {
        app.crank();
    }
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
