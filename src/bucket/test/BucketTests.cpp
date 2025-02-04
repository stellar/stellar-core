// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// This file contains tests for individual Buckets, low-level invariants
// concerning the composition of buckets, the semantics of the merge
// operation(s), and the performance of merging and applying buckets to the
// database.

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"
#include "bucket/BucketInputIterator.h"
#include "bucket/BucketManager.h"
#include "bucket/BucketOutputIterator.h"
#include "bucket/HotArchiveBucket.h"
#include "bucket/LiveBucket.h"
#include "bucket/test/BucketTestUtils.h"
#include "ledger/LedgerTxn.h"
#include "ledger/test/LedgerTestUtils.h"
#include "lib/catch.hpp"
#include "lib/util/stdrandom.h"
#include "main/Application.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Fs.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "util/Timer.h"
#include "xdrpp/autocheck.h"

using namespace stellar;
using namespace BucketTestUtils;

static std::ifstream::pos_type
fileSize(std::string const& name)
{
    assert(fs::exists(name));
    std::ifstream in(name, std::ifstream::ate | std::ifstream::binary);
    REQUIRE(!!in);
    in.exceptions(std::ios::badbit);
    return in.tellg();
}

static void
for_versions_with_differing_initentry_logic(
    Config const& cfg, std::function<void(Config const&)> const& f)
{
    for_versions(
        {static_cast<uint32_t>(
             LiveBucket::FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY) -
             1,
         static_cast<uint32_t>(
             LiveBucket::FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY)},
        cfg, f);
}

TEST_CASE_VERSIONS("file backed buckets", "[bucket][bucketbench]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    for_versions_with_differing_bucket_logic(cfg, [&](Config const& cfg) {
        Application::pointer app = createTestApplication(clock, cfg);

        CLOG_DEBUG(Bucket, "Generating 10000 random ledger entries");
        auto live = LedgerTestUtils::generateValidUniqueLedgerEntries(9000);
        auto dead = LedgerTestUtils::generateValidLedgerEntryKeysWithExclusions(
            {CONFIG_SETTING}, 1000);
        CLOG_DEBUG(Bucket, "Hashing entries");
        std::shared_ptr<LiveBucket> b1 = LiveBucket::fresh(
            app->getBucketManager(), getAppLedgerVersion(app), {}, live, dead,
            /*countMergeEvents=*/true, clock.getIOContext(),
            /*doFsync=*/true);
        for (uint32_t i = 0; i < 5; ++i)
        {
            CLOG_DEBUG(Bucket,
                       "Merging 10000 new ledger entries into {} entry bucket",
                       (i * 10000));
            live = LedgerTestUtils::generateValidUniqueLedgerEntries(9000);
            dead = LedgerTestUtils::generateValidLedgerEntryKeysWithExclusions(
                {CONFIG_SETTING}, 1000);
            {
                b1 = LiveBucket::merge(
                    app->getBucketManager(),
                    app->getConfig().LEDGER_PROTOCOL_VERSION, b1,
                    LiveBucket::fresh(app->getBucketManager(),
                                      getAppLedgerVersion(app), {}, live, dead,
                                      /*countMergeEvents=*/true,
                                      clock.getIOContext(),
                                      /*doFsync=*/true),
                    /*shadows=*/{},
                    /*keepTombstoneEntries=*/true,
                    /*countMergeEvents=*/true, clock.getIOContext(),
                    /*doFsync=*/true);
            }
        }
        auto sz = static_cast<size_t>(fileSize(b1->getFilename().string()));
        CLOG_DEBUG(Bucket, "Spill file size: {}", sz);
    });
}

