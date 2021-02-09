// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// This file contains tests for the BucketList, and mid-level invariants
// concerning the sizes of levels in it, shadowing, the propagation and
// expiration of entries as they move between levels, and so forth.

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"
#include "bucket/Bucket.h"
#include "bucket/BucketInputIterator.h"
#include "bucket/BucketList.h"
#include "bucket/BucketManager.h"
#include "bucket/BucketOutputIterator.h"
#include "bucket/BucketTests.h"
#include "ledger/test/LedgerTestUtils.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Math.h"
#include "util/Timer.h"
#include "xdrpp/autocheck.h"

#include <deque>
#include <sstream>

using namespace stellar;
using namespace BucketTests;

namespace BucketListTests
{

uint32_t
mask(uint32_t v, uint32_t m)
{
    return (v & ~(m - 1));
}
uint32_t
size(uint32_t level)
{
    return 1 << (2 * (level + 1));
}
uint32_t
half(uint32_t level)
{
    return size(level) >> 1;
}
uint32_t
prev(uint32_t level)
{
    return size(level - 1);
}
uint32_t
lowBoundExclusive(uint32_t level, uint32_t ledger)
{
    return mask(ledger, size(level));
}
uint32_t
highBoundInclusive(uint32_t level, uint32_t ledger)
{
    return mask(ledger, prev(level));
}

void
checkBucketSizeAndBounds(BucketList& bl, uint32_t ledgerSeq, uint32_t level,
                         bool isCurr)
{
    std::shared_ptr<Bucket> bucket;
    uint32_t sizeOfBucket = 0;
    uint32_t oldestLedger = 0;
    if (isCurr)
    {
        bucket = bl.getLevel(level).getCurr();
        sizeOfBucket = BucketList::sizeOfCurr(ledgerSeq, level);
        oldestLedger = BucketList::oldestLedgerInCurr(ledgerSeq, level);
    }
    else
    {
        bucket = bl.getLevel(level).getSnap();
        sizeOfBucket = BucketList::sizeOfSnap(ledgerSeq, level);
        oldestLedger = BucketList::oldestLedgerInSnap(ledgerSeq, level);
    }

    std::set<uint32_t> ledgers;
    uint32_t lbound = std::numeric_limits<uint32_t>::max();
    uint32_t ubound = 0;
    for (BucketInputIterator iter(bucket); iter; ++iter)
    {
        auto lastModified = (*iter).liveEntry().lastModifiedLedgerSeq;
        ledgers.insert(lastModified);
        lbound = std::min(lbound, lastModified);
        ubound = std::max(ubound, lastModified);
    }

    REQUIRE(ledgers.size() == sizeOfBucket);
    REQUIRE(lbound == oldestLedger);
    if (ubound > 0)
    {
        REQUIRE(ubound == oldestLedger + sizeOfBucket - 1);
    }
}

// If pred is false for ledger < L and true for ledger >= L then
// binarySearchForLedger will return L.
uint32_t
binarySearchForLedger(uint32_t lbound, uint32_t ubound,
                      const std::function<uint32_t(uint32_t)>& pred)
{
    while (lbound + 1 != ubound)
    {
        uint32_t current = (lbound + ubound) / 2;
        if (pred(current))
        {
            ubound = current;
        }
        else
        {
            lbound = current;
        }
    }
    return ubound;
}
}

using namespace BucketListTests;

TEST_CASE("bucket list", "[bucket][bucketlist]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    try
    {
        for_versions_with_differing_bucket_logic(cfg, [&](Config const& cfg) {
            Application::pointer app = createTestApplication(clock, cfg);
            BucketList bl;
            autocheck::generator<std::vector<LedgerKey>> deadGen;
            CLOG_DEBUG(Bucket, "Adding batches to bucket list");
            for (uint32_t i = 1;
                 !app->getClock().getIOContext().stopped() && i < 130; ++i)
            {
                app->getClock().crank(false);
                bl.addBatch(*app, i, getAppLedgerVersion(app), {},
                            LedgerTestUtils::generateValidLedgerEntries(8),
                            deadGen(5));
                if (i % 10 == 0)
                    CLOG_DEBUG(Bucket, "Added batch {}, hash={}", i,
                               binToHex(bl.getHash()));
                for (uint32_t j = 0; j < BucketList::kNumLevels; ++j)
                {
                    auto const& lev = bl.getLevel(j);
                    auto currSz = countEntries(lev.getCurr());
                    auto snapSz = countEntries(lev.getSnap());
                    CHECK(currSz <= BucketList::levelHalf(j) * 100);
                    CHECK(snapSz <= BucketList::levelHalf(j) * 100);
                }
            }
        });
    }
    catch (std::future_error& e)
    {
        CLOG_DEBUG(Bucket, "Test caught std::future_error {}: {}", e.code(),
                   e.what());
        REQUIRE(false);
    }
}

