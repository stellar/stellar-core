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
#include <cstdio>

using namespace stellar;

namespace stellar {
using xdr::operator==;
};

void
addLocalDirHistoryArchive(TempDir const& dir, Config &cfg)
{
    std::string d = dir.getName();
    cfg.HISTORY["test"] = std::make_shared<HistoryArchive>(
        "test",
        "cp " + d + "/{0} {1}",
        "cp {0} " + d + "/{1}");
}

TEST_CASE("Archive and reload history", "[history]")
{
    VirtualClock clock;
    Config cfg = getTestConfig();
    TempDir dir("archive");
    addLocalDirHistoryArchive(dir, cfg);

    Application app(clock, cfg);
    autocheck::generator<History> gen;
    auto h1 = std::make_shared<History>(gen(10));
    LOG(DEBUG) << "archiving " << h1->entries.size() << " history entries";
    bool done = false;
    app.getHistoryMaster().archiveHistory(
        h1,
        [&app, &done, h1](std::string const& filename)
        {
            LOG(DEBUG) << "archived " << filename << ", deleting";
            std::remove(filename.c_str());
            LOG(DEBUG) << "reloading history for ["
                       << h1->fromLedger << ", "
                       << h1->toLedger << "]";
            app.getHistoryMaster().acquireHistory(
                h1->fromLedger,
                h1->toLedger,
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

TEST_CASE("Archive and reload bucket", "[history]")
{
    VirtualClock clock;
    Config cfg = getTestConfig();
    TempDir dir("archive");
    addLocalDirHistoryArchive(dir, cfg);

    Application app(clock, cfg);
    autocheck::generator<CLFBucket> gen;
    auto b1 = std::make_shared<CLFBucket>(gen(10));
    LOG(DEBUG) << "archiving " << b1->entries.size() << " bucket entries";
    bool done = false;
    app.getHistoryMaster().archiveBucket(
        b1,
        [&app, &done, b1](std::string const& filename)
        {
            LOG(DEBUG) << "archived " << filename << ", deleting";
            std::remove(filename.c_str());
            LOG(DEBUG) << "reloading bucket for seq="
                       << b1->header.ledgerSeq << ", count="
                       << b1->header.ledgerCount;
            app.getHistoryMaster().acquireBucket(
                b1->header.ledgerSeq,
                b1->header.ledgerCount,
                [b1, &done](std::shared_ptr<CLFBucket> b2)
                {
                    LOG(DEBUG) << "reloaded bucket";
                    CHECK(*b1 == *b2);
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