TEST_CASE_VERSIONS("merging bucket entries", "[bucket]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    for_versions_with_differing_bucket_logic(cfg, [&](Config const& cfg) {
        Application::pointer app = createTestApplication(clock, cfg);
        auto& bm = app->getBucketManager();
        auto vers = getAppLedgerVersion(app);

        auto checkDeadAnnihilatesLive = [&](LedgerEntryType let) {
            std::string entryType =
                xdr::xdr_traits<LedgerEntryType>::enum_name(let);
            SECTION("dead " + entryType + " annihilates live " + entryType)
            {
                LedgerEntry liveEntry;
                liveEntry.data.type(let);
                switch (let)
                {
                case ACCOUNT:
                    liveEntry.data.account() =
                        LedgerTestUtils::generateValidAccountEntry(10);
                    break;
                case TRUSTLINE:
                    liveEntry.data.trustLine() =
                        LedgerTestUtils::generateValidTrustLineEntry(10);
                    break;
                case OFFER:
                    liveEntry.data.offer() =
                        LedgerTestUtils::generateValidOfferEntry(10);
                    break;
                case DATA:
                    liveEntry.data.data() =
                        LedgerTestUtils::generateValidDataEntry(10);
                    break;
                case CLAIMABLE_BALANCE:
                    liveEntry.data.claimableBalance() =
                        LedgerTestUtils::generateValidClaimableBalanceEntry(10);
                    break;
                case LIQUIDITY_POOL:
                    liveEntry.data.liquidityPool() =
                        LedgerTestUtils::generateValidLiquidityPoolEntry(10);
                    break;
                case CONFIG_SETTING:
                    liveEntry.data.configSetting() =
                        LedgerTestUtils::generateValidConfigSettingEntry(10);
                    break;
                case CONTRACT_DATA:
                    liveEntry.data.contractData() =
                        LedgerTestUtils::generateValidContractDataEntry(10);
                    break;
                case CONTRACT_CODE:
                    liveEntry.data.contractCode() =
                        LedgerTestUtils::generateValidContractCodeEntry(10);
                    break;
                case TTL:
                    liveEntry.data.ttl() =
                        LedgerTestUtils::generateValidTTLEntry(10);
                    break;
                default:
                    abort();
                }
                auto deadEntry = LedgerEntryKey(liveEntry);
                auto bLive = LiveBucket::fresh(bm, vers, {}, {liveEntry}, {},
                                               /*countMergeEvents=*/true,
                                               clock.getIOContext(),
                                               /*doFsync=*/true);
                auto bDead = LiveBucket::fresh(bm, vers, {}, {}, {deadEntry},
                                               /*countMergeEvents=*/true,
                                               clock.getIOContext(),
                                               /*doFsync=*/true);
                auto b1 = LiveBucket::merge(
                    bm, vers, bLive, bDead, /*shadows=*/{},
                    /*keepTombstoneEntries=*/true,
                    /*countMergeEvents=*/true, clock.getIOContext(),
                    /*doFsync=*/true);
                CHECK(countEntries(b1) == 1);
            }
        };

        checkDeadAnnihilatesLive(ACCOUNT);
        checkDeadAnnihilatesLive(TRUSTLINE);
        checkDeadAnnihilatesLive(OFFER);
        checkDeadAnnihilatesLive(DATA);
        checkDeadAnnihilatesLive(CLAIMABLE_BALANCE);
        checkDeadAnnihilatesLive(LIQUIDITY_POOL);
        checkDeadAnnihilatesLive(CONFIG_SETTING);
        checkDeadAnnihilatesLive(CONTRACT_DATA);
        checkDeadAnnihilatesLive(CONTRACT_CODE);

        SECTION("random dead entries annihilates live entries")
        {
            std::vector<LedgerEntry> live =
                LedgerTestUtils::generateValidUniqueLedgerEntries(100);
            std::vector<LedgerKey> dead;
            for (auto& e : live)
            {
                if (rand_flip() && e.data.type() != CONFIG_SETTING)
                {
                    dead.push_back(LedgerEntryKey(e));
                }
            }
            auto bLive = LiveBucket::fresh(bm, vers, {}, live, {},
                                           /*countMergeEvents=*/true,
                                           clock.getIOContext(),
                                           /*doFsync=*/true);
            auto bDead = LiveBucket::fresh(bm, vers, {}, {}, dead,
                                           /*countMergeEvents=*/true,
                                           clock.getIOContext(),
                                           /*doFsync=*/true);
            auto b1 = LiveBucket::merge(bm, vers, bLive, bDead, /*shadows=*/{},
                                        /*keepTombstoneEntries=*/true,
                                        /*countMergeEvents=*/true,
                                        clock.getIOContext(),
                                        /*doFsync=*/true);
            EntryCounts e(b1);
            CHECK(e.sum() == live.size());
            CLOG_DEBUG(Bucket, "post-merge live count: {} of {}", e.nLive,
                       live.size());
            CHECK(e.nLive == live.size() - dead.size());
        }

        SECTION("random live entries overwrite live entries in any order")
        {
            std::vector<LedgerEntry> live =
                LedgerTestUtils::generateValidUniqueLedgerEntriesWithExclusions(
                    {CONFIG_SETTING}, 100);
            std::vector<LedgerKey> dead;
            std::shared_ptr<LiveBucket> b1 = LiveBucket::fresh(
                app->getBucketManager(), getAppLedgerVersion(app), {}, live,
                dead, /*countMergeEvents=*/true, clock.getIOContext(),
                /*doFsync=*/true);
            // We could just shuffle the live values, but libstdc++7 has a bug
            // that causes self-moves when shuffling such types, in a way that
            // traps in debug mode. Instead we shuffle indexes. See
            // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=85828
            std::vector<size_t> liveIdxs;
            for (size_t i = 0; i < live.size(); ++i)
            {
                liveIdxs.emplace_back(i);
            }
            stellar::shuffle(liveIdxs.begin(), liveIdxs.end(), gRandomEngine);
            for (size_t src = 0; src < live.size(); ++src)
            {
                size_t dst = liveIdxs.at(src);
                if (dst != src)
                {
                    std::swap(live.at(src), live.at(dst));
                }
            }
            size_t liveCount = live.size();
            for (auto& e : live)
            {
                if (rand_flip())
                {
                    e = LedgerTestUtils::generateValidLedgerEntryWithExclusions(
                        {CONFIG_SETTING});
                    ++liveCount;
                }
            }
            std::shared_ptr<LiveBucket> b2 = LiveBucket::fresh(
                app->getBucketManager(), getAppLedgerVersion(app), {}, live,
                dead, /*countMergeEvents=*/true, clock.getIOContext(),
                /*doFsync=*/true);
            std::shared_ptr<LiveBucket> b3 = LiveBucket::merge(
                app->getBucketManager(),
                app->getConfig().LEDGER_PROTOCOL_VERSION, b1, b2,
                /*shadows=*/{}, /*keepTombstoneEntries=*/true,
                /*countMergeEvents=*/true, clock.getIOContext(),
                /*doFsync=*/true);
            CHECK(countEntries(b3) == liveCount);
        }
    });
}