TEST_CASE("bucket list shadowing pre/post proto 12", "[bucket][bucketlist]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    for_versions_with_differing_bucket_logic(cfg, [&](Config const& cfg) {
        Application::pointer app = createTestApplication(clock, cfg);
        BucketList bl;

        // Alice and Bob change in every iteration.
        auto alice = LedgerTestUtils::generateValidAccountEntry(5);
        auto bob = LedgerTestUtils::generateValidAccountEntry(5);

        autocheck::generator<std::vector<LedgerKey>> deadGen;
        CLOG_DEBUG(Bucket, "Adding batches to bucket list");

        uint32_t const totalNumEntries = 1200;
        for (uint32_t i = 1;
             !app->getClock().getIOContext().stopped() && i <= totalNumEntries;
             ++i)
        {
            app->getClock().crank(false);
            auto liveBatch = LedgerTestUtils::generateValidLedgerEntries(5);

            BucketEntry BucketEntryAlice, BucketEntryBob;
            alice.balance++;
            BucketEntryAlice.type(LIVEENTRY);
            BucketEntryAlice.liveEntry().data.type(ACCOUNT);
            BucketEntryAlice.liveEntry().data.account() = alice;
            liveBatch.push_back(BucketEntryAlice.liveEntry());

            bob.balance++;
            BucketEntryBob.type(LIVEENTRY);
            BucketEntryBob.liveEntry().data.type(ACCOUNT);
            BucketEntryBob.liveEntry().data.account() = bob;
            liveBatch.push_back(BucketEntryBob.liveEntry());

            bl.addBatch(*app, i, getAppLedgerVersion(app), {}, liveBatch,
                        deadGen(5));
            if (i % 100 == 0)
            {
                CLOG_DEBUG(Bucket, "Added batch {}, hash={}", i,
                           binToHex(bl.getHash()));
                // Alice and bob should be in either curr or snap of level 0 and
                // 1
                for (uint32_t j = 0; j < 2; ++j)
                {
                    auto const& lev = bl.getLevel(j);
                    auto curr = lev.getCurr();
                    auto snap = lev.getSnap();
                    bool hasAlice =
                        (curr->containsBucketIdentity(BucketEntryAlice) ||
                         snap->containsBucketIdentity(BucketEntryAlice));
                    bool hasBob =
                        (curr->containsBucketIdentity(BucketEntryBob) ||
                         snap->containsBucketIdentity(BucketEntryBob));
                    CHECK(hasAlice);
                    CHECK(hasBob);
                }

                // Alice and Bob should never occur in level 2 .. N because they
                // were shadowed in level 0 continuously.
                for (uint32_t j = 2; j < BucketList::kNumLevels; ++j)
                {
                    auto const& lev = bl.getLevel(j);
                    auto curr = lev.getCurr();
                    auto snap = lev.getSnap();
                    bool hasAlice =
                        (curr->containsBucketIdentity(BucketEntryAlice) ||
                         snap->containsBucketIdentity(BucketEntryAlice));
                    bool hasBob =
                        (curr->containsBucketIdentity(BucketEntryBob) ||
                         snap->containsBucketIdentity(BucketEntryBob));
                    if (app->getConfig().LEDGER_PROTOCOL_VERSION <
                            Bucket::FIRST_PROTOCOL_SHADOWS_REMOVED ||
                        j > 5)
                    {
                        CHECK(!hasAlice);
                        CHECK(!hasBob);
                    }
                    // On the last iteration, when bucket list population is
                    // complete, ensure that post-FIRST_PROTOCOL_SHADOWS_REMOVED
                    // Alice and Bob appear on lower levels unshadowed.
                    else if (i == totalNumEntries)
                    {
                        CHECK(hasAlice);
                        CHECK(hasBob);
                    }
                }
            }
        }
    });
}

