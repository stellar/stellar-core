// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC
#include "util/asio.h"
#include "main/Application.h"
#include "history/HistoryMaster.h"
#include "history/HistoryArchive.h"
#include "main/test.h"
#include "main/Config.h"
#include "clf/CLFMaster.h"
#include "clf/BucketList.h"
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

Config&
addLocalDirHistoryArchive(TmpDir const& dir, Config &cfg)
{
    std::string d = dir.getName();
    cfg.HISTORY["test"] = std::make_shared<HistoryArchive>(
        "test",
        "cp " + d + "/{0} {1}",
        "cp {0} " + d + "/{1}");
    return cfg;
}


class HistoryTests
{
protected:

    VirtualClock clock;
    TmpDirMaster archtmp;
    TmpDir dir;
    Config cfg;
    Application app;

public:
    HistoryTests()
        : archtmp("archtmp")
        , dir(archtmp.tmpDir("archive"))
        , cfg(getTestConfig())
        , app(clock, addLocalDirHistoryArchive(dir, cfg))
        {}

    void crankTillDone(bool& done);
    void generateAndPublishHistory();
};

void
HistoryTests::crankTillDone(bool& done)
{
    while (!done && !app.getMainIOService().stopped())
    {
        app.crank();
    }
}

TEST_CASE_METHOD(HistoryTests, "HistoryMaster::compress", "[history]")
{
    std::string s = "hello there";
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
    crankTillDone(done);
}


TEST_CASE_METHOD(HistoryTests, "HistoryMaster::verifyHash", "[history]")
{
    std::string s = "hello there";
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
    crankTillDone(done);
}


TEST_CASE_METHOD(HistoryTests, "HistoryArchiveState::get_put", "[history]")
{
    HistoryArchiveState has;
    has.currentLedger = 0x1234;
    bool done = false;

    auto i = app.getConfig().HISTORY.find("test");
    CHECK(i != app.getConfig().HISTORY.end());
    auto archive = i->second;

    auto& app = this->app;
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
    crankTillDone(done);
}


extern LedgerEntry
generateValidLedgerEntry();

void
HistoryTests::generateAndPublishHistory()
{
    app.start();
    HistoryMaster &hm = app.getHistoryMaster();
    auto i = app.getConfig().HISTORY.find("test");
    CHECK(i != app.getConfig().HISTORY.end());
    auto archive = i->second;
    HistoryArchiveState has;
    has.currentLedger = 0;

    bool done = false;
    archive->putState(
        app, has,
        [&done, &hm, this](asio::error_code const& ec)
        {
            CHECK(!ec);

            // Make some changes, then publish them.
            std::vector<LedgerEntry> live { generateValidLedgerEntry(),
                    generateValidLedgerEntry(),
                    generateValidLedgerEntry()
            };
            std::vector<LedgerKey> dead { };
            this->app.getCLFMaster().getBucketList().addBatch(app, 1, live, dead);

            hm.publishHistory(
                [&done](asio::error_code const& ec)
                {
                    CHECK(!ec);
                    done = true;
                });
        });
    crankTillDone(done);
}

TEST_CASE_METHOD(HistoryTests, "History publish", "[history]")
{
    generateAndPublishHistory();
}