TEST_CASE_VERSIONS("merging hot archive bucket entries", "[bucket][archival]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();

    auto app = createTestApplication(clock, cfg);
    for_versions_from(23, *app, [&] {
        auto& bm = app->getBucketManager();
        auto vers = getAppLedgerVersion(app);

        SECTION("new annihilates old")
        {
            auto e1 =
                LedgerTestUtils::generateValidLedgerEntryOfType(CONTRACT_CODE);
            auto e2 =
                LedgerTestUtils::generateValidLedgerEntryOfType(CONTRACT_CODE);
            auto e3 =
                LedgerTestUtils::generateValidLedgerEntryOfType(CONTRACT_DATA);
            auto e4 =
                LedgerTestUtils::generateValidLedgerEntryOfType(CONTRACT_DATA);

            // Old bucket:
            // e1 -> ARCHIVED
            // e2 -> LIVE
            // e3 -> DELETED
            // e4 -> DELETED
            auto b1 = HotArchiveBucket::fresh(
                bm, vers, {e1}, {LedgerEntryKey(e2)},
                {LedgerEntryKey(e3), LedgerEntryKey(e4)},
                /*countMergeEvents=*/true, clock.getIOContext(),
                /*doFsync=*/true);

            // New bucket:
            // e1 -> DELETED
            // e2 -> ARCHIVED
            // e3 -> LIVE
            auto b2 = HotArchiveBucket::fresh(
                bm, vers, {e2}, {LedgerEntryKey(e3)}, {LedgerEntryKey(e1)},
                /*countMergeEvents=*/true, clock.getIOContext(),
                /*doFsync=*/true);

            // Expected result:
            // e1 -> DELETED
            // e2 -> ARCHIVED
            // e3 -> LIVE
            // e4 -> DELETED
            auto merged = HotArchiveBucket::merge(
                bm, vers, b1, b2, /*shadows=*/{},
                /*keepTombstoneEntries=*/true,
                /*countMergeEvents=*/true, clock.getIOContext(),
                /*doFsync=*/true);

            bool seen1 = false;
            bool seen4 = false;
            auto count = 0;
            for (HotArchiveBucketInputIterator iter(merged); iter; ++iter)
            {
                ++count;
                auto const& e = *iter;
                if (e.type() == HOT_ARCHIVE_ARCHIVED)
                {
                    REQUIRE(e.archivedEntry() == e2);
                }
                else if (e.type() == HOT_ARCHIVE_LIVE)
                {
                    REQUIRE(e.key() == LedgerEntryKey(e3));
                }
                else if (e.type() == HOT_ARCHIVE_DELETED)
                {
                    if (e.key() == LedgerEntryKey(e1))
                    {
                        REQUIRE(!seen1);
                        seen1 = true;
                    }
                    else if (e.key() == LedgerEntryKey(e4))
                    {
                        REQUIRE(!seen4);
                        seen4 = true;
                    }
                }
                else
                {
                    FAIL();
                }
            }

            REQUIRE(seen1);
            REQUIRE(seen4);
            REQUIRE(count == 4);
        }
    });
}

static LedgerEntry
generateAccount()
{
    LedgerEntry e;
    e.data.type(ACCOUNT);
    e.data.account() = LedgerTestUtils::generateValidAccountEntry(10);
    return e;
}

static LedgerEntry
generateSameAccountDifferentState(std::vector<LedgerEntry> const& others)
{
    assert(
        std::all_of(others.begin(), others.end(), [](LedgerEntry const& other) {
            return other.data.type() == ACCOUNT;
        }));
    assert(!others.empty());
    while (true)
    {
        auto e = generateAccount();
        e.data.account().accountID = others[0].data.account().accountID;
        if (std::none_of(others.begin(), others.end(),
                         [&](LedgerEntry const& other) { return e == other; }))
        {
            return e;
        }
    }
}

static LedgerEntry
generateDifferentAccount(std::vector<LedgerEntry> const& others)
{
    assert(
        std::all_of(others.begin(), others.end(), [](LedgerEntry const& other) {
            return other.data.type() == ACCOUNT;
        }));
    while (true)
    {
        auto e = generateAccount();
        if (std::none_of(others.begin(), others.end(),
                         [&](LedgerEntry const& other) {
                             return e.data.account().accountID ==
                                    other.data.account().accountID;
                         }))
        {
            return e;
        }
    }
}