TEST_CASE("bucket tombstones expire at bottom level",
          "[bucket][bucketlist][tombstones]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();

    for_versions_with_differing_bucket_logic(cfg, [&](Config const& cfg) {
        Application::pointer app = createTestApplication(clock, cfg);
        BucketList bl;
        BucketManager& bm = app->getBucketManager();
        autocheck::generator<std::vector<LedgerKey>> deadGen;
        auto& mergeTimer = bm.getMergeTimer();
        CLOG_INFO(Bucket, "Establishing random bucketlist");
        for (uint32_t i = 0; i < BucketList::kNumLevels; ++i)
        {
            auto& level = bl.getLevel(i);
            level.setCurr(Bucket::fresh(
                bm, getAppLedgerVersion(app), {},
                LedgerTestUtils::generateValidLedgerEntries(8), deadGen(8),
                /*countMergeEvents=*/true, clock.getIOContext(),
                /*doFsync=*/true));
            level.setSnap(Bucket::fresh(
                bm, getAppLedgerVersion(app), {},
                LedgerTestUtils::generateValidLedgerEntries(8), deadGen(8),
                /*countMergeEvents=*/true, clock.getIOContext(),
                /*doFsync=*/true));
        }

        for (uint32_t i = 0; i < BucketList::kNumLevels; ++i)
        {
            std::vector<uint32_t> ledgers = {BucketList::levelHalf(i),
                                             BucketList::levelSize(i)};
            for (auto j : ledgers)
            {
                auto n = mergeTimer.count();
                bl.addBatch(*app, j, getAppLedgerVersion(app), {},
                            LedgerTestUtils::generateValidLedgerEntries(8),
                            deadGen(8));
                app->getClock().crank(false);
                for (uint32_t k = 0u; k < BucketList::kNumLevels; ++k)
                {
                    auto& next = bl.getLevel(k).getNext();
                    if (next.isLive())
                    {
                        next.resolve();
                    }
                }
                n = mergeTimer.count() - n;
                CLOG_INFO(Bucket,
                          "Added batch at ledger {}, merges provoked: {}", j,
                          n);
                REQUIRE(n > 0);
                REQUIRE(n < 2 * BucketList::kNumLevels);
            }
        }

        EntryCounts e0(bl.getLevel(BucketList::kNumLevels - 3).getCurr());
        EntryCounts e1(bl.getLevel(BucketList::kNumLevels - 2).getCurr());
        EntryCounts e2(bl.getLevel(BucketList::kNumLevels - 1).getCurr());
        REQUIRE(e0.nDead != 0);
        REQUIRE(e1.nDead != 0);
        REQUIRE(e2.nDead == 0);
    });
}

TEST_CASE("bucket tombstones mutually-annihilate init entries",
          "[bucket][bucketlist][bl-initentry]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();

    for_versions_with_differing_bucket_logic(cfg, [&](Config const& cfg) {
        Application::pointer app = createTestApplication(clock, cfg);
        BucketList bl;
        auto vers = getAppLedgerVersion(app);
        autocheck::generator<bool> flip;
        std::deque<LedgerEntry> entriesToModify;
        for (uint32_t i = 1; i < 512; ++i)
        {
            std::vector<LedgerEntry> initEntries =
                LedgerTestUtils::generateValidLedgerEntries(8);
            std::vector<LedgerEntry> liveEntries;
            std::vector<LedgerKey> deadEntries;
            for (auto const& e : initEntries)
            {
                entriesToModify.push_back(e);
            }
            while (entriesToModify.size() > 100)
            {
                LedgerEntry e = entriesToModify.front();
                entriesToModify.pop_front();
                if (flip())
                {
                    // Entry will survive another round of the
                    // queue.
                    if (flip())
                    {
                        // Entry will be changed before re-enqueueing.
                        LedgerTestUtils::randomlyModifyEntry(e);
                        liveEntries.push_back(e);
                    }
                    entriesToModify.push_back(e);
                }
                else
                {
                    // Entry will die.
                    deadEntries.push_back(LedgerEntryKey(e));
                }
            }
            bl.addBatch(*app, i, vers, initEntries, liveEntries, deadEntries);
            app->getClock().crank(false);
            for (uint32_t k = 0u; k < BucketList::kNumLevels; ++k)
            {
                auto& next = bl.getLevel(k).getNext();
                if (next.isLive())
                {
                    next.resolve();
                }
            }
        }
        for (uint32_t k = 0u; k < BucketList::kNumLevels; ++k)
        {
            auto const& lev = bl.getLevel(k);
            auto currSz = countEntries(lev.getCurr());
            auto snapSz = countEntries(lev.getSnap());
            if (cfg.LEDGER_PROTOCOL_VERSION >=
                Bucket::FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY)
            {
                // init/dead pairs should mutually-annihilate pretty readily as
                // they go, empirically this test peaks at buckets around 400
                // entries.
                REQUIRE((currSz + snapSz) < 500);
            }
            CLOG_INFO(Bucket, "Level {} size: {}", k, (currSz + snapSz));
        }
    });
}

