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

using namespace stellar;

namespace stellar {
using xdr::operator==;
};

void
addLocalDirHistoryArchive(TmpDir const& dir, Config &cfg)
{
    std::string d = dir.getName();
    cfg.HISTORY["test"] = std::make_shared<HistoryArchive>(
        "test",
        "cp " + d + "/{0} {1}",
        "cp {0} " + d + "/{1}");
}


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
                [&done, &fname, compressed](asio::error_code const& ec)
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

    TmpDirMaster archtmp("archtmp");
    TmpDir dir = archtmp.tmpDir("archive");
    addLocalDirHistoryArchive(dir, cfg);

    Application app(clock, cfg);

    HistoryArchiveState has;
    has.currentLedger = 0x1234;
    bool done = false;

    auto i = app.getConfig().HISTORY.find("test");
    CHECK(i != app.getConfig().HISTORY.end());
    auto archive = i->second;

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
                    CHECK(!ec);
                    CHECK(has2.currentLedger == 0x1234);
                    done = true;
                });
        });
    while (!done && !app.getMainIOService().stopped())
    {
        app.crank();
    }
}