TEST_CASE("merges proceed old-style despite newer shadows",
          "[bucket][bucketmaxprotocol]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application::pointer app = createTestApplication(clock, cfg);
    auto& bm = app->getBucketManager();
    auto v12 =
        static_cast<uint32_t>(LiveBucket::FIRST_PROTOCOL_SHADOWS_REMOVED);
    auto v11 = v12 - 1;
    auto v10 = v11 - 1;

    LedgerEntry liveEntry = generateAccount();
    LedgerEntry otherLiveA = generateDifferentAccount({liveEntry});

    auto b10first =
        LiveBucket::fresh(bm, v10, {}, {liveEntry}, {},
                          /*countMergeEvents=*/true, clock.getIOContext(),
                          /*doFsync=*/true);
    auto b10second =
        LiveBucket::fresh(bm, v10, {}, {otherLiveA}, {},
                          /*countMergeEvents=*/true, clock.getIOContext(),
                          /*doFsync=*/true);

    auto b11first =
        LiveBucket::fresh(bm, v11, {}, {liveEntry}, {},
                          /*countMergeEvents=*/true, clock.getIOContext(),
                          /*doFsync=*/true);
    auto b11second =
        LiveBucket::fresh(bm, v11, {}, {otherLiveA}, {},
                          /*countMergeEvents=*/true, clock.getIOContext(),
                          /*doFsync=*/true);

    auto b12first =
        LiveBucket::fresh(bm, v12, {}, {liveEntry}, {},
                          /*countMergeEvents=*/true, clock.getIOContext(),
                          /*doFsync=*/true);
    auto b12second =
        LiveBucket::fresh(bm, v12, {}, {otherLiveA}, {},
                          /*countMergeEvents=*/true, clock.getIOContext(),
                          /*doFsync=*/true);

    SECTION("shadow version 12")
    {
        // With proto 12, new bucket version solely depends on the snap version
        auto bucket =
            LiveBucket::merge(bm, v12, b11first, b11second,
                              /*shadows=*/{b12first},
                              /*keepTombstoneEntries=*/true,
                              /*countMergeEvents=*/true, clock.getIOContext(),
                              /*doFsync=*/true);
        REQUIRE(bucket->getBucketVersion() == v11);
    }
    SECTION("shadow versions mixed, pick lower")
    {
        // Merging older version (10) buckets, with mixed versions of shadows
        // (11, 12) Pick initentry (11) style merge
        auto bucket =
            LiveBucket::merge(bm, v12, b10first, b10second,
                              /*shadows=*/{b12first, b11second},
                              /*keepTombstoneEntries=*/true,
                              /*countMergeEvents=*/true, clock.getIOContext(),
                              /*doFsync=*/true);
        REQUIRE(bucket->getBucketVersion() == v11);
    }
    SECTION("refuse to merge new version with shadow")
    {
        REQUIRE_THROWS_AS(LiveBucket::merge(bm, v12, b12first, b12second,
                                            /*shadows=*/{b12first},
                                            /*keepTombstoneEntries=*/true,
                                            /*countMergeEvents=*/true,
                                            clock.getIOContext(),
                                            /*doFsync=*/true),
                          std::runtime_error);
    }
}

TEST_CASE("merges refuse to exceed max protocol version",
          "[bucket][bucketmaxprotocol]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application::pointer app = createTestApplication(clock, cfg);
    auto& bm = app->getBucketManager();
    auto vers = getAppLedgerVersion(app);
    LedgerEntry liveEntry = generateAccount();
    LedgerEntry otherLiveA = generateDifferentAccount({liveEntry});
    auto bold1 =
        LiveBucket::fresh(bm, vers - 1, {}, {liveEntry}, {},
                          /*countMergeEvents=*/true, clock.getIOContext(),
                          /*doFsync=*/true);
    auto bold2 =
        LiveBucket::fresh(bm, vers - 1, {}, {otherLiveA}, {},
                          /*countMergeEvents=*/true, clock.getIOContext(),
                          /*doFsync=*/true);
    auto bnew1 =
        LiveBucket::fresh(bm, vers, {}, {liveEntry}, {},
                          /*countMergeEvents=*/true, clock.getIOContext(),
                          /*doFsync=*/true);
    auto bnew2 =
        LiveBucket::fresh(bm, vers, {}, {otherLiveA}, {},
                          /*countMergeEvents=*/true, clock.getIOContext(),
                          /*doFsync=*/true);
    REQUIRE_THROWS_AS(LiveBucket::merge(bm, vers - 1, bnew1, bnew2,
                                        /*shadows=*/{},
                                        /*keepTombstoneEntries=*/true,
                                        /*countMergeEvents=*/true,
                                        clock.getIOContext(),
                                        /*doFsync=*/true),
                      std::runtime_error);
}

