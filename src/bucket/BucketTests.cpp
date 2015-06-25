// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"

#include "bucket/Bucket.h"
#include "bucket/BucketList.h"
#include "bucket/BucketManager.h"
#include "bucket/LedgerCmp.h"
#include "bucket/BucketManagerImpl.h"
#include "crypto/Hex.h"
#include "ledger/LedgerManager.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/test.h"
#include "util/Fs.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include "util/TmpDir.h"
#include "util/types.h"
#include "xdrpp/autocheck.h"
#include <algorithm>
#include <future>

using namespace stellar;

namespace BucketTests
{
uint32_t
mask(uint32_t v, uint32_t m)
{
    return (v & ~(m - 1));
}
uint32_t
size(size_t level)
{
    return 1 << (2 * (level + 1));
}
uint32_t
half(size_t level)
{
    return size(level) >> 1;
}
uint32_t
prev(size_t level)
{
    return size(level - 1);
}
uint32_t
lowBoundExclusive(size_t level, uint32_t ledger)
{
    return mask(ledger, size(level));
}
uint32_t
highBoundInclusive(size_t level, uint32_t ledger)
{
    return mask(ledger, prev(level));
}

static std::ifstream::pos_type
fileSize(std::string const& name)
{
    assert(fs::exists(name));
    std::ifstream in(name, std::ifstream::ate | std::ifstream::binary);
    return in.tellg();
}

static size_t
countEntries(std::shared_ptr<Bucket> bucket)
{
    auto pair = bucket->countLiveAndDeadEntries();
    return pair.first + pair.second;
}
}

using namespace BucketTests;