TEST_CASE("single entry bubbling up", "[bucket][bucketlist][bucketbubble]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    try
    {
        for_versions_with_differing_bucket_logic(cfg, [&](Config const& cfg) {
            Application::pointer app = createTestApplication(clock, cfg);
            BucketList bl;
            std::vector<stellar::LedgerKey> emptySet;
            std::vector<stellar::LedgerEntry> emptySetEntry;

            CLOG_DEBUG(Bucket, "Adding single entry in lowest level");
            bl.addBatch(*app, 1, getAppLedgerVersion(app), {},
                        LedgerTestUtils::generateValidLedgerEntries(1),
                        emptySet);

            CLOG_DEBUG(Bucket, "Adding empty batches to bucket list");
            for (uint32_t i = 2;
                 !app->getClock().getIOContext().stopped() && i < 300; ++i)
            {
                app->getClock().crank(false);
                bl.addBatch(*app, i, getAppLedgerVersion(app), {},
                            emptySetEntry, emptySet);
                if (i % 10 == 0)
                    CLOG_DEBUG(Bucket, "Added batch {}, hash={}", i,
                               binToHex(bl.getHash()));

                CLOG_DEBUG(Bucket, "------- ledger {}", i);

                for (uint32_t j = 0; j <= BucketList::kNumLevels - 1; ++j)
                {
                    uint32_t lb = lowBoundExclusive(j, i);
                    uint32_t hb = highBoundInclusive(j, i);

                    auto const& lev = bl.getLevel(j);
                    auto currSz = countEntries(lev.getCurr());
                    auto snapSz = countEntries(lev.getSnap());
                    CLOG_DEBUG(Bucket, "ledger {}, level {} curr={} snap={}", i,
                               j, currSz, snapSz);

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
        });
    }
    catch (std::future_error& e)
    {
        CLOG_DEBUG(Bucket, "Test caught std::future_error {}: {}", e.code(),
                   e.what());
        REQUIRE(false);
    }
}

TEST_CASE("BucketList sizeOf and oldestLedgerIn relations",
          "[bucket][bucketlist][count]")
{
    std::uniform_int_distribution<uint32_t> dist;
    for (uint32_t i = 0; i < 1000; ++i)
    {
        for (uint32_t level = 0; level < BucketList::kNumLevels; ++level)
        {
            uint32_t ledger = dist(gRandomEngine);
            if (BucketList::sizeOfSnap(ledger, level) > 0)
            {
                uint32_t oldestInCurr =
                    BucketList::oldestLedgerInSnap(ledger, level) +
                    BucketList::sizeOfSnap(ledger, level);
                REQUIRE(oldestInCurr ==
                        BucketList::oldestLedgerInCurr(ledger, level));
            }
            if (BucketList::sizeOfCurr(ledger, level) > 0)
            {
                uint32_t newestInCurr =
                    BucketList::oldestLedgerInCurr(ledger, level) +
                    BucketList::sizeOfCurr(ledger, level) - 1;
                REQUIRE(newestInCurr == (level == 0
                                             ? ledger
                                             : BucketList::oldestLedgerInSnap(
                                                   ledger, level - 1) -
                                                   1));
            }
        }
    }
}

TEST_CASE("BucketList snap reaches steady state", "[bucket][bucketlist][count]")
{
    // Deliberately exclude deepest level since snap on the deepest level
    // is always empty.
    for (uint32_t level = 0; level < BucketList::kNumLevels - 1; ++level)
    {
        uint32_t const half = BucketList::levelHalf(level);

        // Use binary search (assuming that it does reach steady state)
        // to find the ledger where the snap at this level first reaches
        // max size.
        uint32_t boundary = binarySearchForLedger(
            1, std::numeric_limits<uint32_t>::max() / 2,
            [level, half](uint32_t ledger) {
                return (BucketList::sizeOfSnap(ledger, level) == half);
            });

        // Generate random ledgers above and below the split to test that
        // it was actually at steady state.
        std::uniform_int_distribution<uint32_t> distLow(1, boundary - 1);
        std::uniform_int_distribution<uint32_t> distHigh(boundary);
        for (uint32_t i = 0; i < 1000; ++i)
        {
            uint32_t low = distLow(gRandomEngine);
            uint32_t high = distHigh(gRandomEngine);
            REQUIRE(BucketList::sizeOfSnap(low, level) < half);
            REQUIRE(BucketList::sizeOfSnap(high, level) == half);
        }
    }
}

TEST_CASE("BucketList deepest curr accumulates", "[bucket][bucketlist][count]")
{
    uint32_t const deepest = BucketList::kNumLevels - 1;
    // Use binary search to find the first ledger where the deepest curr
    // first is non-empty.
    uint32_t boundary = binarySearchForLedger(
        1, std::numeric_limits<uint32_t>::max() / 2,
        [deepest](uint32_t ledger) {
            return (BucketList::sizeOfCurr(ledger, deepest) > 0);
        });
    std::uniform_int_distribution<uint32_t> distLow(1, boundary - 1);
    std::uniform_int_distribution<uint32_t> distHigh(boundary);
    for (uint32_t i = 0; i < 1000; ++i)
    {
        uint32_t low = distLow(gRandomEngine);
        uint32_t high = distHigh(gRandomEngine);
        REQUIRE(BucketList::sizeOfCurr(low, deepest) == 0);
        REQUIRE(BucketList::oldestLedgerInCurr(low, deepest) ==
                std::numeric_limits<uint32_t>::max());
        REQUIRE(BucketList::sizeOfCurr(high, deepest) > 0);
        REQUIRE(BucketList::oldestLedgerInCurr(high, deepest) == 1);

        REQUIRE(BucketList::sizeOfSnap(low, deepest) == 0);
        REQUIRE(BucketList::oldestLedgerInSnap(low, deepest) ==
                std::numeric_limits<uint32_t>::max());
        REQUIRE(BucketList::sizeOfSnap(high, deepest) == 0);
        REQUIRE(BucketList::oldestLedgerInSnap(high, deepest) ==
                std::numeric_limits<uint32_t>::max());
    }
}

TEST_CASE("BucketList sizes at ledger 1", "[bucket][bucketlist][count]")
{
    REQUIRE(BucketList::sizeOfCurr(1, 0) == 1);
    REQUIRE(BucketList::sizeOfSnap(1, 0) == 0);
    for (uint32_t level = 1; level < BucketList::kNumLevels; ++level)
    {
        REQUIRE(BucketList::sizeOfCurr(1, level) == 0);
        REQUIRE(BucketList::sizeOfSnap(1, level) == 0);
    }
}

TEST_CASE("BucketList check bucket sizes", "[bucket][bucketlist][count]")
{
    VirtualClock clock;
    Config cfg(getTestConfig());
    Application::pointer app = createTestApplication(clock, cfg);
    BucketList& bl = app->getBucketManager().getBucketList();
    std::vector<LedgerKey> emptySet;

    for (uint32_t ledgerSeq = 1; ledgerSeq <= 256; ++ledgerSeq)
    {
        if (ledgerSeq >= 2)
        {
            app->getClock().crank(false);
            auto ledgers = LedgerTestUtils::generateValidLedgerEntries(1);
            ledgers[0].lastModifiedLedgerSeq = ledgerSeq;
            bl.addBatch(*app, ledgerSeq, getAppLedgerVersion(app), {}, ledgers,
                        emptySet);
        }
        for (uint32_t level = 0; level < BucketList::kNumLevels; ++level)
        {
            checkBucketSizeAndBounds(bl, ledgerSeq, level, true);
            checkBucketSizeAndBounds(bl, ledgerSeq, level, false);
        }
    }
}

static std::string
formatX32(uint32_t v)
{
    std::ostringstream oss;
    oss << "0x" << std::hex << std::setw(8) << std::setfill('0') << v;
    return oss.str();
}

static std::string
formatU32(uint32_t v)
{
    std::ostringstream oss;
    oss << std::dec << std::setw(8) << std::setfill(' ') << v << "="
        << formatX32(v);
    return oss.str();
}

static std::string
formatLedgerList(std::vector<uint32_t> const& ledgers)
{
    std::ostringstream oss;
    bool first = true;
    for (size_t i = 0; i < ledgers.size(); ++i)
    {
        if (!first)
        {
            oss << ", ";
        }
        else
        {
            first = false;
        }
        oss << formatU32(ledgers[i]);
        if (i > 5)
        {
            break;
        }
    }
    return oss.str();
}

TEST_CASE("BucketList number dump", "[bucket][bucketlist][count][!hide]")
{
    for (uint32_t level = 0; level < BucketList::kNumLevels; ++level)
    {
        CLOG_INFO(Bucket, "levelSize({}) = {} (formally)", level,
                  formatU32(BucketList::levelSize(level)));
    }

    for (uint32_t level = 0; level < BucketList::kNumLevels; ++level)
    {
        CLOG_INFO(Bucket, "levelHalf({}) = {} (formally)", level,
                  formatU32(BucketList::levelHalf(level)));
    }

    for (uint32_t probe : {0x100, 0x10000, 0x1000000})
    {
        for (uint32_t level = 0; level < BucketList::kNumLevels; ++level)
        {
            auto sz = formatU32(BucketList::sizeOfCurr(probe, level));
            CLOG_INFO(Bucket, "sizeOfCurr({:#x}, {}) = {} (precisely)", probe,
                      level, sz);
        }

        for (uint32_t level = 0; level < BucketList::kNumLevels; ++level)
        {
            auto sz = formatU32(BucketList::sizeOfSnap(probe, level));
            CLOG_INFO(Bucket, "sizeOfSnap({:#x}, {}) = {} (precisely)", probe,
                      level, sz);
        }
    }

    std::vector<std::vector<uint32_t>> spillEvents;
    std::vector<std::vector<uint32_t>> nonMergeCommitEvents;
    std::vector<std::vector<uint32_t>> mergeCommitEvents;
    for (uint32_t level = 0; level < BucketList::kNumLevels; ++level)
    {
        spillEvents.push_back({});
        nonMergeCommitEvents.push_back({});
        mergeCommitEvents.push_back({});
    }
    for (uint32_t level = 0; level < BucketList::kNumLevels; ++level)
    {
        for (uint32_t ledger = 0; ledger < 0x1000000; ++ledger)
        {
            if (BucketList::levelShouldSpill(ledger, level))
            {
                spillEvents[level].push_back(ledger);
                if (spillEvents[level].size() > 5)
                {
                    break;
                }
            }
            if (level != 0 && BucketList::levelShouldSpill(ledger, level - 1))
            {
                uint32_t nextChangeLedger =
                    ledger + BucketList::levelHalf(level - 1);
                if (BucketList::levelShouldSpill(nextChangeLedger, level))
                {
                    nonMergeCommitEvents[level].push_back(ledger);
                }
                else
                {
                    mergeCommitEvents[level].push_back(ledger);
                }
            }
        }
    }
    for (uint32_t level = 0; level < BucketList::kNumLevels; ++level)
    {
        auto ls = formatLedgerList(spillEvents[level]);
        CLOG_INFO(Bucket, "levelShouldSpill({:#x}) = true @ {}", level, ls);
    }
    for (uint32_t level = 0; level < BucketList::kNumLevels; ++level)
    {
        auto ls = formatLedgerList(mergeCommitEvents[level]);
        CLOG_INFO(Bucket, "mergeCommit({:#x}) @ {}", level, ls);
    }
    for (uint32_t level = 0; level < BucketList::kNumLevels; ++level)
    {
        auto ls = formatLedgerList(nonMergeCommitEvents[level]);
        CLOG_INFO(Bucket, "nonMergeCommit({:#x}) @ {}", level, ls);
    }

    // Print out the full bucketlist at an arbitrarily-chosen probe ledger.
    uint32_t probe = 0x11f9ab;
    CLOG_INFO(Bucket, "BucketList state at {:#x}", probe);
    for (uint32_t level = 0; level < BucketList::kNumLevels; ++level)
    {
        uint32_t currOld = BucketList::oldestLedgerInCurr(probe, level);
        uint32_t snapOld = BucketList::oldestLedgerInSnap(probe, level);
        uint32_t currSz = BucketList::sizeOfCurr(probe, level);
        uint32_t snapSz = BucketList::sizeOfSnap(probe, level);
        uint32_t currNew = currOld + currSz - 1;
        uint32_t snapNew = snapOld + snapSz - 1;
        CLOG_INFO(
            Bucket,
            "level[{:x}] curr (size:{}) = [{}, {}] snap (size:{}) = [{}, {}]",
            level, formatX32(currSz), formatX32(currOld), formatX32(currNew),
            formatX32(snapSz), formatX32(snapOld), formatX32(snapNew));
    }
}
