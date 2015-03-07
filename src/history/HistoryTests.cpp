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
#include "transactions/TxTests.h"
#include "ledger/LedgerMaster.h"
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
    while (!done && !app.getClock().getIOService().stopped())
    {
        app.getClock().crank(false);
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

    auto& lm = app.getLedgerMaster();

    // At this point LCL should be 1, current ledger should be 2
    assert(lm.getLastClosedLedgerHeader().ledgerSeq == 1);
    assert(lm.getCurrentLedgerHeader().ledgerSeq == 2);

    SecretKey root = txtest::getRoot();
    SecretKey bob = txtest::getAccount("bob");

    TxSetFramePtr txSet2 = std::make_shared<TxSetFrame>();
    TxSetFramePtr txSet3 = std::make_shared<TxSetFrame>();
    TxSetFramePtr txSet4 = std::make_shared<TxSetFrame>();

    txSet2->add(txtest::createPaymentTx(root, bob, 1, 1000));
    txSet3->add(txtest::createPaymentTx(root, bob, 2, 1000));
    txSet4->add(txtest::createPaymentTx(root, bob, 3, 1000));

    lm.closeLedger(LedgerCloseData(2, txSet2, 2, 10));
    lm.closeLedger(LedgerCloseData(3, txSet3, 3, 10));
    lm.closeLedger(LedgerCloseData(4, txSet4, 4, 10));

    // At this point LCL should be 4, current ledger should be 5
    assert(lm.getLastClosedLedgerHeader().ledgerSeq == 4);
    assert(lm.getCurrentLedgerHeader().ledgerSeq == 5);

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

    Config cfg2(getTestConfig(1));
    Application::pointer app2 = Application::create(clock, addLocalDirHistoryArchive(dir, cfg2));
    app2->start();

    bool done = false;
    app2->getHistoryMaster().catchupHistory(
        [&done](asio::error_code const& ec)
        {
            CHECK(!ec);
            done = true;
        });
    while (!done && !app2->getClock().getIOService().stopped())
    {
        app2->getClock().crank(false);
    }
    CHECK(app2->getCLFMaster().getBucketByHash(hash));
    auto hash2 = app2->getCLFMaster().getBucketList().getLevel(0).getCurr()->getHash();
    CHECK(hash == hash2);
}
