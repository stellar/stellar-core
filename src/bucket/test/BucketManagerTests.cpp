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
#include "bucket/BucketInputIterator.h"
#include "bucket/BucketManager.h"
#include "bucket/HotArchiveBucket.h"
#include "bucket/LiveBucket.h"
#include "bucket/test/BucketTestUtils.h"
#include "history/HistoryArchiveManager.h"
#include "history/test/HistoryTestsUtils.h"
#include "ledger/LedgerTxn.h"
#include "ledger/test/LedgerTestUtils.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "main/Maintainer.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/GlobalChecks.h"
#include "util/Math.h"
#include "util/ProtocolVersion.h"
#include "util/Timer.h"
#include "util/UnorderedSet.h"
#include "xdr/Stellar-ledger-entries.h"

#include <cstdio>
#include <optional>
#include <thread>

using namespace stellar;
using namespace BucketTestUtils;

namespace BucketManagerTests
{

static void
clearFutures(Application::pointer app, LiveBucketList& bl)
{

    // First go through the BL and mop up all the FutureBuckets.
    for (uint32_t i = 0; i < LiveBucketList::kNumLevels; ++i)
    {
        bl.getLevel(i).getNext().clear();
    }

    // Then go through all the _worker threads_ and mop up any work they
    // might still be doing (that might be "dropping a shared_ptr<BucketBase>").

    size_t n = static_cast<size_t>(app->getConfig().WORKER_THREADS);

    // Background eviction takes up one worker thread.
    releaseAssert(n != 0);
    --n;

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

    // Tell the BucketManager to forget all about the futures it knows.
    app->getBucketManager().clearMergeFuturesForTesting();
}
}

using namespace BucketManagerTests;

