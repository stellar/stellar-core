// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// This file contains tests for the BucketManager, and higher-level operations
// concerning the lifecycle of buckets, their ownership and (re)creation, and
// integration into ledgers.

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"
#include "bucket/Bucket.h"
#include "bucket/BucketManager.h"
#include "bucket/BucketManagerImpl.h"
#include "bucket/BucketTests.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Timer.h"

using namespace stellar;
using namespace BucketTests;

namespace BucketManagerTests
{
static void
clearFutures(Application::pointer app, BucketList& bl)
{

    // First go through the BL and mop up all the FutureBuckets.
    for (uint32_t i = 0; i < BucketList::kNumLevels; ++i)
    {
        bl.getLevel(i).getNext().clear();
    }

    // Then go through all the _worker threads_ and mop up any work they
    // might still be doing (that might be "dropping a shared_ptr<Bucket>").

    size_t n = std::thread::hardware_concurrency();
    std::mutex mutex;
    std::condition_variable cv, cv2;
    size_t waiting = 0, finished = 0;
    for (size_t i = 0; i < n; ++i)
    {
        app->postOnBackgroundThread(
            [&] {
                std::unique_lock<std::mutex> lock(mutex);
                if (++waiting == n)
                {
                    cv.notify_all();
                }
                else
                {
                    cv.wait(lock, [&] { return waiting == n; });
                }
                ++finished;
                cv2.notify_one();
            },
            "BucketTests: clearFutures");
    }
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv2.wait(lock, [&] { return finished == n; });
    }
}

static Hash
closeLedger(Application& app)
{
    auto& lm = app.getLedgerManager();
    auto lcl = lm.getLastClosedLedgerHeader();
    uint32_t ledgerNum = lcl.header.ledgerSeq + 1;
    CLOG(INFO, "Bucket")
        << "Artificially closing ledger " << ledgerNum
        << " with lcl=" << hexAbbrev(lcl.hash) << ", buckets="
        << hexAbbrev(app.getBucketManager().getBucketList().getHash());
    auto txSet = std::make_shared<TxSetFrame>(lcl.hash);
    StellarValue sv(txSet->getContentsHash(), lcl.header.scpValue.closeTime,
                    emptyUpgradeSteps, 0);
    LedgerCloseData lcd(ledgerNum, txSet, sv);
    lm.valueExternalized(lcd);
    return lm.getLastClosedLedgerHeader().hash;
}
}

using namespace BucketManagerTests;

TEST_CASE("skip list", "[bucket][bucketmanager]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application::pointer app = createTestApplication(clock, cfg);

    class BucketManagerTest : public BucketManagerImpl
    {
      public:
        BucketManagerTest(Application& app) : BucketManagerImpl(app)
        {
        }
        void
        test()
        {
            Hash h0;
            Hash h1 = HashUtils::random();
            Hash h2 = HashUtils::random();
            Hash h3 = HashUtils::random();
            Hash h4 = HashUtils::random();
            Hash h5 = HashUtils::random();
            Hash h6 = HashUtils::random();
            Hash h7 = HashUtils::random();

            // up first entry
            LedgerHeader header;
            header.ledgerSeq = 5;
            header.bucketListHash = h1;
            calculateSkipValues(header);
            REQUIRE(header.skipList[0] == h0);
            REQUIRE(header.skipList[1] == h0);
            REQUIRE(header.skipList[2] == h0);
            REQUIRE(header.skipList[3] == h0);

            header.ledgerSeq = SKIP_1;
            header.bucketListHash = h2;
            calculateSkipValues(header);
            REQUIRE(header.skipList[0] == h2);
            REQUIRE(header.skipList[1] == h0);
            REQUIRE(header.skipList[2] == h0);
            REQUIRE(header.skipList[3] == h0);

            header.ledgerSeq = SKIP_1 * 2;
            header.bucketListHash = h3;
            calculateSkipValues(header);
            REQUIRE(header.skipList[0] == h3);
            REQUIRE(header.skipList[1] == h0);
            REQUIRE(header.skipList[2] == h0);
            REQUIRE(header.skipList[3] == h0);

            header.ledgerSeq = SKIP_1 * 2 + 1;
            header.bucketListHash = h2;
            calculateSkipValues(header);
            REQUIRE(header.skipList[0] == h3);
            REQUIRE(header.skipList[1] == h0);
            REQUIRE(header.skipList[2] == h0);
            REQUIRE(header.skipList[3] == h0);

            header.ledgerSeq = SKIP_2;
            header.bucketListHash = h4;
            calculateSkipValues(header);
            REQUIRE(header.skipList[0] == h4);
            REQUIRE(header.skipList[1] == h0);
            REQUIRE(header.skipList[2] == h0);
            REQUIRE(header.skipList[3] == h0);

            header.ledgerSeq = SKIP_2 + SKIP_1;
            header.bucketListHash = h5;
            calculateSkipValues(header);
            REQUIRE(header.skipList[0] == h5);
            REQUIRE(header.skipList[1] == h4);
            REQUIRE(header.skipList[2] == h0);
            REQUIRE(header.skipList[3] == h0);

            header.ledgerSeq = SKIP_3 + SKIP_2;
            header.bucketListHash = h6;
            calculateSkipValues(header);
            REQUIRE(header.skipList[0] == h6);
            REQUIRE(header.skipList[1] == h4);
            REQUIRE(header.skipList[2] == h0);
            REQUIRE(header.skipList[3] == h0);

            header.ledgerSeq = SKIP_3 + SKIP_2 + SKIP_1;
            header.bucketListHash = h7;
            calculateSkipValues(header);
            REQUIRE(header.skipList[0] == h7);
            REQUIRE(header.skipList[1] == h6);
            REQUIRE(header.skipList[2] == h4);
            REQUIRE(header.skipList[3] == h0);
        }
    };

    BucketManagerTest btest(*app);
    btest.test();
}