TEST_CASE("bucket output iterator rejects wrong-version entries",
          "[bucket][bucketinitoutput]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    auto vers_new = static_cast<uint32_t>(
        LiveBucket::FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY);
    BucketMetadata meta;
    meta.ledgerVersion = vers_new - 1;
    Application::pointer app = createTestApplication(clock, cfg);
    auto& bm = app->getBucketManager();
    BucketEntry initEntry, metaEntry;
    initEntry.type(INITENTRY);
    initEntry.liveEntry() = generateAccount();
    metaEntry.type(METAENTRY);
    metaEntry.metaEntry() = meta;
    MergeCounters mc;
    LiveBucketOutputIterator out(bm.getTmpDir(), true, meta, mc,
                                 clock.getIOContext(), /*doFsync=*/true);
    REQUIRE_THROWS_AS(out.put(initEntry), std::runtime_error);
    REQUIRE_THROWS_AS(out.put(metaEntry), std::runtime_error);
}

TEST_CASE_VERSIONS("merging bucket entries with initentry",
                   "[bucket][initentry]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    for_versions_with_differing_bucket_logic(cfg, [&](Config const& cfg) {
        CLOG_INFO(Bucket, "=== starting test app == ");
        Application::pointer app = createTestApplication(clock, cfg);
        auto& bm = app->getBucketManager();
        auto vers = getAppLedgerVersion(app);

        // Whether we're in the era of supporting or not-supporting INITENTRY.
        bool initEra = protocolVersionStartsFrom(
            vers,
            LiveBucket::FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY);

        CLOG_INFO(Bucket, "=== finished buckets for initial account == ");

        LedgerEntry liveEntry = generateAccount();
        LedgerEntry liveEntry2 = generateSameAccountDifferentState({liveEntry});
        LedgerEntry liveEntry3 =
            generateSameAccountDifferentState({liveEntry, liveEntry2});
        LedgerEntry otherLiveA = generateDifferentAccount({liveEntry});
        LedgerEntry otherLiveB =
            generateDifferentAccount({liveEntry, otherLiveA});
        LedgerEntry otherLiveC =
            generateDifferentAccount({liveEntry, otherLiveA, otherLiveB});
        LedgerEntry initEntry = generateSameAccountDifferentState(
            {liveEntry, liveEntry2, liveEntry3});
        LedgerEntry initEntry2 = generateSameAccountDifferentState(
            {initEntry, liveEntry, liveEntry2, liveEntry3});
        LedgerEntry otherInitA = generateDifferentAccount({initEntry});
        LedgerKey deadEntry = LedgerEntryKey(liveEntry);

        SECTION("dead and init account entries merge correctly")
        {
            auto bInit = LiveBucket::fresh(bm, vers, {initEntry}, {}, {},
                                           /*countMergeEvents=*/true,
                                           clock.getIOContext(),
                                           /*doFsync=*/true);
            auto bDead = LiveBucket::fresh(bm, vers, {}, {}, {deadEntry},
                                           /*countMergeEvents=*/true,
                                           clock.getIOContext(),
                                           /*doFsync=*/true);
            auto b1 = LiveBucket::merge(
                bm, cfg.LEDGER_PROTOCOL_VERSION, bInit, bDead, /*shadows=*/{},
                /*keepTombstoneEntries=*/true,
                /*countMergeEvents=*/true, clock.getIOContext(),
                /*doFsync=*/true);
            // In initEra, the INIT will make it through fresh() to the bucket,
            // and mutually annihilate on contact with the DEAD, leaving 0
            // entries. Pre-initEra, the INIT will downgrade to a LIVE during
            // fresh(), and that will be killed by the DEAD, leaving 1
            // (tombstone) entry.
            EntryCounts e(b1);
            CHECK(e.nInitOrArchived == 0);
            CHECK(e.nLive == 0);
            if (initEra)
            {
                CHECK(e.nMeta == 1);
                CHECK(e.nDead == 0);
            }
            else
            {
                CHECK(e.nMeta == 0);
                CHECK(e.nDead == 1);
            }
        }

        SECTION("dead and init entries merge with intervening live entries "
                "correctly")
        {
            auto bInit = LiveBucket::fresh(bm, vers, {initEntry}, {}, {},
                                           /*countMergeEvents=*/true,
                                           clock.getIOContext(),
                                           /*doFsync=*/true);
            auto bLive = LiveBucket::fresh(bm, vers, {}, {liveEntry}, {},
                                           /*countMergeEvents=*/true,
                                           clock.getIOContext(),
                                           /*doFsync=*/true);
            auto bDead = LiveBucket::fresh(bm, vers, {}, {}, {deadEntry},
                                           /*countMergeEvents=*/true,
                                           clock.getIOContext(),
                                           /*doFsync=*/true);
            auto bmerge1 = LiveBucket::merge(
                bm, cfg.LEDGER_PROTOCOL_VERSION, bInit, bLive, /*shadows=*/{},
                /*keepTombstoneEntries=*/true,
                /*countMergeEvents=*/true, clock.getIOContext(),
                /*doFsync=*/true);
            auto b1 = LiveBucket::merge(
                bm, cfg.LEDGER_PROTOCOL_VERSION, bmerge1, bDead, /*shadows=*/{},
                /*keepTombstoneEntries=*/true,
                /*countMergeEvents=*/true, clock.getIOContext(),
                /*doFsync=*/true);
            // The same thing should happen here as above, except that the INIT
            // will merge-over the LIVE during fresh().
            EntryCounts e(b1);
            CHECK(e.nInitOrArchived == 0);
            CHECK(e.nLive == 0);
            if (initEra)
            {
                CHECK(e.nMeta == 1);
                CHECK(e.nDead == 0);
            }
            else
            {
                CHECK(e.nMeta == 0);
                CHECK(e.nDead == 1);
            }
        }

        SECTION("dead and init entries annihilate multiple live entries via "
                "separate buckets")
        {
            auto bold = LiveBucket::fresh(bm, vers, {initEntry}, {}, {},
                                          /*countMergeEvents=*/true,
                                          clock.getIOContext(),
                                          /*doFsync=*/true);
            auto bmed = LiveBucket::fresh(
                bm, vers, {}, {otherLiveA, otherLiveB, liveEntry, otherLiveC},
                {}, /*countMergeEvents=*/true, clock.getIOContext(),
                /*doFsync=*/true);
            auto bnew = LiveBucket::fresh(bm, vers, {}, {}, {deadEntry},
                                          /*countMergeEvents=*/true,
                                          clock.getIOContext(),
                                          /*doFsync=*/true);
            EntryCounts eold(bold), emed(bmed), enew(bnew);
            if (initEra)
            {
                CHECK(eold.nMeta == 1);
                CHECK(emed.nMeta == 1);
                CHECK(enew.nMeta == 1);
                CHECK(eold.nInitOrArchived == 1);
                CHECK(eold.nLive == 0);
            }
            else
            {
                CHECK(eold.nMeta == 0);
                CHECK(emed.nMeta == 0);
                CHECK(enew.nMeta == 0);
                CHECK(eold.nInitOrArchived == 0);
                CHECK(eold.nLive == 1);
            }

            CHECK(eold.nDead == 0);

            CHECK(emed.nInitOrArchived == 0);
            CHECK(emed.nLive == 4);
            CHECK(emed.nDead == 0);

            CHECK(enew.nInitOrArchived == 0);
            CHECK(enew.nLive == 0);
            CHECK(enew.nDead == 1);

            auto bmerge1 = LiveBucket::merge(
                bm, cfg.LEDGER_PROTOCOL_VERSION, bold, bmed, /*shadows=*/{},
                /*keepTombstoneEntries=*/true,
                /*countMergeEvents=*/true, clock.getIOContext(),
                /*doFsync=*/true);
            auto bmerge2 = LiveBucket::merge(
                bm, cfg.LEDGER_PROTOCOL_VERSION, bmerge1, bnew, /*shadows=*/{},
                /*keepTombstoneEntries=*/true,
                /*countMergeEvents=*/true, clock.getIOContext(),
                /*doFsync=*/true);
            EntryCounts emerge1(bmerge1), emerge2(bmerge2);
            if (initEra)
            {
                CHECK(emerge1.nMeta == 1);
                CHECK(emerge1.nInitOrArchived == 1);
                CHECK(emerge1.nLive == 3);

                CHECK(emerge2.nMeta == 1);
                CHECK(emerge2.nDead == 0);
            }
            else
            {
                CHECK(emerge1.nMeta == 0);
                CHECK(emerge1.nInitOrArchived == 0);
                CHECK(emerge1.nLive == 4);

                CHECK(emerge2.nMeta == 0);
                CHECK(emerge2.nDead == 1);
            }
            CHECK(emerge1.nDead == 0);
            CHECK(emerge2.nInitOrArchived == 0);
            CHECK(emerge2.nLive == 3);
        }
    });
}

