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
#include "util/Fs.h"
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
        "cp {0} " + d + "/{1}",
        "mkdir -p " + d + "/{0}");
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

TEST_CASE("next checkpoint ledger", "[history]")
{
    CHECK(HistoryMaster::nextCheckpointLedger(0) == 64);
    CHECK(HistoryMaster::nextCheckpointLedger(1) == 64);
    CHECK(HistoryMaster::nextCheckpointLedger(32) == 64);
    CHECK(HistoryMaster::nextCheckpointLedger(62) == 64);
    CHECK(HistoryMaster::nextCheckpointLedger(63) == 64);
    CHECK(HistoryMaster::nextCheckpointLedger(64) == 64);
    CHECK(HistoryMaster::nextCheckpointLedger(65) == 128);
    CHECK(HistoryMaster::nextCheckpointLedger(66) == 128);
    CHECK(HistoryMaster::nextCheckpointLedger(126) == 128);
    CHECK(HistoryMaster::nextCheckpointLedger(127) == 128);
    CHECK(HistoryMaster::nextCheckpointLedger(128) == 128);
    CHECK(HistoryMaster::nextCheckpointLedger(129) == 192);
    CHECK(HistoryMaster::nextCheckpointLedger(130) == 192);
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
            CHECK(!fs::exists(fname));
            CHECK(fs::exists(compressed));
            hm.decompress(
                compressed,
                [&done, &fname, compressed](asio::error_code const& ec)
                {
                    CHECK(fs::exists(fname));
                    CHECK(!fs::exists(compressed));
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
    auto& hm = app.getHistoryMaster();

    // At this point LCL should be 1, current ledger should be 2
    assert(lm.getLastClosedLedgerHeader().header.ledgerSeq == 1);
    assert(lm.getCurrentLedgerHeader().ledgerSeq == 2);

    SecretKey root = txtest::getRoot();
    SecretKey bob = txtest::getAccount("bob");

    uint32_t seq = 1;
    uint32_t ledgerSeq = 1;
    uint64_t closeTime = 1;
    int64_t paymentAmount = lm.getMinBalance(0);
    while (hm.getPublishStartCount() == 0)
    {
        // Keep sending money to bob until we have published some history about it.
        TxSetFramePtr txSet = std::make_shared<TxSetFrame>();
        txSet->add(txtest::createPaymentTx(root, bob, seq++, paymentAmount));
        lm.closeLedger(LedgerCloseData(ledgerSeq++, txSet, closeTime++, 10));
    }

    // At this point LCL should be 63 and we should be starting in on ledger 64...
    assert(lm.getCurrentLedgerHeader().ledgerSeq == HistoryMaster::kCheckpointFrequency);

    // Advance until we've published (or failed to!)
    while (hm.getPublishSuccessCount() == 0)
    {
        CHECK(hm.getPublishFailureCount() == 0);
        app.getClock().crank(false);
    }

    CHECK(hm.getPublishFailureCount() == 0);
    CHECK(hm.getPublishSuccessCount() == 1);
}

TEST_CASE_METHOD(HistoryTests, "History publish", "[history]")
{
    generateAndPublishHistory();
}

TEST_CASE_METHOD(HistoryTests, "History catchup", "[history][historycatchup]")
{
    generateAndPublishHistory();

    auto lclSeq = app.getLedgerMaster().getLastClosedLedgerHeader().header.ledgerSeq;
    auto lclHash = app.getLedgerMaster().getLastClosedLedgerHeader().hash;
    auto bucket0hash = app.getCLFMaster().getBucketList().getLevel(0).getCurr()->getHash();

    Config cfg2(getTestConfig(1));
    Application::pointer app2 = Application::create(clock, addLocalDirHistoryArchive(dir, cfg2));
    app2->start();

    bool done = false;
    app2->getHistoryMaster().catchupHistory(
        0,
        app2->getLedgerMaster().getCurrentLedgerHeader().ledgerSeq,
        HistoryMaster::RESUME_AT_LAST,
        [&done](asio::error_code const& ec, uint32_t nextLedger)
        {
            LOG(INFO) << "Caught up: nextLedger = " << nextLedger;
            CHECK(!ec);
            done = true;
        });
    while (!done &&
           !app2->getClock().getIOService().stopped() &&

           // Amusingly, app2 will also publish, when it catches up.
           (app2->getHistoryMaster().getPublishSuccessCount() +
            app2->getHistoryMaster().getPublishFailureCount() == 0))
    {
        app2->getClock().crank(false);
    }

    auto lclSeq2 = app2->getLedgerMaster().getLastClosedLedgerHeader().header.ledgerSeq;
    auto lclHash2 = app2->getLedgerMaster().getLastClosedLedgerHeader().hash;
    auto bucket0hash2 = app2->getCLFMaster().getBucketList().getLevel(0).getCurr()->getHash();

    CHECK(lclHash == lclHash2);
    CHECK(app2->getCLFMaster().getBucketByHash(bucket0hash));
    CHECK(bucket0hash == bucket0hash2);
}
