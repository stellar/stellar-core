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
    Application::pointer appPtr;
    Application &app;

public:
    HistoryTests()
        : archtmp("archtmp")
        , dir(archtmp.tmpDir("archive"))
        , cfg(getTestConfig())
        , appPtr(Application::create(clock, addLocalDirHistoryArchive(dir, cfg)))
        , app(*appPtr)
        {
            CHECK(HistoryMaster::initializeHistoryArchive(app, "test"));
        }

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

    auto& theApp = this->app; // need a local scope reference
    archive->putState(
        app, has,
        [&done, &theApp, archive](asio::error_code const& ec)
        {
            CHECK(!ec);
            archive->getState(
                theApp,
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
    // Make some changes, then publish them.
    app.start();
    std::vector<LedgerEntry> live { generateValidLedgerEntry(),
            generateValidLedgerEntry(),
            generateValidLedgerEntry()
            };
    std::vector<LedgerKey> dead { };
    app.getCLFMaster().getBucketList().addBatch(app, 1, live, dead);
    bool done = false;
    app.getHistoryMaster().publishHistory(
        [&done](asio::error_code const& ec)
        {
            CHECK(!ec);
            done = true;
        });
    crankTillDone(done);
}

TEST_CASE_METHOD(HistoryTests, "History publish", "[history]")
{
    generateAndPublishHistory();
}

TEST_CASE_METHOD(HistoryTests, "History catchup", "[history]")
{
    generateAndPublishHistory();

    auto hash = app.getCLFMaster().getBucketList().getLevel(0).getCurr()->getHash();

    // Reset BucketList and drop buckets
    app.getCLFMaster().getBucketList() = BucketList();
    app.getCLFMaster().forgetUnreferencedBuckets();

    bool done = false;
    app.getHistoryMaster().catchupHistory(
        [&done](asio::error_code const& ec)
        {
            CHECK(!ec);
            done = true;
        });
    crankTillDone(done);
    CHECK(app.getCLFMaster().getBucketByHash(hash));
    auto hash2 = app.getCLFMaster().getBucketList().getLevel(0).getCurr()->getHash();
    CHECK(hash == hash2);
}


TEST_CASE_METHOD(HistoryTests, "History catchup 2", "[history]")
{
    generateAndPublishHistory();

    auto hash = app.getCLFMaster().getBucketList().getLevel(0).getCurr()->getHash();

    // Reset BucketList, don't drop buckets.
    app.getCLFMaster().getBucketList() = BucketList();

    bool done = false;
    app.getHistoryMaster().catchupHistory(
        [&done](asio::error_code const& ec)
        {
            CHECK(!ec);
            done = true;
        });
    crankTillDone(done);
    CHECK(app.getCLFMaster().getBucketByHash(hash));
    auto hash2 = app.getCLFMaster().getBucketList().getLevel(0).getCurr()->getHash();
    CHECK(hash == hash2);
}