TEST_CASE("skip list", "[bucket]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application::pointer app = Application::create(clock, cfg);

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
            Hash h1 = SecretKey::random().getSeed();
            Hash h2 = SecretKey::random().getSeed();
            Hash h3 = SecretKey::random().getSeed();
            Hash h4 = SecretKey::random().getSeed();
            Hash h5 = SecretKey::random().getSeed();
            Hash h6 = SecretKey::random().getSeed();
            Hash h7 = SecretKey::random().getSeed();

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

TEST_CASE("bucket list", "[bucket]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    try
    {
        Application::pointer app = Application::create(clock, cfg);
        BucketList bl;
        autocheck::generator<std::vector<LedgerEntry>> liveGen;
        autocheck::generator<std::vector<LedgerKey>> deadGen;
        CLOG(DEBUG, "Bucket") << "Adding batches to bucket list";
        for (uint32_t i = 1;
             !app->getClock().getIOService().stopped() && i < 130; ++i)
        {
            app->getClock().crank(false);
            bl.addBatch(*app, i, liveGen(8), deadGen(5));
            if (i % 10 == 0)
                CLOG(DEBUG, "Bucket") << "Added batch " << i
                                      << ", hash=" << binToHex(bl.getHash());
            for (size_t j = 0; j < BucketList::kNumLevels; ++j)
            {
                auto const& lev = bl.getLevel(j);
                auto currSz = countEntries(lev.getCurr());
                auto snapSz = countEntries(lev.getSnap());
                // CLOG(DEBUG, "Bucket") << "level " << j
                //            << " curr=" << currSz
                //            << " snap=" << snapSz;
                CHECK(currSz <= BucketList::levelHalf(j) * 100);
                CHECK(snapSz <= BucketList::levelHalf(j) * 100);
            }
        }
    }
    catch (std::future_error& e)
    {
        CLOG(DEBUG, "Bucket") << "Test caught std::future_error " << e.code()
                              << ": " << e.what();
        REQUIRE(false);
    }
}

TEST_CASE("bucket list shadowing", "[bucket]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application::pointer app = Application::create(clock, cfg);
    BucketList bl;

    // Alice and Bob change in every iteration.
    autocheck::generator<AccountEntry> accountGen;
    auto alice = accountGen(5);
    auto bob = accountGen(5);

    autocheck::generator<std::vector<LedgerEntry>> liveGen;
    autocheck::generator<std::vector<LedgerKey>> deadGen;
    CLOG(DEBUG, "Bucket") << "Adding batches to bucket list";

    for (uint32_t i = 1; !app->getClock().getIOService().stopped() && i < 1200;
         ++i)
    {
        app->getClock().crank(false);
        auto liveBatch = liveGen(5);

        BucketEntry BucketEntryAlice, BucketEntryBob;
        alice.balance++;
        BucketEntryAlice.type(LIVEENTRY);
        BucketEntryAlice.liveEntry().type(ACCOUNT);
        BucketEntryAlice.liveEntry().account() = alice;
        liveBatch.push_back(BucketEntryAlice.liveEntry());

        bob.balance++;
        BucketEntryBob.type(LIVEENTRY);
        BucketEntryBob.liveEntry().type(ACCOUNT);
        BucketEntryBob.liveEntry().account() = bob;
        liveBatch.push_back(BucketEntryBob.liveEntry());

        bl.addBatch(*app, i, liveBatch, deadGen(5));
        if (i % 100 == 0)
        {
            CLOG(DEBUG, "Bucket") << "Added batch " << i
                                  << ", hash=" << binToHex(bl.getHash());
            // Alice and bob should be in either curr or snap of level 0 and 1
            for (size_t j = 0; j < 2; ++j)
            {
                auto const& lev = bl.getLevel(j);
                auto curr = lev.getCurr();
                auto snap = lev.getSnap();
                bool hasAlice =
                    (curr->containsBucketIdentity(BucketEntryAlice) ||
                     snap->containsBucketIdentity(BucketEntryAlice));
                bool hasBob = (curr->containsBucketIdentity(BucketEntryBob) ||
                               snap->containsBucketIdentity(BucketEntryBob));
                CHECK(hasAlice);
                CHECK(hasBob);
            }

            // Alice and Bob should never occur in level 2 .. N because they
            // were shadowed in level 0 continuously.
            for (size_t j = 2; j < BucketList::kNumLevels; ++j)
            {
                auto const& lev = bl.getLevel(j);
                auto curr = lev.getCurr();
                auto snap = lev.getSnap();
                bool hasAlice =
                    (curr->containsBucketIdentity(BucketEntryAlice) ||
                     snap->containsBucketIdentity(BucketEntryAlice));
                bool hasBob = (curr->containsBucketIdentity(BucketEntryBob) ||
                               snap->containsBucketIdentity(BucketEntryBob));
                CHECK(!hasAlice);
                CHECK(!hasBob);
            }
        }
    }
}

TEST_CASE("file-backed buckets", "[bucket]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application::pointer app = Application::create(clock, cfg);

    autocheck::generator<LedgerEntry> liveGen;
    autocheck::generator<LedgerKey> deadGen;
    CLOG(DEBUG, "Bucket") << "Generating 10000 random ledger entries";
    std::vector<LedgerEntry> live(9000);
    std::vector<LedgerKey> dead(1000);
    for (auto& e : live)
        e = liveGen(3);
    for (auto& e : dead)
        e = deadGen(3);
    CLOG(DEBUG, "Bucket") << "Hashing entries";
    std::shared_ptr<Bucket> b1 =
        Bucket::fresh(app->getBucketManager(), live, dead);
    for (size_t i = 0; i < 5; ++i)
    {
        CLOG(DEBUG, "Bucket") << "Merging 10000 new ledger entries into "
                              << (i * 10000) << " entry bucket";
        for (auto& e : live)
            e = liveGen(3);
        for (auto& e : dead)
            e = deadGen(3);
        {
            TIMED_SCOPE(timerObj2, "merge");
            b1 = Bucket::merge(
                app->getBucketManager(), b1,
                Bucket::fresh(app->getBucketManager(), live, dead));
        }
    }
    CLOG(DEBUG, "Bucket") << "Spill file size: " << fileSize(b1->getFilename());
}

TEST_CASE("merging bucket entries", "[bucket]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application::pointer app = Application::create(clock, cfg);

    LedgerEntry liveEntry;
    LedgerKey deadEntry;

    autocheck::generator<LedgerEntry> leGen;
    autocheck::generator<AccountEntry> acGen;
    autocheck::generator<TrustLineEntry> tlGen;
    autocheck::generator<OfferEntry> ofGen;
    autocheck::generator<bool> flip;

    SECTION("dead account entry annihilates live account entry")
    {
        liveEntry.type(ACCOUNT);
        liveEntry.account() = acGen(10);
        deadEntry.type(ACCOUNT);
        deadEntry.account().accountID = liveEntry.account().accountID;
        std::vector<LedgerEntry> live{liveEntry};
        std::vector<LedgerKey> dead{deadEntry};
        std::shared_ptr<Bucket> b1 =
            Bucket::fresh(app->getBucketManager(), live, dead);
        CHECK(countEntries(b1) == 1);
    }

    SECTION("dead trustline entry annihilates live trustline entry")
    {
        liveEntry.type(TRUSTLINE);
        liveEntry.trustLine() = tlGen(10);
        deadEntry.type(TRUSTLINE);
        deadEntry.trustLine().accountID = liveEntry.trustLine().accountID;
        deadEntry.trustLine().currency = liveEntry.trustLine().currency;
        std::vector<LedgerEntry> live{liveEntry};
        std::vector<LedgerKey> dead{deadEntry};
        std::shared_ptr<Bucket> b1 =
            Bucket::fresh(app->getBucketManager(), live, dead);
        CHECK(countEntries(b1) == 1);
    }

    SECTION("dead offer entry annihilates live offer entry")
    {
        liveEntry.type(OFFER);
        liveEntry.offer() = ofGen(10);
        deadEntry.type(OFFER);
        deadEntry.offer().accountID = liveEntry.offer().accountID;
        deadEntry.offer().offerID = liveEntry.offer().offerID;
        std::vector<LedgerEntry> live{liveEntry};
        std::vector<LedgerKey> dead{deadEntry};
        std::shared_ptr<Bucket> b1 =
            Bucket::fresh(app->getBucketManager(), live, dead);
        CHECK(countEntries(b1) == 1);
    }

    SECTION("random dead entries annihilates live entries")
    {
        std::vector<LedgerEntry> live(100);
        std::vector<LedgerKey> dead;
        for (auto& e : live)
        {
            e = leGen(10);
            if (flip())
            {
                dead.push_back(LedgerEntryKey(e));
            }
        }
        std::shared_ptr<Bucket> b1 =
            Bucket::fresh(app->getBucketManager(), live, dead);
        CHECK(countEntries(b1) == live.size());
        auto liveCount = b1->countLiveAndDeadEntries().first;
        CLOG(DEBUG, "Bucket") << "post-merge live count: " << liveCount
                              << " of " << live.size();
        CHECK(liveCount == live.size() - dead.size());
    }

    SECTION("random live entries overwrite live entries in any order")
    {
        std::vector<LedgerEntry> live(100);
        std::vector<LedgerKey> dead;
        for (auto& e : live)
        {
            e = leGen(10);
        }
        std::shared_ptr<Bucket> b1 =
            Bucket::fresh(app->getBucketManager(), live, dead);
        std::random_shuffle(live.begin(), live.end());
        size_t liveCount = live.size();
        for (auto& e : live)
        {
            if (flip())
            {
                e = leGen(10);
                ++liveCount;
            }
        }
        std::shared_ptr<Bucket> b2 =
            Bucket::fresh(app->getBucketManager(), live, dead);
        std::shared_ptr<Bucket> b3 =
            Bucket::merge(app->getBucketManager(), b1, b2);
        CHECK(countEntries(b3) == liveCount);
    }
}

TEST_CASE("bucketmanager ownership", "[bucket]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application::pointer app = Application::create(clock, cfg);

    autocheck::generator<LedgerEntry> leGen;
    std::vector<LedgerEntry> live{leGen(10)};
    std::vector<LedgerKey> dead{};

    std::shared_ptr<Bucket> b1;

    {
        std::shared_ptr<Bucket> b2 =
            Bucket::fresh(app->getBucketManager(), live, dead);
        b1 = b2;

        // Bucket is referenced by b1, b2 and the BucketManager.
        CHECK(b1.use_count() == 3);

        std::shared_ptr<Bucket> b3 =
            Bucket::fresh(app->getBucketManager(), live, dead);
        std::shared_ptr<Bucket> b4 =
            Bucket::fresh(app->getBucketManager(), live, dead);
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
    bl.addBatch(*app, 1, live, dead);
    b1 = bl.getLevel(0).getCurr();

    // Bucket should be referenced by bucketlist itself, BucketManager cache and
    // b1.
    CHECK(b1.use_count() == 3);

    // This shouldn't change if we forget unreferenced buckets since it's
    // referenced by bucketlist.
    app->getBucketManager().forgetUnreferencedBuckets();
    CHECK(b1.use_count() == 3);

    // But if we mutate the curr bucket of the bucketlist, it should.
    live[0] = leGen(10);
    bl.addBatch(*app, 1, live, dead);
    CHECK(b1.use_count() == 2);

    // Drop it again.
    filename = b1->getFilename();
    CHECK(fs::exists(filename));
    b1.reset();
    app->getBucketManager().forgetUnreferencedBuckets();
    CHECK(!fs::exists(filename));
}

TEST_CASE("single entry bubbling up", "[bucket][bucketbubble]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    try
    {
        Application::pointer app = Application::create(clock, cfg);
        BucketList bl;
        autocheck::generator<std::vector<LedgerEntry>> liveGen;
        std::vector<stellar::LedgerKey> emptySet;
        std::vector<stellar::LedgerEntry> emptySetEntry;

        CLOG(DEBUG, "Bucket") << "Adding single entry in lowest level";
        bl.addBatch(*app, 1, liveGen(1), emptySet);

        CLOG(DEBUG, "Bucket") << "Adding empty batches to bucket list";
        for (uint32_t i = 2;
             !app->getClock().getIOService().stopped() && i < 300; ++i)
        {
            app->getClock().crank(false);
            bl.addBatch(*app, i, emptySetEntry, emptySet);
            if (i % 10 == 0)
                CLOG(DEBUG, "Bucket") << "Added batch " << i
                                      << ", hash=" << binToHex(bl.getHash());

            CLOG(DEBUG, "Bucket") << "------- ledger " << i;

            for (size_t j = 0; j <= BucketList::kNumLevels - 1; ++j)
            {
                size_t lb = lowBoundExclusive(j, i);
                size_t hb = highBoundInclusive(j, i);

                auto const& lev = bl.getLevel(j);
                auto currSz = countEntries(lev.getCurr());
                auto snapSz = countEntries(lev.getSnap());
                CLOG(DEBUG, "Bucket") << "ledger " << i << ", level " << j
                                      << " curr=" << currSz
                                      << " snap=" << snapSz;

                if (1 > lb && 1 <= hb)
                {
                    REQUIRE((currSz + snapSz) == 1);
                }
                else
                {
                    REQUIRE(currSz == 0);
                    REQUIRE(snapSz == 0);
                }
            }
        }
    }
    catch (std::future_error& e)
    {
        CLOG(DEBUG, "Bucket") << "Test caught std::future_error " << e.code()
                              << ": " << e.what();
        REQUIRE(false);
    }
}

static Hash
closeLedger(Application& app)
{
    auto& lm = app.getLedgerManager();
    auto lclHash = lm.getLastClosedLedgerHeader().hash;
    CLOG(INFO, "Bucket")
        << "Artificially closing ledger " << lm.getLedgerNum()
        << " with lcl=" << hexAbbrev(lclHash) << ", buckets="
        << hexAbbrev(app.getBucketManager().getBucketList().getHash());
    LedgerCloseData lcd(lm.getLedgerNum(),
                        std::make_shared<TxSetFrame>(lclHash),
                        lm.getCloseTime(), static_cast<uint32>(lm.getTxFee()));
    lm.externalizeValue(lcd);
    return lm.getLastClosedLedgerHeader().hash;
}

TEST_CASE("bucket persistence over app restart", "[bucket][bucketpersist]")
{
    autocheck::generator<std::vector<LedgerEntry>> liveGen;
    autocheck::generator<LedgerEntry> liveSingleGen;
    std::vector<stellar::LedgerKey> emptySet;
    std::vector<stellar::LedgerEntry> emptySetEntry;

    VirtualClock clock;
    Config cfg0(getTestConfig(0, Config::TESTDB_ON_DISK_SQLITE));
    Config cfg1(getTestConfig(1, Config::TESTDB_ON_DISK_SQLITE));

    cfg1.ARTIFICIALLY_PESSIMIZE_MERGES_FOR_TESTING = true;

    std::vector<std::vector<LedgerEntry>> batches;
    for (size_t i = 0; i < 110; ++i)
    {
        batches.push_back(liveGen(1));
    }

    // Inject a common object at the first batch we're going to run (batch #2)
    // and at the pause-merge threshold; this makes the pause-merge (#64, where
    // we stop and serialize) sensitive to shadowing, and requires shadows be
    // reconstituted when the merge is restarted.
    auto alice = liveSingleGen(1);
    uint32_t pause = 65;
    batches[2].push_back(alice);
    batches[pause - 2].push_back(alice);

    Hash Lh1, Lh2;
    Hash Blh1, Blh2;

    // First, run an application through two ledger closes, picking up
    // the bucket and ledger closes at each.
    {
        Application::pointer app = Application::create(clock, cfg0);
        app->start();
        BucketList& bl = app->getBucketManager().getBucketList();

        uint32_t i = 2;
        while (i < pause)
        {
            CLOG(INFO, "Bucket") << "Adding setup phase 1 batch " << i;
            bl.addBatch(*app, i, batches[i], emptySet);
            i++;
        }

        Lh1 = closeLedger(*app);
        Blh1 = bl.getHash();
        REQUIRE(!isZero(Lh1));
        REQUIRE(!isZero(Blh1));

        while (i < 100)
        {
            CLOG(INFO, "Bucket") << "Adding setup phase 2 batch " << i;
            bl.addBatch(*app, i, batches[i], emptySet);
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
        Application::pointer app = Application::create(clock, cfg1);
        app->start();
        BucketList& bl = app->getBucketManager().getBucketList();

        uint32_t i = 2;
        while (i < pause)
        {
            CLOG(INFO, "Bucket") << "Adding prefix-batch " << i;
            bl.addBatch(*app, i, batches[i], emptySet);
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
    cfg1.REBUILD_DB = false;
    cfg1.FORCE_SCP = false;
    {
        Application::pointer app = Application::create(clock, cfg1);
        app->start();
        BucketList& bl = app->getBucketManager().getBucketList();

        // Confirm that we re-acquired the close-ledger state.
        REQUIRE(hexAbbrev(Lh1) ==
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
            bl.addBatch(*app, i, batches[i], emptySet);
            i++;
        }

        // Confirm that merges-in-progress finished with expected results.
        REQUIRE(hexAbbrev(Lh2) == hexAbbrev(closeLedger(*app)));
        REQUIRE(hexAbbrev(Blh2) == hexAbbrev(bl.getHash()));
    }
}