TEST_CASE("bucketmanager ownership", "[bucket][bucketmanager]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    for_versions_with_differing_bucket_logic(cfg, [&](Config const& cfg) {
        Application::pointer app = createTestApplication(clock, cfg);

        std::vector<LedgerEntry> live(
            LedgerTestUtils::generateValidLedgerEntries(10));
        std::vector<LedgerKey> dead{};

        std::shared_ptr<Bucket> b1;

        {
            std::shared_ptr<Bucket> b2 =
                Bucket::fresh(app->getBucketManager(), getAppLedgerVersion(app),
                              {}, live, dead, /*countMergeEvents=*/true);
            b1 = b2;

            // Bucket is referenced by b1, b2 and the BucketManager.
            CHECK(b1.use_count() == 3);

            std::shared_ptr<Bucket> b3 =
                Bucket::fresh(app->getBucketManager(), getAppLedgerVersion(app),
                              {}, live, dead, /*countMergeEvents=*/true);
            std::shared_ptr<Bucket> b4 =
                Bucket::fresh(app->getBucketManager(), getAppLedgerVersion(app),
                              {}, live, dead, /*countMergeEvents=*/true);
            // Bucket is referenced by b1, b2, b3, b4 and the BucketManager.
            CHECK(b1.use_count() == 5);
        }

        // Bucket is now only referenced by b1 and the BucketManager.
        CHECK(b1.use_count() == 2);

        // Drop bucket ourselves then purge bucketManager.
        std::string filename = b1->getFilename();
        CHECK(fs::exists(filename));
        b1.reset();
        app->getBucketManager().forgetUnreferencedBuckets();
        CHECK(!fs::exists(filename));

        // Try adding a bucket to the BucketManager's bucketlist
        auto& bl = app->getBucketManager().getBucketList();
        bl.addBatch(*app, 1, getAppLedgerVersion(app), {}, live, dead);
        clearFutures(app, bl);
        b1 = bl.getLevel(0).getCurr();

        // Bucket should be referenced by bucketlist itself, BucketManager cache
        // and b1.
        CHECK(b1.use_count() == 3);

        // This shouldn't change if we forget unreferenced buckets since it's
        // referenced by bucketlist.
        app->getBucketManager().forgetUnreferencedBuckets();
        CHECK(b1.use_count() == 3);

        // But if we mutate the curr bucket of the bucketlist, it should.
        live[0] = LedgerTestUtils::generateValidLedgerEntry(10);
        bl.addBatch(*app, 1, getAppLedgerVersion(app), {}, live, dead);
        clearFutures(app, bl);
        CHECK(b1.use_count() == 2);

        // Drop it again.
        filename = b1->getFilename();
        CHECK(fs::exists(filename));
        b1.reset();
        app->getBucketManager().forgetUnreferencedBuckets();
        CHECK(!fs::exists(filename));
    });
}