TEST_CASE("skip list", "[bucket][bucketmanager]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application::pointer app = createTestApplication(clock, cfg);

    class BucketManagerTest : public BucketManager
    {
      public:
        BucketManagerTest(Application& app) : BucketManager(app)
        {
        }
        void
        test()
        {
            Hash h0;
            Hash h1 = HashUtils::pseudoRandomForTesting();
            Hash h2 = HashUtils::pseudoRandomForTesting();
            Hash h3 = HashUtils::pseudoRandomForTesting();
            Hash h4 = HashUtils::pseudoRandomForTesting();
            Hash h5 = HashUtils::pseudoRandomForTesting();
            Hash h6 = HashUtils::pseudoRandomForTesting();
            Hash h7 = HashUtils::pseudoRandomForTesting();

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

TEST_CASE_VERSIONS("bucketmanager ownership", "[bucket][bucketmanager]")
{
    VirtualClock clock;
    Config cfg = getTestConfig();

    // Make sure all Buckets serialize indexes to disk for test
    cfg.BUCKETLIST_DB_INDEX_CUTOFF = 0;
    cfg.MANUAL_CLOSE = false;

    for_versions_with_differing_bucket_logic(cfg, [&](Config const& cfg) {
        Application::pointer app = createTestApplication(clock, cfg);

        std::vector<LedgerEntry> live(
            LedgerTestUtils::generateValidUniqueLedgerEntriesWithExclusions(
                {CONFIG_SETTING}, 10));
        std::vector<LedgerKey> dead{};

        std::shared_ptr<LiveBucket> b1;

        {
            std::shared_ptr<LiveBucket> b2 = LiveBucket::fresh(
                app->getBucketManager(), getAppLedgerVersion(app), {}, live,
                dead, /*countMergeEvents=*/true, clock.getIOContext(),
                /*doFsync=*/true);
            b1 = b2;

            // Bucket is referenced by b1, b2 and the BucketManager.
            CHECK(b1.use_count() == 3);

            std::shared_ptr<LiveBucket> b3 = LiveBucket::fresh(
                app->getBucketManager(), getAppLedgerVersion(app), {}, live,
                dead, /*countMergeEvents=*/true, clock.getIOContext(),
                /*doFsync=*/true);
            std::shared_ptr<LiveBucket> b4 = LiveBucket::fresh(
                app->getBucketManager(), getAppLedgerVersion(app), {}, live,
                dead, /*countMergeEvents=*/true, clock.getIOContext(),
                /*doFsync=*/true);
            // Bucket is referenced by b1, b2, b3, b4 and the BucketManager.
            CHECK(b1.use_count() == 5);
        }

        // Take pointer by reference to not mess up use_count()
        auto dropBucket = [&](std::shared_ptr<LiveBucket>& b) {
            std::string filename = b->getFilename().string();
            std::string indexFilename =
                app->getBucketManager().bucketIndexFilename(b->getHash());
            CHECK(fs::exists(filename));
            CHECK(fs::exists(indexFilename));

            b.reset();
            app->getBucketManager().forgetUnreferencedBuckets(
                app->getLedgerManager().getLastClosedLedgerHAS());
            CHECK(!fs::exists(filename));
            CHECK(!fs::exists(indexFilename));
        };

        // Bucket is now only referenced by b1 and the BucketManager.
        CHECK(b1.use_count() == 2);

        // Drop bucket ourselves then purge bucketManager.
        dropBucket(b1);

        // Try adding a bucket to the BucketManager's bucketlist
        auto& bl = app->getBucketManager().getLiveBucketList();
        bl.addBatch(*app, 1, getAppLedgerVersion(app), {}, live, dead);
        clearFutures(app, bl);
        b1 = bl.getLevel(0).getCurr();

        // Bucket should be referenced by bucketlist itself, BucketManager
        // cache and b1.
        CHECK(b1.use_count() == 3);

        // This shouldn't change if we forget unreferenced buckets since
        // it's referenced by bucketlist.
        app->getBucketManager().forgetUnreferencedBuckets(
            app->getLedgerManager().getLastClosedLedgerHAS());
        CHECK(b1.use_count() == 3);

        // But if we mutate the curr bucket of the bucketlist, it should.
        live[0] = LedgerTestUtils::generateValidLedgerEntryWithExclusions(
            {CONFIG_SETTING});
        bl.addBatch(*app, 1, getAppLedgerVersion(app), {}, live, dead);
        clearFutures(app, bl);
        CHECK(b1.use_count() == 2);

        // Drop it again.
        dropBucket(b1);
    });
}

TEST_CASE("bucketmanager missing buckets fail", "[bucket][bucketmanager]")
{
    Config cfg(getTestConfig(0, Config::TESTDB_BUCKET_DB_PERSISTENT));
    std::string someBucketFileName;
    {
        VirtualClock clock;
        auto app = createTestApplication<BucketTestApplication>(clock, cfg);
        BucketManager& bm = app->getBucketManager();
        LiveBucketList& bl = bm.getLiveBucketList();
        LedgerManagerForBucketTests& lm = app->getLedgerManager();

        uint32_t ledger = 0;
        uint32_t level = 3;
        do
        {
            ++ledger;
            lm.setNextLedgerEntryBatchForBucketTesting(
                {},
                LedgerTestUtils::generateValidUniqueLedgerEntriesWithExclusions(
                    {CONFIG_SETTING}, 10),
                {});
            closeLedger(*app);
        } while (!LiveBucketList::levelShouldSpill(ledger, level - 1));
        auto someBucket = bl.getLevel(1).getCurr();
        someBucketFileName = someBucket->getFilename().string();
    }

    // Delete a bucket from the bucket dir
    REQUIRE(std::remove(someBucketFileName.c_str()) == 0);

    // Check that restarting the app crashes.
    {
        VirtualClock clock;
        Application::pointer app = createTestApplication(
            clock, cfg, /*newDB=*/false, /*startApp*/ false);
        CHECK_THROWS_AS(app->start(), std::runtime_error);
    }
}

TEST_CASE_VERSIONS("bucketmanager reattach to finished merge",
                   "[bucket][bucketmanager]")
{
    VirtualClock clock;
    Config cfg(getTestConfig());
    cfg.ARTIFICIALLY_PESSIMIZE_MERGES_FOR_TESTING = true;
    cfg.MANUAL_CLOSE = false;

    for_versions_with_differing_bucket_logic(cfg, [&](Config const& cfg) {
        Application::pointer app = createTestApplication(clock, cfg);

        BucketManager& bm = app->getBucketManager();
        LiveBucketList& bl = bm.getLiveBucketList();
        HotArchiveBucketList& hotArchive = bm.getHotArchiveBucketList();
        auto vers = getAppLedgerVersion(app);
        bool hasHotArchive = protocolVersionStartsFrom(
            vers, LiveBucket::FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION);
        // Add some entries to get to a nontrivial merge-state.
        uint32_t ledger = 0;
        uint32_t level = 4;
        UnorderedSet<LedgerKey> addedHotArchiveKeys;

        // To prevent duplicate merges that can interfere with counters, seed
        // the starting Bucket so that each merge is unique. Otherwise, the
        // first call to addBatch will merge [{first_batch}, empty_bucket]. We
        // will then see other instances of [{first_batch}, empty_bucket] merges
        // later on as the Bucket moves its way down the bl. By providing a
        // seeded bucket, the first addBatch is a [{first_batch}, seeded_bucket]
        // merge, which will not be duplicated by empty bucket merges later. The
        // live BL is automatically seeded with the genesis ledger.
        if (hasHotArchive)
        {
            auto initialHotArchiveBucket =
                LedgerTestUtils::generateValidUniqueLedgerKeysWithTypes(
                    {CONTRACT_CODE}, 10, addedHotArchiveKeys);
            hotArchive.getLevel(0).setCurr(HotArchiveBucket::fresh(
                bm, vers, {}, initialHotArchiveBucket, {}, {},
                clock.getIOContext(), /*doFsync=*/true));
        }

        do
        {
            ++ledger;
            auto lh =
                app->getLedgerManager().getLastClosedLedgerHeader().header;
            lh.ledgerSeq = ledger;
            addLiveBatchAndUpdateSnapshot(
                *app, lh, {},
                LedgerTestUtils::generateValidLedgerEntriesWithExclusions(
                    {CONFIG_SETTING}, 10),
                {});
            if (protocolVersionStartsFrom(
                    vers,
                    LiveBucket::FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION))
            {
                addHotArchiveBatchAndUpdateSnapshot(
                    *app, lh, {}, {},
                    LedgerTestUtils::generateValidUniqueLedgerKeysWithTypes(
                        {CONTRACT_CODE}, 10, addedHotArchiveKeys));
            }
            bm.forgetUnreferencedBuckets(
                app->getLedgerManager().getLastClosedLedgerHAS());
        } while (!LiveBucketList::levelShouldSpill(ledger, level - 1));

        // Check that the merge on level isn't committed (we're in
        // ARTIFICIALLY_PESSIMIZE_MERGES_FOR_TESTING mode that does not resolve
        // eagerly)
        REQUIRE(bl.getLevel(level).getNext().isMerging());

        HistoryArchiveState has;
        if (hasHotArchive)
        {
            REQUIRE(hotArchive.getLevel(level).getNext().isMerging());
            has = HistoryArchiveState(ledger, bl, hotArchive,
                                      app->getConfig().NETWORK_PASSPHRASE);
            REQUIRE(has.hasHotArchiveBuckets());
        }
        else
        {
            has = HistoryArchiveState(ledger, bl,
                                      app->getConfig().NETWORK_PASSPHRASE);
            REQUIRE(!has.hasHotArchiveBuckets());
        }

        // Serialize HAS.
        std::string serialHas = has.toString();

        // Simulate level committing (and the FutureBucket clearing),
        // followed by the typical ledger-close bucket GC event.
        bl.getLevel(level).commit();
        REQUIRE(!bl.getLevel(level).getNext().isMerging());
        if (hasHotArchive)
        {
            hotArchive.getLevel(level).commit();
            REQUIRE(!hotArchive.getLevel(level).getNext().isMerging());
        }

        REQUIRE(
            bm.readMergeCounters<LiveBucket>().mFinishedMergeReattachments ==
            0);
        if (hasHotArchive)
        {
            REQUIRE(bm.readMergeCounters<HotArchiveBucket>()
                        .mFinishedMergeReattachments == 0);
        }

        // Deserialize HAS.
        HistoryArchiveState has2;
        has2.fromString(serialHas);

        // Reattach to _finished_ merge future on level.
        has2.currentBuckets[level].next.makeLive(
            *app, vers, LiveBucketList::keepTombstoneEntries(level));
        REQUIRE(has2.currentBuckets[level].next.isMerging());

        if (hasHotArchive)
        {
            has2.hotArchiveBuckets[level].next.makeLive(
                *app, vers, HotArchiveBucketList::keepTombstoneEntries(level));
            REQUIRE(has2.hotArchiveBuckets[level].next.isMerging());

            // Resolve reattached future.
            has2.hotArchiveBuckets[level].next.resolve();
        }

        // Resolve reattached future.
        has2.currentBuckets[level].next.resolve();

        // Check that we reattached to one finished merge per bl.
        if (hasHotArchive)
        {
            REQUIRE(bm.readMergeCounters<HotArchiveBucket>()
                        .mFinishedMergeReattachments == 1);
        }

        REQUIRE(
            bm.readMergeCounters<LiveBucket>().mFinishedMergeReattachments ==
            1);
    });
}

TEST_CASE_VERSIONS("bucketmanager reattach to running merge",
                   "[bucket][bucketmanager]")
{
    VirtualClock clock;
    Config cfg(getTestConfig(0, Config::TESTDB_BUCKET_DB_PERSISTENT));
    cfg.ARTIFICIALLY_PESSIMIZE_MERGES_FOR_TESTING = true;
    cfg.MANUAL_CLOSE = false;

    for_versions_with_differing_bucket_logic(cfg, [&](Config const& cfg) {
        Application::pointer app = createTestApplication(clock, cfg);

        BucketManager& bm = app->getBucketManager();
        LiveBucketList& bl = bm.getLiveBucketList();
        HotArchiveBucketList& hotArchive = bm.getHotArchiveBucketList();
        auto vers = getAppLedgerVersion(app);
        bool hasHotArchive = protocolVersionStartsFrom(
            vers, LiveBucket::FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION);

        // This test is a race that will (if all goes well) eventually be won:
        // we keep trying to do an immediate-reattach to a running merge and
        // will only lose in cases of extremely short-running merges that finish
        // before we have a chance to reattach. Once the merges are at all
        // nontrivial in size, we'll likely win. In practice we observe the race
        // being won within 10 ledgers.
        //
        // The amount of testing machinery we have to put in place to make this
        // a non-racy test seems to be (a) extensive and (b) not worth the
        // trouble, since it'd be testing fake circumstances anyways: the entire
        // point of reattachment is to provide a benefit in cases where this
        // race is won. So testing it _as_ a race seems reasonably fair.
        //
        // That said, nondeterminism in tests is no fun and tests that run
        // potentially-forever are no fun. So we put a statistically-unlikely
        // limit in here of 10,000 ledgers. If we consistently lose for that
        // long, there's probably something wrong with the code, and in any case
        // it's a better nondeterministic failure than timing out the entire
        // testsuite with no explanation.
        uint32_t ledger = 0;
        uint32_t limit = 10000;

        // Iterate until we've reached the limit, or stop early if both the Hot
        // Archive and live BucketList have seen a running merge reattachment.
        auto cond = [&]() {
            bool reattachmentsNotFinished;
            if (hasHotArchive)
            {
                reattachmentsNotFinished =
                    bm.readMergeCounters<HotArchiveBucket>()
                            .mRunningMergeReattachments < 1 ||
                    bm.readMergeCounters<LiveBucket>()
                            .mRunningMergeReattachments < 1;
            }
            else
            {
                reattachmentsNotFinished = bm.readMergeCounters<LiveBucket>()
                                               .mRunningMergeReattachments < 1;
            }
            return ledger < limit && reattachmentsNotFinished;
        };

        while (cond())
        {
            ++ledger;
            // Merges will start on one or more levels here, starting a race
            // between the main thread here and the background workers doing
            // the merges.
            auto lh =
                app->getLedgerManager().getLastClosedLedgerHeader().header;
            lh.ledgerSeq = ledger;
            addLiveBatchAndUpdateSnapshot(
                *app, lh, {},
                LedgerTestUtils::generateValidUniqueLedgerEntriesWithExclusions(
                    {CONFIG_SETTING}, 100),
                {});
            if (hasHotArchive)
            {
                addHotArchiveBatchAndUpdateSnapshot(
                    *app, lh,
                    LedgerTestUtils::generateValidUniqueLedgerEntriesWithTypes(
                        {CONTRACT_CODE}, 100),
                    {}, {});
            }

            bm.forgetUnreferencedBuckets(
                app->getLedgerManager().getLastClosedLedgerHAS());

            HistoryArchiveState has;
            if (hasHotArchive)
            {
                has = HistoryArchiveState(ledger, bl, hotArchive,
                                          app->getConfig().NETWORK_PASSPHRASE);
            }
            else
            {
                has = HistoryArchiveState(ledger, bl,
                                          app->getConfig().NETWORK_PASSPHRASE);
            }

            std::string serialHas = has.toString();

            // Deserialize and reactivate levels of HAS. Races with the merge
            // workers end here. One of these reactivations should eventually
            // reattach, winning the race (each time around this loop the
            // merge workers will take longer to finish, so we will likely
            // win quite shortly).
            HistoryArchiveState has2;
            has2.fromString(serialHas);
            for (uint32_t level = 0; level < LiveBucketList::kNumLevels;
                 ++level)
            {
                if (has2.currentBuckets[level].next.hasHashes())
                {
                    has2.currentBuckets[level].next.makeLive(
                        *app, vers,
                        LiveBucketList::keepTombstoneEntries(level));
                }
            }

            for (uint32_t level = 0; level < has2.hotArchiveBuckets.size();
                 ++level)
            {
                if (has2.hotArchiveBuckets[level].next.hasHashes())
                {
                    has2.hotArchiveBuckets[level].next.makeLive(
                        *app, vers,
                        HotArchiveBucketList::keepTombstoneEntries(level));
                }
            }
        }
        CLOG_INFO(Bucket, "reattached to running merge at or around ledger {}",
                  ledger);
        REQUIRE(ledger < limit);

        // Because there is a race, we can't guarantee that we'll see exactly 1
        // reattachment, but we should see at least 1.
        if (hasHotArchive)
        {
            REQUIRE(bm.readMergeCounters<HotArchiveBucket>()
                        .mRunningMergeReattachments >= 1);
        }

        REQUIRE(bm.readMergeCounters<LiveBucket>().mRunningMergeReattachments >=
                1);
    });
}

TEST_CASE("bucketmanager do not leak empty-merge futures",
          "[bucket][bucketmanager]")
{
    // The point of this test is to confirm that
    // BucketManager::noteEmptyMergeOutput is being called properly from merges
    // that produce empty outputs, and that the input buckets to those merges
    // are thereby not leaking. Disable BucketListDB so that snapshots do not
    // hold persist buckets, complicating bucket counting.
    VirtualClock clock;
    Config cfg(getTestConfig(0, Config::TESTDB_IN_MEMORY));
    cfg.ARTIFICIALLY_PESSIMIZE_MERGES_FOR_TESTING = true;
    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
        static_cast<uint32_t>(
            LiveBucket::FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY) -
        1;

    auto app = createTestApplication<BucketTestApplication>(clock, cfg);

    BucketManager& bm = app->getBucketManager();
    LiveBucketList& bl = bm.getLiveBucketList();
    LedgerManagerForBucketTests& lm = app->getLedgerManager();

    // We create 8 live ledger entries spread across 8 ledgers then add a ledger
    // that destroys all 8, which then serves to shadow-out the entries when
    // subsequent merges touch them, producing empty buckets.
    for (size_t i = 0; i < 128; ++i)
    {
        auto entries =
            LedgerTestUtils::generateValidLedgerEntriesWithExclusions(
                {CONFIG_SETTING}, 8);
        REQUIRE(entries.size() == 8);
        for (auto const& e : entries)
        {
            lm.setNextLedgerEntryBatchForBucketTesting({}, {e}, {});
            closeLedger(*app);
        }
        std::vector<LedgerKey> dead;
        for (auto const& e : entries)
        {
            dead.emplace_back(LedgerEntryKey(e));
        }
        lm.setNextLedgerEntryBatchForBucketTesting({}, {}, dead);
        closeLedger(*app);
    }

    // Now check that the bucket dir has only as many buckets as
    // we expect given the nonzero entries in the bucketlist.
    while (!bl.futuresAllResolved())
    {
        bl.resolveAnyReadyFutures();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    bm.forgetUnreferencedBuckets(
        app->getLedgerManager().getLastClosedLedgerHAS());
    auto bmRefBuckets = bm.getAllReferencedBuckets(
        app->getLedgerManager().getLastClosedLedgerHAS());
    auto bmDirBuckets = bm.getBucketHashesInBucketDirForTesting();

    // Remove the 0 bucket in case it's "referenced"; it's never a file.
    bmRefBuckets.erase(Hash());

    for (auto const& bmr : bmRefBuckets)
    {
        CLOG_DEBUG(Bucket, "bucketmanager ref: {}", binToHex(bmr));
    }
    for (auto const& bmd : bmDirBuckets)
    {
        CLOG_DEBUG(Bucket, "bucketmanager dir: {}", binToHex(bmd));
    }
    REQUIRE(bmRefBuckets.size() == bmDirBuckets.size());
}

TEST_CASE_VERSIONS(
    "bucketmanager reattach HAS from publish queue to finished merge",
    "[bucket][bucketmanager]")
{
    Config cfg(getTestConfig());
    cfg.MANUAL_CLOSE = false;
    cfg.MAX_CONCURRENT_SUBPROCESSES = 1;
    cfg.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = true;
    cfg.ARTIFICIALLY_PESSIMIZE_MERGES_FOR_TESTING = true;
    stellar::historytestutils::TmpDirHistoryConfigurator tcfg;
    cfg = tcfg.configure(cfg, true);

    for_versions_with_differing_bucket_logic(cfg, [&](Config const& cfg) {
        VirtualClock clock;
        auto app = createTestApplication<BucketTestApplication>(clock, cfg);
        auto vers = getAppLedgerVersion(app);
        auto& hm = app->getHistoryManager();
        auto& bm = app->getBucketManager();
        auto& lm = app->getLedgerManager();
        bool hasHotArchive = protocolVersionStartsFrom(
            vers, LiveBucket::FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION);
        hm.setPublicationEnabled(false);
        app->getHistoryArchiveManager().initializeHistoryArchive(
            tcfg.getArchiveDirName());
        UnorderedSet<LedgerKey> hotArchiveKeys{};
        auto lastLcl = lm.getLastClosedLedgerNum();
        while (hm.getPublishQueueCount() < 5)
        {
            // Do not merge this line with the next line: CLOG and
            // readMergeCounters each acquire a mutex, and it's possible to
            // deadlock with one of the worker threads if you try to hold them
            // both at the same time.
            auto ra =
                bm.readMergeCounters<LiveBucket>().mFinishedMergeReattachments;
            auto raHotArchive = bm.readMergeCounters<HotArchiveBucket>()
                                    .mFinishedMergeReattachments;
            CLOG_INFO(Bucket,
                      "finished-merge reattachments while queueing: live "
                      "BucketList {}, Hot Archive BucketList {}",
                      ra, raHotArchive);
            if (lm.getLastClosedLedgerNum() != lastLcl)
            {
                lastLcl = lm.getLastClosedLedgerNum();
                lm.setNextLedgerEntryBatchForBucketTesting(
                    {},
                    LedgerTestUtils::
                        generateValidUniqueLedgerEntriesWithExclusions(
                            {CONFIG_SETTING}, 100),
                    {});
                if (hasHotArchive)
                {
                    lm.setNextArchiveBatchForBucketTesting(
                        {}, {},
                        LedgerTestUtils::generateValidUniqueLedgerKeysWithTypes(
                            {CONTRACT_CODE}, 10, hotArchiveKeys));
                }
            }

            clock.crank(false);
            bm.forgetUnreferencedBuckets(
                app->getLedgerManager().getLastClosedLedgerHAS());
        }

        // We should have published nothing and have the first
        // checkpoint still queued.
        REQUIRE(hm.getPublishSuccessCount() == 0);
        REQUIRE(HistoryManager::getMinLedgerQueuedToPublish(app->getConfig()) ==
                7);

        auto oldLiveReattachments =
            bm.readMergeCounters<LiveBucket>().mFinishedMergeReattachments;
        auto oldHotArchiveReattachments =
            bm.readMergeCounters<HotArchiveBucket>()
                .mFinishedMergeReattachments;
        auto HASs = HistoryManager::getPublishQueueStates(app->getConfig());
        REQUIRE(HASs.size() == 5);
        for (auto& has : HASs)
        {
            has.prepareForPublish(*app);
            REQUIRE(has.hasHotArchiveBuckets() == hasHotArchive);
        }

        auto liveRa =
            bm.readMergeCounters<LiveBucket>().mFinishedMergeReattachments;
        auto hotArchiveRa = bm.readMergeCounters<HotArchiveBucket>()
                                .mFinishedMergeReattachments;
        if (protocolVersionIsBefore(vers,
                                    LiveBucket::FIRST_PROTOCOL_SHADOWS_REMOVED))
        {
            // Versions prior to FIRST_PROTOCOL_SHADOWS_REMOVED re-attach to
            // finished merges
            REQUIRE(liveRa > oldLiveReattachments);
            CLOG_INFO(Bucket,
                      "finished-merge reattachments after making-live: {}",
                      liveRa);

            // Sanity check: Hot archive disabled in older protocols
            releaseAssert(!hasHotArchive);
        }
        else
        {
            // Versions after FIRST_PROTOCOL_SHADOWS_REMOVED do not re-attach,
            // because merges are cleared
            REQUIRE(liveRa == oldLiveReattachments);

            if (hasHotArchive)
            {
                REQUIRE(hotArchiveRa == oldHotArchiveReattachments);
            }
        }

        // Un-cork the publication process, nothing should be broken.
        hm.setPublicationEnabled(true);
        while (hm.getPublishSuccessCount() < 5)
        {
            clock.crank(false);

            // Trim history after publishing whenever possible.
            app->getMaintainer().performMaintenance(50000);
        }
    });
}

// Running one of these tests involves comparing three timelines with different
// application lifecycles for identical outcomes.
//
// A single initial 'control' timeline runs through to the end of the time
// window without interruption, surveying various points along the way. At the
// end, a complete set of the live ledger entries as represented by the bucket
// list is collected.
//
// Then a second timeline runs in which the application is stopped and restarted
// at each of the survey points in the control timeline, comparing each such
// survey for equal outcomes (same ledger hash, bucket hashes, same bucket-list
// hash, etc.) and confirming that the merge started before the application
// stops is restarted when the application is restarted.
//
// Finally, a third timeline runs that starts and stops at all the same places,
// but _switches protocol_ on one of the boundaries, between the protocol the
// test was constructed with, and the next protocol. In this timeline the
// surveys are not expected to match (as the bucket list will behave differently
// after the protocol switch) but the final live ledger entry set should be the
// same.
//
// In all cases, we are focusing on a given "designated level" of the bucket
// list, and a few "designated ledgers" at key values before and after ledgers
// when that designated level is perturbed (either by incoming or outgoing
// spills, or snapshots).
//
// For example, if we run the test with designated level 5, level 5 snaps/spills
// once at every multiple of 2048 ledgers, and prepares (merging a level 4 spill
// into its curr) once every multiple of 512 ledgers. So we calculate a set of
// designated ledgers (+/- a few ledgers each way) in the vicinity of ledgers
// 1024, 1536, 2048, 2560, and 3072 (and so on for a few other multiples of
// 2048).
class StopAndRestartBucketMergesTest
{
    template <class BucketListT>
    static void
    resolveAllMerges(BucketListT& bl)
    {
        for (uint32 i = 0; i < BucketListT::kNumLevels; ++i)
        {
            auto& level = bl.getLevel(i);
            auto& next = level.getNext();
            if (next.isMerging())
            {
                next.resolve();
            }
        }
    }

    struct Survey
    {
        Hash mCurrBucketHash;
        Hash mSnapBucketHash;
        Hash mBucketListHash;
        Hash mHotArchiveBucketListHash;
        Hash mLedgerHeaderHash;
        MergeCounters mLiveMergeCounters;
        MergeCounters mHotArchiveMergeCounters;

        void
        checkEmptyHotArchiveMetrics() const
        {
            // If before p23, check that all hot archive metrics are zero
            CHECK(mHotArchiveMergeCounters.mPreInitEntryProtocolMerges == 0);
            CHECK(mHotArchiveMergeCounters.mPostInitEntryProtocolMerges == 0);
            CHECK(mHotArchiveMergeCounters.mPreShadowRemovalProtocolMerges ==
                  0);
            CHECK(mHotArchiveMergeCounters.mPostShadowRemovalProtocolMerges ==
                  0);
            CHECK(mHotArchiveMergeCounters.mNewMetaEntries == 0);
            CHECK(mHotArchiveMergeCounters.mNewInitEntries == 0);
            CHECK(mHotArchiveMergeCounters.mNewLiveEntries == 0);
            CHECK(mHotArchiveMergeCounters.mNewDeadEntries == 0);
            CHECK(mHotArchiveMergeCounters.mOldMetaEntries == 0);
            CHECK(mHotArchiveMergeCounters.mOldInitEntries == 0);
            CHECK(mHotArchiveMergeCounters.mOldLiveEntries == 0);
            CHECK(mHotArchiveMergeCounters.mOldDeadEntries == 0);
            CHECK(mHotArchiveMergeCounters.mOldEntriesDefaultAccepted == 0);
            CHECK(mHotArchiveMergeCounters.mNewEntriesDefaultAccepted == 0);
            CHECK(mHotArchiveMergeCounters.mNewInitEntriesMergedWithOldDead ==
                  0);
            CHECK(mHotArchiveMergeCounters.mOldInitEntriesMergedWithNewLive ==
                  0);
            CHECK(mHotArchiveMergeCounters.mOldInitEntriesMergedWithNewDead ==
                  0);
            CHECK(
                mHotArchiveMergeCounters.mNewEntriesMergedWithOldNeitherInit ==
                0);
            CHECK(mHotArchiveMergeCounters.mShadowScanSteps == 0);
            CHECK(mHotArchiveMergeCounters.mMetaEntryShadowElisions == 0);
            CHECK(mHotArchiveMergeCounters.mLiveEntryShadowElisions == 0);
            CHECK(mHotArchiveMergeCounters.mInitEntryShadowElisions == 0);
            CHECK(mHotArchiveMergeCounters.mDeadEntryShadowElisions == 0);
            CHECK(mHotArchiveMergeCounters.mOutputIteratorBufferUpdates == 0);
            CHECK(mHotArchiveMergeCounters.mOutputIteratorActualWrites == 0);
        }

        void
        dumpMergeCounters(std::string const& label, uint32_t level,
                          uint32_t protocol) const
        {
            auto dumpCounters = [&](std::string const& label, uint32_t level,
                                    MergeCounters const& counters) {
                CLOG_INFO(Bucket, "MergeCounters: {} (designated level: {})",
                          label, level);
                CLOG_INFO(Bucket, "PreInitEntryProtocolMerges: {}",
                          counters.mPreInitEntryProtocolMerges);
                CLOG_INFO(Bucket, "PostInitEntryProtocolMerges: {}",
                          counters.mPostInitEntryProtocolMerges);
                CLOG_INFO(Bucket, "mPreShadowRemovalProtocolMerges: {}",
                          counters.mPreShadowRemovalProtocolMerges);
                CLOG_INFO(Bucket, "mPostShadowRemovalProtocolMerges: {}",
                          counters.mPostShadowRemovalProtocolMerges);
                CLOG_INFO(Bucket, "RunningMergeReattachments: {}",
                          counters.mRunningMergeReattachments);
                CLOG_INFO(Bucket, "FinishedMergeReattachments: {}",
                          counters.mFinishedMergeReattachments);
                CLOG_INFO(Bucket, "NewMetaEntries: {}",
                          counters.mNewMetaEntries);
                CLOG_INFO(Bucket, "NewInitEntries: {}",
                          counters.mNewInitEntries);
                CLOG_INFO(Bucket, "NewLiveEntries: {}",
                          counters.mNewLiveEntries);
                CLOG_INFO(Bucket, "NewDeadEntries: {}",
                          counters.mNewDeadEntries);
                CLOG_INFO(Bucket, "OldMetaEntries: {}",
                          counters.mOldMetaEntries);
                CLOG_INFO(Bucket, "OldInitEntries: {}",
                          counters.mOldInitEntries);
                CLOG_INFO(Bucket, "OldLiveEntries: {}",
                          counters.mOldLiveEntries);
                CLOG_INFO(Bucket, "OldDeadEntries: {}",
                          counters.mOldDeadEntries);
                CLOG_INFO(Bucket, "OldEntriesDefaultAccepted: {}",
                          counters.mOldEntriesDefaultAccepted);
                CLOG_INFO(Bucket, "NewEntriesDefaultAccepted: {}",
                          counters.mNewEntriesDefaultAccepted);
                CLOG_INFO(Bucket, "NewInitEntriesMergedWithOldDead: {}",
                          counters.mNewInitEntriesMergedWithOldDead);
                CLOG_INFO(Bucket, "OldInitEntriesMergedWithNewLive: {}",
                          counters.mOldInitEntriesMergedWithNewLive);
                CLOG_INFO(Bucket, "OldInitEntriesMergedWithNewDead: {}",
                          counters.mOldInitEntriesMergedWithNewDead);
                CLOG_INFO(Bucket, "NewEntriesMergedWithOldNeitherInit: {}",
                          counters.mNewEntriesMergedWithOldNeitherInit);
                CLOG_INFO(Bucket, "ShadowScanSteps: {}",
                          counters.mShadowScanSteps);
                CLOG_INFO(Bucket, "MetaEntryShadowElisions: {}",
                          counters.mMetaEntryShadowElisions);
                CLOG_INFO(Bucket, "LiveEntryShadowElisions: {}",
                          counters.mLiveEntryShadowElisions);
                CLOG_INFO(Bucket, "InitEntryShadowElisions: {}",
                          counters.mInitEntryShadowElisions);
                CLOG_INFO(Bucket, "DeadEntryShadowElisions: {}",
                          counters.mDeadEntryShadowElisions);
                CLOG_INFO(Bucket, "OutputIteratorTombstoneElisions: {}",
                          counters.mOutputIteratorTombstoneElisions);
                CLOG_INFO(Bucket, "OutputIteratorBufferUpdates: {}",
                          counters.mOutputIteratorBufferUpdates);
                CLOG_INFO(Bucket, "OutputIteratorActualWrites: {}",
                          counters.mOutputIteratorActualWrites);
            };

            dumpCounters(label + " (live)", level, mLiveMergeCounters);
            if (protocolVersionStartsFrom(
                    protocol,
                    LiveBucket::FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION))
            {
                dumpCounters(label + " (hot)", level, mHotArchiveMergeCounters);
            }
        }

        void
        checkSensiblePostInitEntryMergeCounters(uint32_t protocol) const
        {
            // Check live merge counters
            CHECK(mLiveMergeCounters.mPostInitEntryProtocolMerges != 0);
            if (protocolVersionIsBefore(
                    protocol, LiveBucket::FIRST_PROTOCOL_SHADOWS_REMOVED))
            {
                CHECK(mLiveMergeCounters.mPostShadowRemovalProtocolMerges == 0);
            }
            else
            {
                CHECK(mLiveMergeCounters.mPostShadowRemovalProtocolMerges != 0);
            }

            CHECK(mLiveMergeCounters.mNewMetaEntries == 0);
            CHECK(mLiveMergeCounters.mNewInitEntries != 0);
            CHECK(mLiveMergeCounters.mNewLiveEntries != 0);
            CHECK(mLiveMergeCounters.mNewDeadEntries != 0);

            CHECK(mLiveMergeCounters.mOldMetaEntries == 0);
            CHECK(mLiveMergeCounters.mOldInitEntries != 0);
            CHECK(mLiveMergeCounters.mOldLiveEntries != 0);
            CHECK(mLiveMergeCounters.mOldDeadEntries != 0);

            CHECK(mLiveMergeCounters.mOldEntriesDefaultAccepted != 0);
            CHECK(mLiveMergeCounters.mNewEntriesDefaultAccepted != 0);
            CHECK(mLiveMergeCounters.mNewInitEntriesMergedWithOldDead != 0);
            CHECK(mLiveMergeCounters.mOldInitEntriesMergedWithNewLive != 0);
            CHECK(mLiveMergeCounters.mOldInitEntriesMergedWithNewDead != 0);
            CHECK(mLiveMergeCounters.mNewEntriesMergedWithOldNeitherInit != 0);

            if (protocolVersionIsBefore(
                    protocol, LiveBucket::FIRST_PROTOCOL_SHADOWS_REMOVED))
            {
                CHECK(mLiveMergeCounters.mShadowScanSteps != 0);
                CHECK(mLiveMergeCounters.mLiveEntryShadowElisions != 0);
            }
            else
            {
                CHECK(mLiveMergeCounters.mShadowScanSteps == 0);
                CHECK(mLiveMergeCounters.mLiveEntryShadowElisions == 0);
            }

            CHECK(mLiveMergeCounters.mMetaEntryShadowElisions == 0);
            CHECK(mLiveMergeCounters.mInitEntryShadowElisions == 0);
            CHECK(mLiveMergeCounters.mDeadEntryShadowElisions == 0);

            CHECK(mLiveMergeCounters.mOutputIteratorBufferUpdates != 0);
            CHECK(mLiveMergeCounters.mOutputIteratorActualWrites != 0);
            CHECK(mLiveMergeCounters.mOutputIteratorBufferUpdates >=
                  mLiveMergeCounters.mOutputIteratorActualWrites);

            // Check hot archive merge counters
            if (protocolVersionStartsFrom(
                    protocol,
                    LiveBucket::FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION))
            {
                CHECK(mHotArchiveMergeCounters.mPostInitEntryProtocolMerges ==
                      0);
                CHECK(
                    mHotArchiveMergeCounters.mPostShadowRemovalProtocolMerges ==
                    0);

                CHECK(mHotArchiveMergeCounters.mNewMetaEntries == 0);
                CHECK(mHotArchiveMergeCounters.mNewInitEntries == 0);
                CHECK(mHotArchiveMergeCounters.mNewLiveEntries == 0);
                CHECK(mHotArchiveMergeCounters.mNewDeadEntries == 0);

                CHECK(mHotArchiveMergeCounters.mOldMetaEntries == 0);
                CHECK(mHotArchiveMergeCounters.mOldInitEntries == 0);
                CHECK(mHotArchiveMergeCounters.mOldLiveEntries == 0);
                CHECK(mHotArchiveMergeCounters.mOldDeadEntries == 0);

                CHECK(mHotArchiveMergeCounters.mOldEntriesDefaultAccepted != 0);
                CHECK(mHotArchiveMergeCounters.mNewEntriesDefaultAccepted != 0);
                CHECK(
                    mHotArchiveMergeCounters.mNewInitEntriesMergedWithOldDead ==
                    0);
                CHECK(
                    mHotArchiveMergeCounters.mOldInitEntriesMergedWithNewLive ==
                    0);
                CHECK(
                    mHotArchiveMergeCounters.mOldInitEntriesMergedWithNewDead ==
                    0);
                CHECK(mHotArchiveMergeCounters
                          .mNewEntriesMergedWithOldNeitherInit == 0);

                CHECK(mHotArchiveMergeCounters.mShadowScanSteps == 0);
                CHECK(mHotArchiveMergeCounters.mLiveEntryShadowElisions == 0);

                CHECK(mHotArchiveMergeCounters.mMetaEntryShadowElisions == 0);
                CHECK(mHotArchiveMergeCounters.mInitEntryShadowElisions == 0);
                CHECK(mHotArchiveMergeCounters.mDeadEntryShadowElisions == 0);

                CHECK(mHotArchiveMergeCounters.mOutputIteratorBufferUpdates !=
                      0);
                CHECK(mHotArchiveMergeCounters.mOutputIteratorActualWrites !=
                      0);
                CHECK(mHotArchiveMergeCounters.mOutputIteratorBufferUpdates >=
                      mHotArchiveMergeCounters.mOutputIteratorActualWrites);
            }
            else
            {
                checkEmptyHotArchiveMetrics();
            }
        }

        void
        checkSensiblePreInitEntryMergeCounters(uint32_t protocol) const
        {
            CHECK(mLiveMergeCounters.mPreInitEntryProtocolMerges != 0);
            CHECK(mLiveMergeCounters.mPreShadowRemovalProtocolMerges != 0);

            CHECK(mLiveMergeCounters.mNewMetaEntries == 0);
            CHECK(mLiveMergeCounters.mNewInitEntries == 0);
            CHECK(mLiveMergeCounters.mNewLiveEntries != 0);
            CHECK(mLiveMergeCounters.mNewDeadEntries != 0);

            CHECK(mLiveMergeCounters.mOldMetaEntries == 0);
            CHECK(mLiveMergeCounters.mOldInitEntries == 0);
            CHECK(mLiveMergeCounters.mOldLiveEntries != 0);
            CHECK(mLiveMergeCounters.mOldDeadEntries != 0);

            CHECK(mLiveMergeCounters.mOldEntriesDefaultAccepted != 0);
            CHECK(mLiveMergeCounters.mNewEntriesDefaultAccepted != 0);
            CHECK(mLiveMergeCounters.mNewInitEntriesMergedWithOldDead == 0);
            CHECK(mLiveMergeCounters.mOldInitEntriesMergedWithNewLive == 0);
            CHECK(mLiveMergeCounters.mOldInitEntriesMergedWithNewDead == 0);
            CHECK(mLiveMergeCounters.mNewEntriesMergedWithOldNeitherInit != 0);

            CHECK(mLiveMergeCounters.mShadowScanSteps != 0);
            CHECK(mLiveMergeCounters.mMetaEntryShadowElisions == 0);
            CHECK(mLiveMergeCounters.mLiveEntryShadowElisions != 0);
            CHECK(mLiveMergeCounters.mInitEntryShadowElisions == 0);
            CHECK(mLiveMergeCounters.mDeadEntryShadowElisions != 0);

            CHECK(mLiveMergeCounters.mOutputIteratorBufferUpdates != 0);
            CHECK(mLiveMergeCounters.mOutputIteratorActualWrites != 0);
            CHECK(mLiveMergeCounters.mOutputIteratorBufferUpdates >=
                  mLiveMergeCounters.mOutputIteratorActualWrites);
        }

        void
        checkEqualMergeCounters(Survey const& other) const
        {
            auto checkCountersEqual = [](auto const& counters,
                                         auto const& other) {
                CHECK(counters.mPreInitEntryProtocolMerges ==
                      other.mPreInitEntryProtocolMerges);
                CHECK(counters.mPostInitEntryProtocolMerges ==
                      other.mPostInitEntryProtocolMerges);

                CHECK(counters.mPreShadowRemovalProtocolMerges ==
                      other.mPreShadowRemovalProtocolMerges);
                CHECK(counters.mPostShadowRemovalProtocolMerges ==
                      other.mPostShadowRemovalProtocolMerges);

                CHECK(counters.mRunningMergeReattachments ==
                      other.mRunningMergeReattachments);
                CHECK(counters.mFinishedMergeReattachments ==
                      other.mFinishedMergeReattachments);

                CHECK(counters.mNewMetaEntries == other.mNewMetaEntries);
                CHECK(counters.mNewInitEntries == other.mNewInitEntries);
                CHECK(counters.mNewLiveEntries == other.mNewLiveEntries);
                CHECK(counters.mNewDeadEntries == other.mNewDeadEntries);
                CHECK(counters.mOldMetaEntries == other.mOldMetaEntries);
                CHECK(counters.mOldInitEntries == other.mOldInitEntries);
                CHECK(counters.mOldLiveEntries == other.mOldLiveEntries);
                CHECK(counters.mOldDeadEntries == other.mOldDeadEntries);

                CHECK(counters.mOldEntriesDefaultAccepted ==
                      other.mOldEntriesDefaultAccepted);
                CHECK(counters.mNewEntriesDefaultAccepted ==
                      other.mNewEntriesDefaultAccepted);
                CHECK(counters.mNewInitEntriesMergedWithOldDead ==
                      other.mNewInitEntriesMergedWithOldDead);
                CHECK(counters.mOldInitEntriesMergedWithNewLive ==
                      other.mOldInitEntriesMergedWithNewLive);
                CHECK(counters.mOldInitEntriesMergedWithNewDead ==
                      other.mOldInitEntriesMergedWithNewDead);
                CHECK(counters.mNewEntriesMergedWithOldNeitherInit ==
                      other.mNewEntriesMergedWithOldNeitherInit);

                CHECK(counters.mShadowScanSteps == other.mShadowScanSteps);
                CHECK(counters.mMetaEntryShadowElisions ==
                      other.mMetaEntryShadowElisions);
                CHECK(counters.mLiveEntryShadowElisions ==
                      other.mLiveEntryShadowElisions);
                CHECK(counters.mInitEntryShadowElisions ==
                      other.mInitEntryShadowElisions);
                CHECK(counters.mDeadEntryShadowElisions ==
                      other.mDeadEntryShadowElisions);

                CHECK(counters.mOutputIteratorTombstoneElisions ==
                      other.mOutputIteratorTombstoneElisions);
                CHECK(counters.mOutputIteratorBufferUpdates ==
                      other.mOutputIteratorBufferUpdates);
                CHECK(counters.mOutputIteratorActualWrites ==
                      other.mOutputIteratorActualWrites);
            };

            checkCountersEqual(mLiveMergeCounters, other.mLiveMergeCounters);
            checkCountersEqual(mHotArchiveMergeCounters,
                               other.mHotArchiveMergeCounters);
        }

        void
        checkEqual(Survey const& other) const
        {
            CHECK(mCurrBucketHash == other.mCurrBucketHash);
            CHECK(mSnapBucketHash == other.mSnapBucketHash);
            CHECK(mBucketListHash == other.mBucketListHash);
            CHECK(mLedgerHeaderHash == other.mLedgerHeaderHash);
            CHECK(mHotArchiveBucketListHash == other.mHotArchiveBucketListHash);
            checkEqualMergeCounters(other);
        }
        Survey(Application& app, uint32_t level, uint32_t protocol)
        {
            LedgerManager& lm = app.getLedgerManager();
            BucketManager& bm = app.getBucketManager();
            LiveBucketList& bl = bm.getLiveBucketList();
            HotArchiveBucketList& hotBl = bm.getHotArchiveBucketList();
            // Complete those merges we're about to inspect.
            resolveAllMerges(bl);
            if (protocolVersionStartsFrom(
                    protocol,
                    LiveBucket::FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION))
            {
                resolveAllMerges(hotBl);
                mHotArchiveBucketListHash = hotBl.getHash();
                mHotArchiveMergeCounters =
                    bm.readMergeCounters<HotArchiveBucket>();
            }

            mLiveMergeCounters = bm.readMergeCounters<LiveBucket>();
            mLedgerHeaderHash = lm.getLastClosedLedgerHeader().hash;
            mBucketListHash = bl.getHash();
            BucketLevel<LiveBucket>& blv = bl.getLevel(level);
            mCurrBucketHash = blv.getCurr()->getHash();
            mSnapBucketHash = blv.getSnap()->getHash();
        }
    };

    uint32_t mProtocol;
    uint32_t mDesignatedLevel;
    std::set<uint32_t> mDesignatedLedgers;
    std::map<uint32_t, Survey> mControlSurveys;
    std::map<LedgerKey, LedgerEntry> mFinalEntries;
    std::map<LedgerKey, LedgerEntry> mFinalArchiveEntries;
    std::vector<std::vector<LedgerEntry>> mInitEntryBatches;
    std::vector<std::vector<LedgerEntry>> mLiveEntryBatches;
    std::vector<std::vector<LedgerKey>> mDeadEntryBatches;
    std::vector<std::vector<LedgerEntry>> mArchiveEntryBatches;

    // Initial entries in Hot Archive BucketList, a "genesis leger" equivalent
    // for Hot Archive
    std::vector<LedgerKey> mHotArchiveInitialBatch;

    void
    collectLedgerEntries(Application& app,
                         std::map<LedgerKey, LedgerEntry>& liveEntries,
                         std::map<LedgerKey, LedgerEntry>& archiveEntries)
    {
        auto bl = app.getBucketManager().getLiveBucketList();
        for (uint32_t i = LiveBucketList::kNumLevels; i > 0; --i)
        {
            BucketLevel<LiveBucket> const& level = bl.getLevel(i - 1);
            for (auto bucket : {level.getSnap(), level.getCurr()})
            {
                for (LiveBucketInputIterator bi(bucket); bi; ++bi)
                {
                    BucketEntry const& e = *bi;
                    if (e.type() == LIVEENTRY || e.type() == INITENTRY)
                    {
                        auto le = e.liveEntry();
                        liveEntries[LedgerEntryKey(le)] = le;
                    }
                    else
                    {
                        assert(e.type() == DEADENTRY);
                        liveEntries.erase(e.deadEntry());
                    }
                }
            }
        }

        if (protocolVersionStartsFrom(
                getAppLedgerVersion(app),
                LiveBucket::FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION))
        {
            HotArchiveBucketList& hotBl =
                app.getBucketManager().getHotArchiveBucketList();
            for (uint32_t i = HotArchiveBucketList::kNumLevels; i > 0; --i)
            {
                BucketLevel<HotArchiveBucket> const& level =
                    hotBl.getLevel(i - 1);
                for (auto bucket : {level.getSnap(), level.getCurr()})
                {
                    for (HotArchiveBucketInputIterator bi(bucket); bi; ++bi)
                    {
                        auto const& e = *bi;
                        if (e.type() == HOT_ARCHIVE_LIVE)
                        {
                            archiveEntries.erase(e.key());
                        }
                        else
                        {
                            archiveEntries[LedgerEntryKey(e.archivedEntry())] =
                                e.archivedEntry();
                        }
                    }
                }
            }
        }
    }

    void
    collectFinalLedgerEntries(Application& app)
    {
        collectLedgerEntries(app, mFinalEntries, mFinalArchiveEntries);
        CLOG_INFO(Bucket,
                  "Collected final ledger live state with {} entries, archived "
                  "state with {} entries",
                  mFinalEntries.size(), mFinalArchiveEntries.size());
    }

    void
    checkAgainstFinalLedgerEntries(Application& app)
    {
        std::map<LedgerKey, LedgerEntry> testEntries;
        std::map<LedgerKey, LedgerEntry> testArchiveEntries;
        collectLedgerEntries(app, testEntries, testArchiveEntries);
        CLOG_INFO(Bucket,
                  "Collected test ledger state with {} live entries, {} "
                  "archived entries",
                  testEntries.size(), testArchiveEntries.size());
        CHECK(testEntries.size() == mFinalEntries.size());
        CHECK(testArchiveEntries.size() == mFinalArchiveEntries.size());
        for (auto const& pair : testEntries)
        {
            CHECK(mFinalEntries[pair.first] == pair.second);
        }
        for (auto const& pair : testArchiveEntries)
        {
            CHECK(mFinalArchiveEntries[pair.first] == pair.second);
        }
    }

    void
    calculateDesignatedLedgers()
    {
        uint32_t spillFreq = LiveBucketList::levelHalf(mDesignatedLevel);
        uint32_t prepFreq =
            (mDesignatedLevel == 0
                 ? 1
                 : LiveBucketList::levelHalf(mDesignatedLevel - 1));

        uint32_t const SPILLCOUNT = 5;
        uint32_t const PREPCOUNT = 5;
        uint32_t const STEPCOUNT = 5;

        for (uint32_t nSpill = 0; nSpill < SPILLCOUNT; ++nSpill)
        {
            for (uint32_t nPrep = 0; nPrep < PREPCOUNT; ++nPrep)
            {
                for (uint32_t nStep = 0; nStep < STEPCOUNT; ++nStep)
                {
                    // For each spill we want to look in the vicinity of 2
                    // prepares before and after it (as well as _at_ the spill)
                    // and for each vicinity we want to look 2 ledgers before
                    // and after the event.
                    uint32_t target = (nSpill * spillFreq);
                    target += ((PREPCOUNT / 2) * prepFreq);
                    for (uint32_t i = 0; i < nPrep && target > prepFreq; ++i)
                    {
                        target -= prepFreq;
                    }
                    target += (STEPCOUNT / 2);
                    for (uint32_t i = 0; i < nStep && target > 1; ++i)
                    {
                        target -= 1;
                    }
                    mDesignatedLedgers.insert(target);
                }
            }
        }
        CLOG_INFO(Bucket, "Collected {} designated ledgers for level {}",
                  mDesignatedLedgers.size(), mDesignatedLevel);
        for (auto d : mDesignatedLedgers)
        {
            CLOG_INFO(Bucket, "Designated ledger: {} = {:#x}", d, d);
        }
    }

    // Designated ledgers are where stop/restart events will occur. We further
    // _survey_ ledgers +/- 1 on each side of _designated_ ledgers.
    bool
    shouldSurveyLedger(uint32_t ledger)
    {
        if (mDesignatedLedgers.find(ledger + 1) != mDesignatedLedgers.end())
        {
            return true;
        }
        if (mDesignatedLedgers.find(ledger) != mDesignatedLedgers.end())
        {
            return true;
        }
        if (ledger > 0 &&
            mDesignatedLedgers.find(ledger - 1) != mDesignatedLedgers.end())
        {
            return true;
        }
        return false;
    }

    void
    collectControlSurveys()
    {
        VirtualClock clock;
        Config cfg(getTestConfig(0, Config::TESTDB_BUCKET_DB_PERSISTENT));
        cfg.ARTIFICIALLY_PESSIMIZE_MERGES_FOR_TESTING = true;
        cfg.ARTIFICIALLY_REDUCE_MERGE_COUNTS_FOR_TESTING = true;
        cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION = mProtocol;
        assert(!mDesignatedLedgers.empty());
        uint32_t finalLedger = (*mDesignatedLedgers.rbegin()) + 1;
        CLOG_INFO(Bucket,
                  "Collecting control surveys in ledger range 2..{} = {:#x}",
                  finalLedger, finalLedger);
        auto app = createTestApplication<BucketTestApplication>(clock, cfg);
        auto hasHotArchive = protocolVersionStartsFrom(
            mProtocol,
            LiveBucket::FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION);

        std::vector<LedgerKey> allKeys;
        std::map<LedgerKey, LedgerEntry> currLive;
        std::map<LedgerKey, LedgerEntry> currDead;
        std::map<LedgerKey, LedgerEntry> currArchive;

        // To prevent duplicate merges that can interfere with counters, seed
        // the starting Bucket so that each merge is unique. Otherwise, the
        // first call to addBatch will merge [{first_batch}, empty_bucket]. We
        // will then see other instances of [{first_batch}, empty_bucket] merges
        // later on as the Bucket moves its way down the bl. By providing a
        // seeded bucket, the first addBatch is a [{first_batch}, seeded_bucket]
        // merge, which will not be duplicated by empty bucket merges later. The
        // live BL is automatically seeded with the genesis ledger.
        if (hasHotArchive)
        {
            UnorderedSet<LedgerKey> empty;
            mHotArchiveInitialBatch =
                LedgerTestUtils::generateValidUniqueLedgerKeysWithTypes(
                    {CONTRACT_CODE}, 10, empty);
            app->getBucketManager()
                .getHotArchiveBucketList()
                .getLevel(0)
                .setCurr(HotArchiveBucket::fresh(
                    app->getBucketManager(), mProtocol, {},
                    mHotArchiveInitialBatch, {}, {},
                    app->getClock().getIOContext(), /*doFsync=*/true));
        }

        for (uint32_t i = 2;
             !app->getClock().getIOContext().stopped() && i < finalLedger; ++i)
        {
            size_t nEntriesInBatch = 10;
            std::vector<LedgerEntry> initEntries;
            std::vector<LedgerEntry> liveEntries;
            std::vector<LedgerKey> deadEntries;
            std::vector<LedgerEntry> archiveEntries;
            if (mInitEntryBatches.size() > 2)
            {
                std::set<LedgerKey> changedEntries;
                for (size_t j = 0; j < nEntriesInBatch / 2; ++j)
                {
                    auto const& existingKey = rand_element(allKeys);
                    if (changedEntries.find(existingKey) !=
                        changedEntries.end())
                        continue;
                    changedEntries.insert(existingKey);
                    auto liveIter = currLive.find(existingKey);
                    auto deadIter = currDead.find(existingKey);
                    assert(liveIter == currLive.end() ||
                           deadIter == currDead.end());
                    assert(liveIter != currLive.end() ||
                           deadIter != currDead.end());
                    auto& existingEntry =
                        (liveIter == currLive.end() ? deadIter->second
                                                    : liveIter->second);
                    if (rand_flip())
                    {
                        // Try to do a to-live transition
                        LedgerTestUtils::randomlyModifyEntry(existingEntry);
                        if (liveIter == currLive.end())
                        {
                            // Currently dead: revive.
                            initEntries.emplace_back(existingEntry);
                            currLive.insert(*deadIter);
                            currDead.erase(deadIter);
                        }
                        else
                        {
                            // Already live: stays alive.
                            liveEntries.emplace_back(existingEntry);
                        }
                    }
                    else
                    {
                        // Try to do a to-dead transition
                        if (liveIter == currLive.end())
                        {
                            // Already dead: we tried!
                        }
                        else
                        {
                            // Currently alive: kill.
                            deadEntries.emplace_back(existingKey);
                            currDead.insert(*liveIter);
                            currLive.erase(liveIter);
                        }
                    }
                }
            }
            auto nInits =
                nEntriesInBatch - (liveEntries.size() + deadEntries.size());
            auto newRandom =
                LedgerTestUtils::generateValidLedgerEntriesWithExclusions(
                    {CONFIG_SETTING}, nInits);
            for (auto const& e : newRandom)
            {
                auto k = LedgerEntryKey(e);
                initEntries.emplace_back(e);
                allKeys.emplace_back(k);
                currLive.emplace(std::make_pair(k, e));
            }
            auto newRandomArchive =
                LedgerTestUtils::generateValidUniqueLedgerEntriesWithTypes(
                    {CONTRACT_CODE}, nEntriesInBatch);
            for (auto const& e : newRandomArchive)
            {
                auto k = LedgerEntryKey(e);
                auto [iter, inserted] =
                    currArchive.emplace(std::make_pair(k, e));

                // only insert new entries to Archive BucketList
                if (inserted)
                {
                    archiveEntries.emplace_back(e);
                }
            }

            mInitEntryBatches.emplace_back(initEntries);
            mLiveEntryBatches.emplace_back(liveEntries);
            mDeadEntryBatches.emplace_back(deadEntries);
            LedgerManagerForBucketTests& lm = app->getLedgerManager();
            lm.setNextLedgerEntryBatchForBucketTesting(
                mInitEntryBatches.back(), mLiveEntryBatches.back(),
                mDeadEntryBatches.back());
            if (hasHotArchive)
            {
                mArchiveEntryBatches.emplace_back(archiveEntries);
                lm.setNextArchiveBatchForBucketTesting(
                    mArchiveEntryBatches.back(), {}, {});
            }

            closeLedger(*app);
            assert(i == lm.getLastClosedLedgerHeader().header.ledgerSeq);
            if (shouldSurveyLedger(i))
            {
                CLOG_INFO(Bucket, "Taking survey at {} = {:#x}", i, i);
                mControlSurveys.insert(std::make_pair(
                    i, Survey(*app, mDesignatedLevel, mProtocol)));
            }
        }

        collectFinalLedgerEntries(*app);
    }

    void
    runStopAndRestartTest(uint32_t firstProtocol, uint32_t secondProtocol)
    {
        std::unique_ptr<VirtualClock> clock = std::make_unique<VirtualClock>();
        Config cfg(getTestConfig(0, Config::TESTDB_BUCKET_DB_PERSISTENT));
        cfg.ARTIFICIALLY_PESSIMIZE_MERGES_FOR_TESTING = true;
        cfg.ARTIFICIALLY_REDUCE_MERGE_COUNTS_FOR_TESTING = true;
        cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION = firstProtocol;
        cfg.INVARIANT_CHECKS = {};
        assert(!mDesignatedLedgers.empty());
        uint32_t finalLedger = (*mDesignatedLedgers.rbegin()) + 1;
        uint32_t currProtocol = firstProtocol;

        // If firstProtocol != secondProtocol, we will switch protocols at
        // protocolSwitchLedger. At this point the surveys are expected to
        // diverge, but the set of live ledger entries at the end of the run --
        // "what the state of the bucket list means" -- should still be
        // identical.
        uint32_t protocolSwitchLedger = *(std::next(
            mDesignatedLedgers.begin(), mDesignatedLedgers.size() / 2));

        auto app = createTestApplication<BucketTestApplication>(*clock, cfg);
        uint32_t finalLedger2 = finalLedger;
        CLOG_INFO(Bucket,
                  "Running stop/restart test in ledger range 2..{} = {:#x}",
                  finalLedger, finalLedger2);

        if (protocolVersionStartsFrom(
                firstProtocol,
                LiveBucket::FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION))
        {
            app->getBucketManager()
                .getHotArchiveBucketList()
                .getLevel(0)
                .setCurr(HotArchiveBucket::fresh(
                    app->getBucketManager(), mProtocol, {},
                    mHotArchiveInitialBatch, {}, {},
                    app->getClock().getIOContext(), /*doFsync=*/true));
        }

        for (uint32_t i = 2;
             !app->getClock().getIOContext().stopped() && i < finalLedger; ++i)
        {
            LedgerManagerForBucketTests& lm = app->getLedgerManager();
            lm.setNextLedgerEntryBatchForBucketTesting(
                mInitEntryBatches[i - 2], mLiveEntryBatches[i - 2],
                mDeadEntryBatches[i - 2]);
            resolveAllMerges(app->getBucketManager().getLiveBucketList());

            if (protocolVersionStartsFrom(
                    firstProtocol,
                    LiveBucket::FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION))
            {
                lm.setNextArchiveBatchForBucketTesting(
                    mArchiveEntryBatches[i - 2], {}, {});
                resolveAllMerges(
                    app->getBucketManager().getHotArchiveBucketList());
            }

            auto liveCountersBeforeClose =
                app->getBucketManager().readMergeCounters<LiveBucket>();
            auto archiveCountersBeforeClose =
                app->getBucketManager().readMergeCounters<HotArchiveBucket>();
            if (firstProtocol != secondProtocol && i == protocolSwitchLedger)
            {
                CLOG_INFO(Bucket,
                          "Switching protocol at ledger {} from protocol {} "
                          "to protocol {}",
                          i, firstProtocol, secondProtocol);

                auto ledgerUpgrade = LedgerUpgrade{LEDGER_UPGRADE_VERSION};
                ledgerUpgrade.newLedgerVersion() = secondProtocol;
                closeLedger(*app, std::nullopt,
                            {LedgerTestUtils::toUpgradeType(ledgerUpgrade)});
                currProtocol = secondProtocol;
            }
            else
            {
                closeLedger(*app);
            }

            assert(i == app->getLedgerManager()
                            .getLastClosedLedgerHeader()
                            .header.ledgerSeq);
            auto j = mControlSurveys.find(i);
            if (j != mControlSurveys.end())
            {
                if (LiveBucketList::levelShouldSpill(i, mDesignatedLevel - 1))
                {
                    // Confirm that there's a merge-in-progress at this level
                    // (closing ledger i should have provoked a spill from
                    // mDesignatedLevel-1 to mDesignatedLevel)
                    LiveBucketList& bl =
                        app->getBucketManager().getLiveBucketList();
                    BucketLevel<LiveBucket>& blv =
                        bl.getLevel(mDesignatedLevel);
                    REQUIRE(blv.getNext().isMerging());
                    if (protocolVersionStartsFrom(
                            currProtocol,
                            LiveBucket::
                                FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION))
                    {
                        HotArchiveBucketList& hotBl =
                            app->getBucketManager().getHotArchiveBucketList();
                        BucketLevel<HotArchiveBucket>& hotBlv =
                            hotBl.getLevel(mDesignatedLevel);
                        REQUIRE(hotBlv.getNext().isMerging());
                    }
                }

                if (currProtocol == firstProtocol)
                {
                    resolveAllMerges(
                        app->getBucketManager().getHotArchiveBucketList());
                    // Check that the survey matches expectations.
                    Survey s(*app, mDesignatedLevel, currProtocol);
                    s.checkEqual(j->second);
                }

                // Stop the application.
                CLOG_INFO(Bucket,
                          "Stopping application after closing ledger {}", i);
                app.reset();

                // Prepare for protocol upgrade
                if (firstProtocol != secondProtocol &&
                    i == protocolSwitchLedger)
                {
                    cfg.LEDGER_PROTOCOL_VERSION = secondProtocol;
                }

                // Restart the application.
                CLOG_INFO(Bucket, "Restarting application at ledger {}", i);
                clock = std::make_unique<VirtualClock>();
                app = createTestApplication<BucketTestApplication>(*clock, cfg,
                                                                   false);
                if (LiveBucketList::levelShouldSpill(i, mDesignatedLevel - 1))
                {
                    // Confirm that the merge-in-progress was restarted.
                    LiveBucketList& bl =
                        app->getBucketManager().getLiveBucketList();
                    BucketLevel<LiveBucket>& blv =
                        bl.getLevel(mDesignatedLevel);
                    REQUIRE(blv.getNext().isMerging());
                    if (protocolVersionStartsFrom(
                            currProtocol,
                            LiveBucket::
                                FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION))
                    {
                        HotArchiveBucketList& hotBl =
                            app->getBucketManager().getHotArchiveBucketList();
                        BucketLevel<HotArchiveBucket>& hotBlv =
                            hotBl.getLevel(mDesignatedLevel);
                        REQUIRE(hotBlv.getNext().isMerging());
                    }
                }

                // If there are restarted merges, we need to reset the counters
                // to the values they had _before_ the ledger-close so the
                // restarted merges don't count twice.
                app->getBucketManager().incrMergeCounters<LiveBucket>(
                    liveCountersBeforeClose);
                app->getBucketManager().incrMergeCounters<HotArchiveBucket>(
                    archiveCountersBeforeClose);

                if (currProtocol == firstProtocol)
                {
                    // Re-check that the survey matches expectations.
                    Survey s2(*app, mDesignatedLevel, currProtocol);
                    s2.checkEqual(j->second);
                }
            }
        }
        checkAgainstFinalLedgerEntries(*app);
    }

  public:
    StopAndRestartBucketMergesTest(uint32_t protocol, uint32_t designatedLevel)
        : mProtocol(protocol), mDesignatedLevel(designatedLevel)
    {
    }

    void
    run()
    {
        calculateDesignatedLedgers();
        collectControlSurveys();
        assert(!mControlSurveys.empty());
        if (protocolVersionStartsFrom(
                mProtocol,
                LiveBucket::FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY))
        {
            mControlSurveys.rbegin()->second.dumpMergeCounters(
                "control, Post-INITENTRY", mDesignatedLevel, mProtocol);
            mControlSurveys.rbegin()
                ->second.checkSensiblePostInitEntryMergeCounters(mProtocol);
        }
        else
        {
            mControlSurveys.rbegin()->second.dumpMergeCounters(
                "control, Pre-INITENTRY", mDesignatedLevel, mProtocol);
            mControlSurveys.rbegin()
                ->second.checkSensiblePreInitEntryMergeCounters(mProtocol);
        }
        runStopAndRestartTest(mProtocol, mProtocol);
        runStopAndRestartTest(mProtocol, mProtocol + 1);
    }
};

TEST_CASE("bucket persistence over app restart with initentry",
          "[bucket][bucketmanager][bp-initentry][!hide]")
{
    for (uint32_t protocol :
         {static_cast<uint32_t>(
              LiveBucket::FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY) -
              1,
          static_cast<uint32_t>(
              LiveBucket::FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY),
          static_cast<uint32_t>(LiveBucket::FIRST_PROTOCOL_SHADOWS_REMOVED)
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
              ,
          static_cast<uint32_t>(
              HotArchiveBucket::FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION)
#endif
         })
    {
        for (uint32_t level : {2, 3})
        {
            StopAndRestartBucketMergesTest t(protocol, level);
            t.run();
        }
    }
}

// Same as previous test, but runs a long time; too long to run in CI.
TEST_CASE("bucket persistence over app restart with initentry - extended",
          "[bucket][bucketmanager][bp-initentry-ext][!hide]")
{
    for (uint32_t protocol :
         {static_cast<uint32_t>(
              LiveBucket::FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY) -
              1,
          static_cast<uint32_t>(
              LiveBucket::FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY),
          static_cast<uint32_t>(LiveBucket::FIRST_PROTOCOL_SHADOWS_REMOVED)
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
              ,
          static_cast<uint32_t>(
              HotArchiveBucket::FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION)
#endif
         })
    {
        for (uint32_t level : {2, 3, 4, 5})
        {
            StopAndRestartBucketMergesTest t(protocol, level);
            t.run();
        }
    }
}

TEST_CASE_VERSIONS("bucket persistence over app restart",
                   "[bucket][bucketmanager][bucketpersist]")
{
    std::vector<stellar::LedgerKey> emptySet;
    std::vector<stellar::LedgerEntry> emptySetEntry;

    Config cfg0(getTestConfig(0, Config::TESTDB_BUCKET_DB_PERSISTENT));
    cfg0.MANUAL_CLOSE = false;

    for_versions_with_differing_bucket_logic(cfg0, [&](Config const& cfg0) {
        Config cfg1(getTestConfig(1, Config::TESTDB_BUCKET_DB_PERSISTENT));
        cfg1.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
            cfg0.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION;
        cfg1.ARTIFICIALLY_PESSIMIZE_MERGES_FOR_TESTING = true;

        auto batch_entries =
            LedgerTestUtils::generateValidUniqueLedgerEntriesWithExclusions(
                {CONFIG_SETTING, OFFER}, 111);
        auto alice = batch_entries.back();
        batch_entries.pop_back();
        std::vector<std::vector<LedgerEntry>> batches;
        for (auto const& batch_entry : batch_entries)
        {
            batches.emplace_back().push_back(batch_entry);
        }

        // Inject a common object at the first batch we're going to run
        // (batch #2) and at the pause-merge threshold; this makes the
        // pause-merge (#64, where we stop and serialize) sensitive to
        // shadowing, and requires shadows be reconstituted when the merge
        // is restarted.
        uint32_t pause = 65;
        batches[2].push_back(alice);
        batches[pause - 2].push_back(alice);

        Hash Lh1, Lh2;
        Hash Blh1, Blh2;

        // First, run an application through two ledger closes, picking up
        // the bucket and ledger closes at each.
        std::optional<SecretKey> sk;
        {
            VirtualClock clock;
            Application::pointer app = createTestApplication(clock, cfg0);
            sk = std::make_optional<SecretKey>(cfg0.NODE_SEED);
            LiveBucketList& bl = app->getBucketManager().getLiveBucketList();

            uint32_t i = 2;
            while (i < pause)
            {
                CLOG_INFO(Bucket, "Adding setup phase 1 batch {}", i);
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
                CLOG_INFO(Bucket, "Adding setup phase 2 batch {}", i);
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
            VirtualClock clock;
            Application::pointer app = createTestApplication(clock, cfg1);
            LiveBucketList& bl = app->getBucketManager().getLiveBucketList();

            uint32_t i = 2;
            while (i < pause)
            {
                CLOG_INFO(Bucket, "Adding prefix-batch {}", i);
                bl.addBatch(*app, i, getAppLedgerVersion(app), {}, batches[i],
                            emptySet);
                i++;
            }

            REQUIRE(sk);
            REQUIRE(hexAbbrev(Lh1) == hexAbbrev(closeLedger(*app, sk)));
            REQUIRE(hexAbbrev(Blh1) == hexAbbrev(bl.getHash()));

            // Confirm that there are merges-in-progress in this checkpoint.
            HistoryArchiveState has(i, bl, app->getConfig().NETWORK_PASSPHRASE);
            REQUIRE(!has.futuresAllResolved());
        }

        // Finally *restart* an app on the same config, and see if it can
        // pick up the bucket list correctly.
        cfg1.FORCE_SCP = false;
        {
            VirtualClock clock;
            Application::pointer app = Application::create(clock, cfg1, false);
            app->start();
            LiveBucketList& bl = app->getBucketManager().getLiveBucketList();

            // Confirm that we re-acquired the close-ledger state.
            REQUIRE(
                hexAbbrev(Lh1) ==
                hexAbbrev(
                    app->getLedgerManager().getLastClosedLedgerHeader().hash));
            REQUIRE(hexAbbrev(Blh1) == hexAbbrev(bl.getHash()));

            uint32_t i = pause;

            // Confirm that merges-in-progress were restarted.
            HistoryArchiveState has(i, bl, app->getConfig().NETWORK_PASSPHRASE);
            REQUIRE(!has.futuresAllResolved());

            while (i < 100)
            {
                CLOG_INFO(Bucket, "Adding suffix-batch {}", i);
                bl.addBatch(*app, i, getAppLedgerVersion(app), {}, batches[i],
                            emptySet);
                i++;
            }

            // Confirm that merges-in-progress finished with expected
            // results.
            REQUIRE(hexAbbrev(Lh2) == hexAbbrev(closeLedger(*app, sk)));
            REQUIRE(hexAbbrev(Blh2) == hexAbbrev(bl.getHash()));
        }
    });
}