TEST_CASE_VERSIONS("merging bucket entries with initentry with shadows",
                   "[bucket][initentry]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    for_versions_with_differing_initentry_logic(cfg, [&](Config const& cfg) {
        CLOG_INFO(Bucket, "=== starting test app == ");
        Application::pointer app = createTestApplication(clock, cfg);
        auto& bm = app->getBucketManager();
        auto vers = getAppLedgerVersion(app);

        // Whether we're in the era of supporting or not-supporting INITENTRY.
        bool initEra = protocolVersionStartsFrom(
            vers,
            LiveBucket::FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY);

        CLOG_INFO(Bucket, "=== finished buckets for initial account == ");

        LedgerEntry liveEntry = generateAccount();
        LedgerEntry liveEntry2 = generateSameAccountDifferentState({liveEntry});
        LedgerEntry liveEntry3 =
            generateSameAccountDifferentState({liveEntry, liveEntry2});
        LedgerEntry otherLiveA = generateDifferentAccount({liveEntry});
        LedgerEntry otherLiveB =
            generateDifferentAccount({liveEntry, otherLiveA});
        LedgerEntry otherLiveC =
            generateDifferentAccount({liveEntry, otherLiveA, otherLiveB});
        LedgerEntry initEntry = generateSameAccountDifferentState(
            {liveEntry, liveEntry2, liveEntry3});
        LedgerEntry initEntry2 = generateSameAccountDifferentState(
            {initEntry, liveEntry, liveEntry2, liveEntry3});
        LedgerEntry otherInitA = generateDifferentAccount({initEntry});
        LedgerKey deadEntry = LedgerEntryKey(liveEntry);

        SECTION("shadows influence lifecycle entries appropriately")
        {
            // In pre-11 versions, shadows _do_ eliminate lifecycle entries
            // (INIT/DEAD). In 11-and-after versions, shadows _don't_ eliminate
            // lifecycle entries.
            auto shadow = LiveBucket::fresh(bm, vers, {}, {liveEntry}, {},
                                            /*countMergeEvents=*/true,
                                            clock.getIOContext(),
                                            /*doFsync=*/true);
            auto b1 = LiveBucket::fresh(bm, vers, {initEntry}, {}, {},
                                        /*countMergeEvents=*/true,
                                        clock.getIOContext(),
                                        /*doFsync=*/true);
            auto b2 = LiveBucket::fresh(bm, vers, {otherInitA}, {}, {},
                                        /*countMergeEvents=*/true,
                                        clock.getIOContext(),
                                        /*doFsync=*/true);
            auto merged = LiveBucket::merge(
                bm, cfg.LEDGER_PROTOCOL_VERSION, b1, b2,
                /*shadows=*/{shadow},
                /*keepTombstoneEntries=*/true,
                /*countMergeEvents=*/true, clock.getIOContext(),
                /*doFsync=*/true);
            EntryCounts e(merged);
            if (initEra)
            {
                CHECK(e.nMeta == 1);
                CHECK(e.nInitOrArchived == 2);
                CHECK(e.nLive == 0);
                CHECK(e.nDead == 0);
            }
            else
            {
                CHECK(e.nMeta == 0);
                CHECK(e.nInitOrArchived == 0);
                CHECK(e.nLive == 1);
                CHECK(e.nDead == 0);
            }
        }
        SECTION("shadowing does not revive dead entries")
        {
            // This is the first contrived example of what might go wrong if we
            // shadowed aggressively while supporting INIT+DEAD annihilation,
            // and why we had to change the shadowing behaviour when introducing
            // INIT. See comment in `maybePut` in Bucket.cpp.
            //
            // (level1 is newest here, level5 is oldest)
            auto level1 = LiveBucket::fresh(bm, vers, {}, {}, {deadEntry},
                                            /*countMergeEvents=*/true,
                                            clock.getIOContext(),
                                            /*doFsync=*/true);
            auto level2 = LiveBucket::fresh(bm, vers, {initEntry2}, {}, {},
                                            /*countMergeEvents=*/true,
                                            clock.getIOContext(),
                                            /*doFsync=*/true);
            auto level3 = LiveBucket::fresh(bm, vers, {}, {}, {deadEntry},
                                            /*countMergeEvents=*/true,
                                            clock.getIOContext(),
                                            /*doFsync=*/true);
            auto level4 = LiveBucket::fresh(bm, vers, {}, {}, {},
                                            /*countMergeEvents=*/true,
                                            clock.getIOContext(),
                                            /*doFsync=*/true);
            auto level5 = LiveBucket::fresh(bm, vers, {initEntry}, {}, {},
                                            /*countMergeEvents=*/true,
                                            clock.getIOContext(),
                                            /*doFsync=*/true);

            // Do a merge between levels 4 and 3, with shadows from 2 and 1,
            // risking shadowing-out level 3. Level 4 is a placeholder here,
            // just to be a thing-to-merge-level-3-with in the presence of
            // shadowing from 1 and 2.
            auto merge43 = LiveBucket::merge(
                bm, cfg.LEDGER_PROTOCOL_VERSION, level4, level3,
                /*shadows=*/{level2, level1},
                /*keepTombstoneEntries=*/true,
                /*countMergeEvents=*/true, clock.getIOContext(),
                /*doFsync=*/true);
            EntryCounts e43(merge43);
            if (initEra)
            {
                // New-style, we preserve the dead entry.
                CHECK(e43.nMeta == 1);
                CHECK(e43.nInitOrArchived == 0);
                CHECK(e43.nLive == 0);
                CHECK(e43.nDead == 1);
            }
            else
            {
                // Old-style, we shadowed-out the dead entry.
                CHECK(e43.nMeta == 0);
                CHECK(e43.nInitOrArchived == 0);
                CHECK(e43.nLive == 0);
                CHECK(e43.nDead == 0);
            }

            // Do a merge between level 2 and 1, producing potentially
            // an annihilation of their INIT and DEAD pair.
            auto merge21 = LiveBucket::merge(
                bm, cfg.LEDGER_PROTOCOL_VERSION, level2, level1,
                /*shadows=*/{},
                /*keepTombstoneEntries=*/true,
                /*countMergeEvents=*/true, clock.getIOContext(),
                /*doFsync=*/true);
            EntryCounts e21(merge21);
            if (initEra)
            {
                // New-style, they mutually annihilate.
                CHECK(e21.nMeta == 1);
                CHECK(e21.nInitOrArchived == 0);
                CHECK(e21.nLive == 0);
                CHECK(e21.nDead == 0);
            }
            else
            {
                // Old-style, we keep the tombstone around.
                CHECK(e21.nMeta == 0);
                CHECK(e21.nInitOrArchived == 0);
                CHECK(e21.nLive == 0);
                CHECK(e21.nDead == 1);
            }

            // Do two more merges: one between the two merges we've
            // done so far, and then finally one with level 5.
            auto merge4321 = LiveBucket::merge(
                bm, cfg.LEDGER_PROTOCOL_VERSION, merge43, merge21,
                /*shadows=*/{},
                /*keepTombstoneEntries=*/true,
                /*countMergeEvents=*/true, clock.getIOContext(),
                /*doFsync=*/true);
            auto merge54321 = LiveBucket::merge(
                bm, cfg.LEDGER_PROTOCOL_VERSION, level5, merge4321,
                /*shadows=*/{},
                /*keepTombstoneEntries=*/true,
                /*countMergeEvents=*/true, clock.getIOContext(),
                /*doFsync=*/true);
            EntryCounts e54321(merge21);
            if (initEra)
            {
                // New-style, we should get a second mutual annihilation.
                CHECK(e54321.nMeta == 1);
                CHECK(e54321.nInitOrArchived == 0);
                CHECK(e54321.nLive == 0);
                CHECK(e54321.nDead == 0);
            }
            else
            {
                // Old-style, the tombstone should clobber the live entry.
                CHECK(e54321.nMeta == 0);
                CHECK(e54321.nInitOrArchived == 0);
                CHECK(e54321.nLive == 0);
                CHECK(e54321.nDead == 1);
            }
        }

        SECTION("shadowing does not eliminate init entries")
        {
            // This is the second less-bad but still problematic contrived
            // example of what might go wrong if we shadowed aggressively while
            // supporting INIT+DEAD annihilation, and why we had to change the
            // shadowing behaviour when introducing INIT. See comment in
            // `maybePut` in Bucket.cpp.
            //
            // (level1 is newest here, level3 is oldest)
            auto level1 = LiveBucket::fresh(bm, vers, {}, {}, {deadEntry},
                                            /*countMergeEvents=*/true,
                                            clock.getIOContext(),
                                            /*doFsync=*/true);
            auto level2 = LiveBucket::fresh(bm, vers, {}, {liveEntry}, {},
                                            /*countMergeEvents=*/true,
                                            clock.getIOContext(),
                                            /*doFsync=*/true);
            auto level3 = LiveBucket::fresh(bm, vers, {initEntry}, {}, {},
                                            /*countMergeEvents=*/true,
                                            clock.getIOContext(),
                                            /*doFsync=*/true);

            // Do a merge between levels 3 and 2, with shadow from 1, risking
            // shadowing-out the init on level 3. Level 2 is a placeholder here,
            // just to be a thing-to-merge-level-3-with in the presence of
            // shadowing from 1.
            auto merge32 = LiveBucket::merge(
                bm, cfg.LEDGER_PROTOCOL_VERSION, level3, level2,
                /*shadows=*/{level1},
                /*keepTombstoneEntries=*/true,
                /*countMergeEvents=*/true, clock.getIOContext(),
                /*doFsync=*/true);
            EntryCounts e32(merge32);
            if (initEra)
            {
                // New-style, we preserve the init entry.
                CHECK(e32.nMeta == 1);
                CHECK(e32.nInitOrArchived == 1);
                CHECK(e32.nLive == 0);
                CHECK(e32.nDead == 0);
            }
            else
            {
                // Old-style, we shadowed-out the live and init entries.
                CHECK(e32.nMeta == 0);
                CHECK(e32.nInitOrArchived == 0);
                CHECK(e32.nLive == 0);
                CHECK(e32.nDead == 0);
            }

            // Now do a merge between that 3+2 merge and level 1, and we risk
            // collecting tombstones in the lower levels, which we're expressly
            // trying to _stop_ doing by adding INIT.
            auto merge321 = LiveBucket::merge(
                bm, cfg.LEDGER_PROTOCOL_VERSION, merge32, level1,
                /*shadows=*/{},
                /*keepTombstoneEntries=*/true,
                /*countMergeEvents=*/true, clock.getIOContext(),
                /*doFsync=*/true);
            EntryCounts e321(merge321);
            if (initEra)
            {
                // New-style, init meets dead and they annihilate.
                CHECK(e321.nMeta == 1);
                CHECK(e321.nInitOrArchived == 0);
                CHECK(e321.nLive == 0);
                CHECK(e321.nDead == 0);
            }
            else
            {
                // Old-style, init was already shadowed-out, so dead
                // accumulates.
                CHECK(e321.nMeta == 0);
                CHECK(e321.nInitOrArchived == 0);
                CHECK(e321.nLive == 0);
                CHECK(e321.nDead == 1);
            }
        }
    });
}