TEST_CASE("bucket persistence over app restart", "[bucket][bucketmanager][bucketpersist]")
{
    std::vector<stellar::LedgerKey> emptySet;
    std::vector<stellar::LedgerEntry> emptySetEntry;

    VirtualClock clock;
    Config cfg0(getTestConfig(0, Config::TESTDB_ON_DISK_SQLITE));
    for_versions_with_differing_bucket_logic(cfg0, [&](Config const& cfg0) {

        Config cfg1(getTestConfig(1, Config::TESTDB_ON_DISK_SQLITE));
        cfg1.LEDGER_PROTOCOL_VERSION = cfg0.LEDGER_PROTOCOL_VERSION;
        cfg1.ARTIFICIALLY_PESSIMIZE_MERGES_FOR_TESTING = true;

        std::vector<std::vector<LedgerEntry>> batches;
        for (uint32_t i = 0; i < 110; ++i)
        {
            batches.push_back(LedgerTestUtils::generateValidLedgerEntries(1));
        }

        // Inject a common object at the first batch we're going to run
        // (batch #2) and at the pause-merge threshold; this makes the
        // pause-merge (#64, where we stop and serialize) sensitive to
        // shadowing, and requires shadows be reconstituted when the merge
        // is restarted.
        auto alice = LedgerTestUtils::generateValidLedgerEntry(1);
        uint32_t pause = 65;
        batches[2].push_back(alice);
        batches[pause - 2].push_back(alice);

        Hash Lh1, Lh2;
        Hash Blh1, Blh2;

        // First, run an application through two ledger closes, picking up
        // the bucket and ledger closes at each.
        {
            Application::pointer app = createTestApplication(clock, cfg0);
            app->start();
            BucketList& bl = app->getBucketManager().getBucketList();

            uint32_t i = 2;
            while (i < pause)
            {
                CLOG(INFO, "Bucket") << "Adding setup phase 1 batch " << i;
                bl.addBatch(*app, i, getAppLedgerVersion(app), {}, batches[i],
                            emptySet);
                i++;
            }

            Lh1 = closeLedger(*app);
            Blh1 = bl.getHash();
            REQUIRE(!isZero(Lh1));
            REQUIRE(!isZero(Blh1));

            while (i < 100)
            {
                CLOG(INFO, "Bucket") << "Adding setup phase 2 batch " << i;
                bl.addBatch(*app, i, getAppLedgerVersion(app), {}, batches[i],
                            emptySet);
                i++;
            }

            Lh2 = closeLedger(*app);
            Blh2 = bl.getHash();
            REQUIRE(!isZero(Blh2));
            REQUIRE(!isZero(Lh2));
        }

        // Next run a new app with a disjoint config one ledger close, and
        // stop it. It should have acquired the same state and ledger.
        {
            Application::pointer app = createTestApplication(clock, cfg1);
            app->start();
            BucketList& bl = app->getBucketManager().getBucketList();

            uint32_t i = 2;
            while (i < pause)
            {
                CLOG(INFO, "Bucket") << "Adding prefix-batch " << i;
                bl.addBatch(*app, i, getAppLedgerVersion(app), {}, batches[i],
                            emptySet);
                i++;
            }

            REQUIRE(hexAbbrev(Lh1) == hexAbbrev(closeLedger(*app)));
            REQUIRE(hexAbbrev(Blh1) == hexAbbrev(bl.getHash()));

            // Confirm that there are merges-in-progress in this checkpoint.
            HistoryArchiveState has(i, bl);
            REQUIRE(!has.futuresAllResolved());
        }

        // Finally *restart* an app on the same config, and see if it can
        // pick up the bucket list correctly.
        cfg1.FORCE_SCP = false;
        {
            Application::pointer app = Application::create(clock, cfg1, false);
            app->start();
            BucketList& bl = app->getBucketManager().getBucketList();

            // Confirm that we re-acquired the close-ledger state.
            REQUIRE(
                hexAbbrev(Lh1) ==
                hexAbbrev(
                    app->getLedgerManager().getLastClosedLedgerHeader().hash));
            REQUIRE(hexAbbrev(Blh1) == hexAbbrev(bl.getHash()));

            uint32_t i = pause;

            // Confirm that merges-in-progress were restarted.
            HistoryArchiveState has(i, bl);
            REQUIRE(!has.futuresAllResolved());

            while (i < 100)
            {
                CLOG(INFO, "Bucket") << "Adding suffix-batch " << i;
                bl.addBatch(*app, i, getAppLedgerVersion(app), {}, batches[i],
                            emptySet);
                i++;
            }

            // Confirm that merges-in-progress finished with expected
            // results.
            REQUIRE(hexAbbrev(Lh2) == hexAbbrev(closeLedger(*app)));
            REQUIRE(hexAbbrev(Blh2) == hexAbbrev(bl.getHash()));
        }
    });
}
