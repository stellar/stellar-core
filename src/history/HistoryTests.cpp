// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC
#include "util/asio.h"
#include "main/Application.h"
#include "history/HistoryMaster.h"
#include "history/HistoryArchive.h"
#include "main/test.h"
#include "main/Config.h"
#include "crypto/Hex.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "util/Timer.h"
#include "util/TmpDir.h"
#include <cstdio>
#include <xdrpp/autocheck.h>
#include <fstream>
#include <cstdio>

using namespace stellar;

namespace stellar {
using xdr::operator==;
};

TmpDir
addLocalDirHistoryArchive(Application& app, Config &cfg)
{
    TmpDir dir = app.getTmpDirMaster().tmpDir("archive");
    std::string d = dir.getName();
    cfg.HISTORY["test"] = std::make_shared<HistoryArchive>(
        "test",
        "cp " + d + "/{0} {1}",
        "cp {0} " + d + "/{1}");
    return dir;
}

#if 0
TEST_CASE("Archive and reload history", "[history]")
{
    VirtualClock clock;
    Config cfg = getTestConfig();
    Application app(clock, cfg);
    TmpDir dir = addLocalDirHistoryArchive(app, cfg)

    autocheck::generator<History> gen;
    auto h1 = std::make_shared<History>(gen(10));
    LOG(DEBUG) << "archiving " << h1->entries.size() << " history entries";
    bool done = false;
    app.getHistoryMaster().archiveHistory(
        h1,
        [&app, &done, h1](asio::error_code const& ec)
        {
            CHECK(!ec);
            if (ec)
            {
                done = true;
                return;
            }
            LOG(DEBUG) << "archived history, reloading ["
                       << h1->fromLedger << ", "
                       << h1->toLedger << "]";
            app.getHistoryMaster().acquireHistory(
                h1->fromLedger,
                h1->toLedger,
                [h1, &done](asio::error_code const& ec,
                            std::shared_ptr<History> h2)
                {
                    CHECK(!ec);
                    CHECK(h2);
                    if (!ec && h2)
                    {
                        LOG(DEBUG) << "reloaded history";
                        CHECK(*h1 == *h2);
                    }
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
    Application app(clock, cfg);
    TmpDir dir = addLocalDirHistoryArchive(app, cfg)

    autocheck::generator<CLFBucket> gen;
    auto b1 = std::make_shared<CLFBucket>(gen(10));
    LOG(DEBUG) << "archiving " << b1->entries.size() << " bucket entries";
    bool done = false;
    app.getHistoryMaster().archiveBucket(
        b1,
        [&app, &done, b1](asio::error_code const& ec)
        {
            CHECK(!ec);
            if (ec)
            {
                done = true;
                return;
            }
            LOG(DEBUG) << "archived bucket, reloading seq="
                       << b1->header.ledgerSeq << ", count="
                       << b1->header.ledgerCount;
            app.getHistoryMaster().acquireBucket(
                b1->header.ledgerSeq,
                b1->header.ledgerCount,
                [b1, &done](asio::error_code const& ec,
                            std::shared_ptr<CLFBucket> b2)
                {
                    CHECK(!ec);
                    CHECK(b2);
                    if (!ec && b2)
                    {
                        LOG(DEBUG) << "reloaded bucket";
                        CHECK(*b1 == *b2);
                    }
                    done = true;
                });
        });

    while (!done && !app.getMainIOService().stopped())
    {
        app.crank();
    }
}
#endif


TEST_CASE("HistoryMaster::compress", "[history]")
{
    std::string s = "hello there";
    VirtualClock clock;
    Config cfg = getTestConfig();
    Application app(clock, cfg);
    HistoryMaster &hm = app.getHistoryMaster();
    std::string fname = hm.localFilename("compressme");
    {
        std::ofstream out(fname, std::ofstream::binary);
        out.write(s.data(), s.size());
    }
    bool done = false;
    hm.compress(
        fname,
        [&done, &fname, &hm](asio::error_code const& ec)
        {
            std::string compressed = fname + ".gz";
            CHECK(!TmpDir::exists(fname));
            CHECK(TmpDir::exists(compressed));
            hm.decompress(
                compressed,
                [&done, &fname, &compressed](asio::error_code const& ec)
                {
                    CHECK(TmpDir::exists(fname));
                    CHECK(!TmpDir::exists(compressed));
                    done = true;
                });
        });
    while (!done && !app.getMainIOService().stopped())
    {
        app.crank();
    }
}


TEST_CASE("HistoryMaster::verifyHash", "[history]")
{
    std::string s = "hello there";
    VirtualClock clock;
    Config cfg = getTestConfig();
    Application app(clock, cfg);
    HistoryMaster &hm = app.getHistoryMaster();
    std::string fname = hm.localFilename("hashme");
    {
        std::ofstream out(fname, std::ofstream::binary);
        out.write(s.data(), s.size());
    }
    bool done = false;
    uint256 hash = hexToBin256("12998c017066eb0d2a70b94e6ed3192985855ce390f321bbdb832022888bd251");
    hm.verifyHash(
        fname, hash,
        [&done](asio::error_code const& ec)
        {
            CHECK(!ec);
            done = true;
        });

    while (!done && !app.getMainIOService().stopped())
    {
        app.crank();
    }
}


TEST_CASE("HistoryArchiveState::get_put", "[history]")
{
    VirtualClock clock;
    Config cfg = getTestConfig();
    Application app(clock, cfg);
    TmpDir dir = addLocalDirHistoryArchive(app, cfg);
    HistoryArchiveState has;
    has.currentLedger = 0x1234;
    bool done = false;
    auto archive = cfg.HISTORY["test"];
    archive->putState(
        app, has,
        [&done, &app, archive](asio::error_code const& ec)
        {
            CHECK(!ec);
            archive->getState(
                app,
                [&done](asio::error_code const& ec,
                        HistoryArchiveState const& has2)
                {
                    CHECK(has2.currentLedger == 0x1234);
                    CHECK(!ec);
                    done = true;
                });
        });
    while (!done && !app.getMainIOService().stopped())
    {
        app.crank();
    }
}
