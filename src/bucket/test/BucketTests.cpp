// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// This file contains tests for individual Buckets, low-level invariants
// concerning the composition of buckets, the semantics of the merge
// operation(s), and the perfomance of merging and applying buckets to the
// database.

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"
#include "bucket/BucketTests.h"
#include "bucket/Bucket.h"
#include "bucket/BucketInputIterator.h"
#include "bucket/BucketManager.h"
#include "bucket/BucketOutputIterator.h"
#include "ledger/LedgerTxn.h"
#include "ledger/test/LedgerTestUtils.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Fs.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "util/Timer.h"
#include "xdrpp/autocheck.h"

using namespace stellar;

namespace BucketTests
{

static std::ifstream::pos_type
fileSize(std::string const& name)
{
    assert(fs::exists(name));
    std::ifstream in(name, std::ifstream::ate | std::ifstream::binary);
    REQUIRE(!!in);
    in.exceptions(std::ios::badbit);
    return in.tellg();
}

uint32_t
getAppLedgerVersion(Application& app)
{
    auto const& lcl = app.getLedgerManager().getLastClosedLedgerHeader();
    return lcl.header.ledgerVersion;
}

uint32_t
getAppLedgerVersion(Application::pointer app)
{
    return getAppLedgerVersion(*app);
}

void
for_versions_with_differing_bucket_logic(
    Config const& cfg, std::function<void(Config const&)> const& f)
{
    for_versions({Bucket::FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY - 1,
                  Bucket::FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY,
                  Bucket::FIRST_PROTOCOL_SHADOWS_REMOVED},
                 cfg, f);
}

void
for_versions_with_differing_initentry_logic(
    Config const& cfg, std::function<void(Config const&)> const& f)
{
    for_versions({Bucket::FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY - 1,
                  Bucket::FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY},
                 cfg, f);
}

EntryCounts::EntryCounts(std::shared_ptr<Bucket> bucket)
{
    BucketInputIterator iter(bucket);
    if (iter.seenMetadata())
    {
        ++nMeta;
    }
    while (iter)
    {
        switch ((*iter).type())
        {
        case INITENTRY:
            ++nInit;
            break;
        case LIVEENTRY:
            ++nLive;
            break;
        case DEADENTRY:
            ++nDead;
            break;
        case METAENTRY:
            // This should never happen: only the first record can be METAENTRY
            // and it is counted above.
            abort();
        }
        ++iter;
    }
}

size_t
countEntries(std::shared_ptr<Bucket> bucket)
{
    EntryCounts e(bucket);
    return e.sum();
}
}

using namespace BucketTests;

TEST_CASE("file backed buckets", "[bucket][bucketbench]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    for_versions_with_differing_bucket_logic(cfg, [&](Config const& cfg) {
        Application::pointer app = createTestApplication(clock, cfg);

        autocheck::generator<LedgerKey> deadGen;
        CLOG_DEBUG(Bucket, "Generating 10000 random ledger entries");
        std::vector<LedgerEntry> live(9000);
        std::vector<LedgerKey> dead(1000);
        for (auto& e : live)
            e = LedgerTestUtils::generateValidLedgerEntry(3);
        for (auto& e : dead)
            e = deadGen(3);
        CLOG_DEBUG(Bucket, "Hashing entries");
        std::shared_ptr<Bucket> b1 = Bucket::fresh(
            app->getBucketManager(), getAppLedgerVersion(app), {}, live, dead,
            /*countMergeEvents=*/true, clock.getIOContext(),
            /*doFsync=*/true);
        for (uint32_t i = 0; i < 5; ++i)
        {
            CLOG_DEBUG(Bucket,
                       "Merging 10000 new ledger entries into {} entry bucket",
                       (i * 10000));
            for (auto& e : live)
                e = LedgerTestUtils::generateValidLedgerEntry(3);
            for (auto& e : dead)
                e = deadGen(3);
            {
                b1 = Bucket::merge(
                    app->getBucketManager(),
                    app->getConfig().LEDGER_PROTOCOL_VERSION, b1,
                    Bucket::fresh(app->getBucketManager(),
                                  getAppLedgerVersion(app), {}, live, dead,
                                  /*countMergeEvents=*/true,
                                  clock.getIOContext(),
                                  /*doFsync=*/true),
                    /*shadows=*/{},
                    /*keepDeadEntries=*/true,
                    /*countMergeEvents=*/true, clock.getIOContext(),
                    /*doFsync=*/true);
            }
        }
        auto sz = static_cast<size_t>(fileSize(b1->getFilename()));
        CLOG_DEBUG(Bucket, "Spill file size: {}", sz);
    });
}

TEST_CASE("merging bucket entries", "[bucket]")
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
                default:
                    abort();
                }
                auto deadEntry = LedgerEntryKey(liveEntry);
                auto bLive = Bucket::fresh(bm, vers, {}, {liveEntry}, {},
                                           /*countMergeEvents=*/true,
                                           clock.getIOContext(),
                                           /*doFsync=*/true);
                auto bDead = Bucket::fresh(bm, vers, {}, {}, {deadEntry},
                                           /*countMergeEvents=*/true,
                                           clock.getIOContext(),
                                           /*doFsync=*/true);
                auto b1 = Bucket::merge(bm, vers, bLive, bDead, /*shadows=*/{},
                                        /*keepDeadEntries=*/true,
                                        /*countMergeEvents=*/true,
                                        clock.getIOContext(),
                                        /*doFsync=*/true);
                CHECK(countEntries(b1) == 1);
            }
        };

        checkDeadAnnihilatesLive(ACCOUNT);
        checkDeadAnnihilatesLive(TRUSTLINE);
        checkDeadAnnihilatesLive(OFFER);
        checkDeadAnnihilatesLive(DATA);
        checkDeadAnnihilatesLive(CLAIMABLE_BALANCE);

        SECTION("random dead entries annihilates live entries")
        {
            std::vector<LedgerEntry> live(100);
            std::vector<LedgerKey> dead;
            for (auto& e : live)
            {
                e = LedgerTestUtils::generateValidLedgerEntry(10);
                if (rand_flip())
                {
                    dead.push_back(LedgerEntryKey(e));
                }
            }
            auto bLive =
                Bucket::fresh(bm, vers, {}, live, {},
                              /*countMergeEvents=*/true, clock.getIOContext(),
                              /*doFsync=*/true);
            auto bDead =
                Bucket::fresh(bm, vers, {}, {}, dead,
                              /*countMergeEvents=*/true, clock.getIOContext(),
                              /*doFsync=*/true);
            auto b1 =
                Bucket::merge(bm, vers, bLive, bDead, /*shadows=*/{},
                              /*keepDeadEntries=*/true,
                              /*countMergeEvents=*/true, clock.getIOContext(),
                              /*doFsync=*/true);
            EntryCounts e(b1);
            CHECK(e.sum() == live.size());
            CLOG_DEBUG(Bucket, "post-merge live count: {} of {}", e.nLive,
                       live.size());
            CHECK(e.nLive == live.size() - dead.size());
        }

        SECTION("random live entries overwrite live entries in any order")
        {
            std::vector<LedgerEntry> live(100);
            std::vector<LedgerKey> dead;
            for (auto& e : live)
            {
                e = LedgerTestUtils::generateValidLedgerEntry(10);
            }
            std::shared_ptr<Bucket> b1 = Bucket::fresh(
                app->getBucketManager(), getAppLedgerVersion(app), {}, live,
                dead, /*countMergeEvents=*/true, clock.getIOContext(),
                /*doFsync=*/true);
            std::random_shuffle(live.begin(), live.end());
            size_t liveCount = live.size();
            for (auto& e : live)
            {
                if (rand_flip())
                {
                    e = LedgerTestUtils::generateValidLedgerEntry(10);
                    ++liveCount;
                }
            }
            std::shared_ptr<Bucket> b2 = Bucket::fresh(
                app->getBucketManager(), getAppLedgerVersion(app), {}, live,
                dead, /*countMergeEvents=*/true, clock.getIOContext(),
                /*doFsync=*/true);
            std::shared_ptr<Bucket> b3 =
                Bucket::merge(app->getBucketManager(),
                              app->getConfig().LEDGER_PROTOCOL_VERSION, b1, b2,
                              /*shadows=*/{}, /*keepDeadEntries=*/true,
                              /*countMergeEvents=*/true, clock.getIOContext(),
                              /*doFsync=*/true);
            CHECK(countEntries(b3) == liveCount);
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
    auto v12 = Bucket::FIRST_PROTOCOL_SHADOWS_REMOVED;
    auto v11 = v12 - 1;
    auto v10 = v11 - 1;

    LedgerEntry liveEntry = generateAccount();
    LedgerEntry otherLiveA = generateDifferentAccount({liveEntry});

    auto b10first =
        Bucket::fresh(bm, v10, {}, {liveEntry}, {},
                      /*countMergeEvents=*/true, clock.getIOContext(),
                      /*doFsync=*/true);
    auto b10second =
        Bucket::fresh(bm, v10, {}, {otherLiveA}, {},
                      /*countMergeEvents=*/true, clock.getIOContext(),
                      /*doFsync=*/true);

    auto b11first =
        Bucket::fresh(bm, v11, {}, {liveEntry}, {},
                      /*countMergeEvents=*/true, clock.getIOContext(),
                      /*doFsync=*/true);
    auto b11second =
        Bucket::fresh(bm, v11, {}, {otherLiveA}, {},
                      /*countMergeEvents=*/true, clock.getIOContext(),
                      /*doFsync=*/true);

    auto b12first =
        Bucket::fresh(bm, v12, {}, {liveEntry}, {}, /*countMergeEvents=*/true,
                      clock.getIOContext(),
                      /*doFsync=*/true);
    auto b12second =
        Bucket::fresh(bm, v12, {}, {otherLiveA}, {},
                      /*countMergeEvents=*/true, clock.getIOContext(),
                      /*doFsync=*/true);

    SECTION("shadow version 12")
    {
        // With proto 12, new bucket version solely depends on the snap version
        auto bucket =
            Bucket::merge(bm, v12, b11first, b11second,
                          /*shadows=*/{b12first},
                          /*keepDeadEntries=*/true,
                          /*countMergeEvents=*/true, clock.getIOContext(),
                          /*doFsync=*/true);
        REQUIRE(Bucket::getBucketVersion(bucket) == v11);
    }
    SECTION("shadow versions mixed, pick lower")
    {
        // Merging older version (10) buckets, with mixed versions of shadows
        // (11, 12) Pick initentry (11) style merge
        auto bucket =
            Bucket::merge(bm, v12, b10first, b10second,
                          /*shadows=*/{b12first, b11second},
                          /*keepDeadEntries=*/true,
                          /*countMergeEvents=*/true, clock.getIOContext(),
                          /*doFsync=*/true);
        REQUIRE(Bucket::getBucketVersion(bucket) == v11);
    }
    SECTION("refuse to merge new version with shadow")
    {
        REQUIRE_THROWS_AS(Bucket::merge(bm, v12, b12first, b12second,
                                        /*shadows=*/{b12first},
                                        /*keepDeadEntries=*/true,
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
    auto bold1 = Bucket::fresh(bm, vers - 1, {}, {liveEntry}, {},
                               /*countMergeEvents=*/true, clock.getIOContext(),
                               /*doFsync=*/true);
    auto bold2 = Bucket::fresh(bm, vers - 1, {}, {otherLiveA}, {},
                               /*countMergeEvents=*/true, clock.getIOContext(),
                               /*doFsync=*/true);
    auto bnew1 = Bucket::fresh(bm, vers, {}, {liveEntry}, {},
                               /*countMergeEvents=*/true, clock.getIOContext(),
                               /*doFsync=*/true);
    auto bnew2 = Bucket::fresh(bm, vers, {}, {otherLiveA}, {},
                               /*countMergeEvents=*/true, clock.getIOContext(),
                               /*doFsync=*/true);
    REQUIRE_THROWS_AS(Bucket::merge(bm, vers - 1, bnew1, bnew2,
                                    /*shadows=*/{},
                                    /*keepDeadEntries=*/true,
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
    auto vers_new = Bucket::FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY;
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
    BucketOutputIterator out(bm.getTmpDir(), true, meta, mc,
                             clock.getIOContext(), /*doFsync=*/true);
    REQUIRE_THROWS_AS(out.put(initEntry), std::runtime_error);
    REQUIRE_THROWS_AS(out.put(metaEntry), std::runtime_error);
}

TEST_CASE("merging bucket entries with initentry", "[bucket][initentry]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    for_versions_with_differing_bucket_logic(cfg, [&](Config const& cfg) {
        CLOG_INFO(Bucket, "=== starting test app == ");
        Application::pointer app = createTestApplication(clock, cfg);
        auto& bm = app->getBucketManager();
        auto vers = getAppLedgerVersion(app);

        // Whether we're in the era of supporting or not-supporting INITENTRY.
        bool initEra =
            (vers >= Bucket::FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY);

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
            auto bInit =
                Bucket::fresh(bm, vers, {initEntry}, {}, {},
                              /*countMergeEvents=*/true, clock.getIOContext(),
                              /*doFsync=*/true);
            auto bDead =
                Bucket::fresh(bm, vers, {}, {}, {deadEntry},
                              /*countMergeEvents=*/true, clock.getIOContext(),
                              /*doFsync=*/true);
            auto b1 = Bucket::merge(
                bm, cfg.LEDGER_PROTOCOL_VERSION, bInit, bDead, /*shadows=*/{},
                /*keepDeadEntries=*/true,
                /*countMergeEvents=*/true, clock.getIOContext(),
                /*doFsync=*/true);
            // In initEra, the INIT will make it through fresh() to the bucket,
            // and mutually annihilate on contact with the DEAD, leaving 0
            // entries. Pre-initEra, the INIT will downgrade to a LIVE during
            // fresh(), and that will be killed by the DEAD, leaving 1
            // (tombstone) entry.
            EntryCounts e(b1);
            CHECK(e.nInit == 0);
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
            auto bInit =
                Bucket::fresh(bm, vers, {initEntry}, {}, {},
                              /*countMergeEvents=*/true, clock.getIOContext(),
                              /*doFsync=*/true);
            auto bLive =
                Bucket::fresh(bm, vers, {}, {liveEntry}, {},
                              /*countMergeEvents=*/true, clock.getIOContext(),
                              /*doFsync=*/true);
            auto bDead =
                Bucket::fresh(bm, vers, {}, {}, {deadEntry},
                              /*countMergeEvents=*/true, clock.getIOContext(),
                              /*doFsync=*/true);
            auto bmerge1 = Bucket::merge(
                bm, cfg.LEDGER_PROTOCOL_VERSION, bInit, bLive, /*shadows=*/{},
                /*keepDeadEntries=*/true,
                /*countMergeEvents=*/true, clock.getIOContext(),
                /*doFsync=*/true);
            auto b1 = Bucket::merge(
                bm, cfg.LEDGER_PROTOCOL_VERSION, bmerge1, bDead, /*shadows=*/{},
                /*keepDeadEntries=*/true,
                /*countMergeEvents=*/true, clock.getIOContext(),
                /*doFsync=*/true);
            // The same thing should happen here as above, except that the INIT
            // will merge-over the LIVE during fresh().
            EntryCounts e(b1);
            CHECK(e.nInit == 0);
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
            auto bold =
                Bucket::fresh(bm, vers, {initEntry}, {}, {},
                              /*countMergeEvents=*/true, clock.getIOContext(),
                              /*doFsync=*/true);
            auto bmed = Bucket::fresh(
                bm, vers, {}, {otherLiveA, otherLiveB, liveEntry, otherLiveC},
                {}, /*countMergeEvents=*/true, clock.getIOContext(),
                /*doFsync=*/true);
            auto bnew =
                Bucket::fresh(bm, vers, {}, {}, {deadEntry},
                              /*countMergeEvents=*/true, clock.getIOContext(),
                              /*doFsync=*/true);
            EntryCounts eold(bold), emed(bmed), enew(bnew);
            if (initEra)
            {
                CHECK(eold.nMeta == 1);
                CHECK(emed.nMeta == 1);
                CHECK(enew.nMeta == 1);
                CHECK(eold.nInit == 1);
                CHECK(eold.nLive == 0);
            }
            else
            {
                CHECK(eold.nMeta == 0);
                CHECK(emed.nMeta == 0);
                CHECK(enew.nMeta == 0);
                CHECK(eold.nInit == 0);
                CHECK(eold.nLive == 1);
            }

            CHECK(eold.nDead == 0);

            CHECK(emed.nInit == 0);
            CHECK(emed.nLive == 4);
            CHECK(emed.nDead == 0);

            CHECK(enew.nInit == 0);
            CHECK(enew.nLive == 0);
            CHECK(enew.nDead == 1);

            auto bmerge1 = Bucket::merge(
                bm, cfg.LEDGER_PROTOCOL_VERSION, bold, bmed, /*shadows=*/{},
                /*keepDeadEntries=*/true,
                /*countMergeEvents=*/true, clock.getIOContext(),
                /*doFsync=*/true);
            auto bmerge2 = Bucket::merge(
                bm, cfg.LEDGER_PROTOCOL_VERSION, bmerge1, bnew, /*shadows=*/{},
                /*keepDeadEntries=*/true,
                /*countMergeEvents=*/true, clock.getIOContext(),
                /*doFsync=*/true);
            EntryCounts emerge1(bmerge1), emerge2(bmerge2);
            if (initEra)
            {
                CHECK(emerge1.nMeta == 1);
                CHECK(emerge1.nInit == 1);
                CHECK(emerge1.nLive == 3);

                CHECK(emerge2.nMeta == 1);
                CHECK(emerge2.nDead == 0);
            }
            else
            {
                CHECK(emerge1.nMeta == 0);
                CHECK(emerge1.nInit == 0);
                CHECK(emerge1.nLive == 4);

                CHECK(emerge2.nMeta == 0);
                CHECK(emerge2.nDead == 1);
            }
            CHECK(emerge1.nDead == 0);
            CHECK(emerge2.nInit == 0);
            CHECK(emerge2.nLive == 3);
        }
    });
}

TEST_CASE("merging bucket entries with initentry with shadows",
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
        bool initEra =
            (vers >= Bucket::FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY);

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
            auto shadow =
                Bucket::fresh(bm, vers, {}, {liveEntry}, {},
                              /*countMergeEvents=*/true, clock.getIOContext(),
                              /*doFsync=*/true);
            auto b1 =
                Bucket::fresh(bm, vers, {initEntry}, {}, {},
                              /*countMergeEvents=*/true, clock.getIOContext(),
                              /*doFsync=*/true);
            auto b2 =
                Bucket::fresh(bm, vers, {otherInitA}, {}, {},
                              /*countMergeEvents=*/true, clock.getIOContext(),
                              /*doFsync=*/true);
            auto merged =
                Bucket::merge(bm, cfg.LEDGER_PROTOCOL_VERSION, b1, b2,
                              /*shadows=*/{shadow},
                              /*keepDeadEntries=*/true,
                              /*countMergeEvents=*/true, clock.getIOContext(),
                              /*doFsync=*/true);
            EntryCounts e(merged);
            if (initEra)
            {
                CHECK(e.nMeta == 1);
                CHECK(e.nInit == 2);
                CHECK(e.nLive == 0);
                CHECK(e.nDead == 0);
            }
            else
            {
                CHECK(e.nMeta == 0);
                CHECK(e.nInit == 0);
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
            auto level1 =
                Bucket::fresh(bm, vers, {}, {}, {deadEntry},
                              /*countMergeEvents=*/true, clock.getIOContext(),
                              /*doFsync=*/true);
            auto level2 =
                Bucket::fresh(bm, vers, {initEntry2}, {}, {},
                              /*countMergeEvents=*/true, clock.getIOContext(),
                              /*doFsync=*/true);
            auto level3 =
                Bucket::fresh(bm, vers, {}, {}, {deadEntry},
                              /*countMergeEvents=*/true, clock.getIOContext(),
                              /*doFsync=*/true);
            auto level4 =
                Bucket::fresh(bm, vers, {}, {}, {}, /*countMergeEvents=*/true,
                              clock.getIOContext(),
                              /*doFsync=*/true);
            auto level5 =
                Bucket::fresh(bm, vers, {initEntry}, {}, {},
                              /*countMergeEvents=*/true, clock.getIOContext(),
                              /*doFsync=*/true);

            // Do a merge between levels 4 and 3, with shadows from 2 and 1,
            // risking shadowing-out level 3. Level 4 is a placeholder here,
            // just to be a thing-to-merge-level-3-with in the presence of
            // shadowing from 1 and 2.
            auto merge43 =
                Bucket::merge(bm, cfg.LEDGER_PROTOCOL_VERSION, level4, level3,
                              /*shadows=*/{level2, level1},
                              /*keepDeadEntries=*/true,
                              /*countMergeEvents=*/true, clock.getIOContext(),
                              /*doFsync=*/true);
            EntryCounts e43(merge43);
            if (initEra)
            {
                // New-style, we preserve the dead entry.
                CHECK(e43.nMeta == 1);
                CHECK(e43.nInit == 0);
                CHECK(e43.nLive == 0);
                CHECK(e43.nDead == 1);
            }
            else
            {
                // Old-style, we shadowed-out the dead entry.
                CHECK(e43.nMeta == 0);
                CHECK(e43.nInit == 0);
                CHECK(e43.nLive == 0);
                CHECK(e43.nDead == 0);
            }

            // Do a merge between level 2 and 1, producing potentially
            // an annihilation of their INIT and DEAD pair.
            auto merge21 =
                Bucket::merge(bm, cfg.LEDGER_PROTOCOL_VERSION, level2, level1,
                              /*shadows=*/{},
                              /*keepDeadEntries=*/true,
                              /*countMergeEvents=*/true, clock.getIOContext(),
                              /*doFsync=*/true);
            EntryCounts e21(merge21);
            if (initEra)
            {
                // New-style, they mutually annihilate.
                CHECK(e21.nMeta == 1);
                CHECK(e21.nInit == 0);
                CHECK(e21.nLive == 0);
                CHECK(e21.nDead == 0);
            }
            else
            {
                // Old-style, we keep the tombstone around.
                CHECK(e21.nMeta == 0);
                CHECK(e21.nInit == 0);
                CHECK(e21.nLive == 0);
                CHECK(e21.nDead == 1);
            }

            // Do two more merges: one between the two merges we've
            // done so far, and then finally one with level 5.
            auto merge4321 =
                Bucket::merge(bm, cfg.LEDGER_PROTOCOL_VERSION, merge43, merge21,
                              /*shadows=*/{},
                              /*keepDeadEntries=*/true,
                              /*countMergeEvents=*/true, clock.getIOContext(),
                              /*doFsync=*/true);
            auto merge54321 = Bucket::merge(
                bm, cfg.LEDGER_PROTOCOL_VERSION, level5, merge4321,
                /*shadows=*/{},
                /*keepDeadEntries=*/true,
                /*countMergeEvents=*/true, clock.getIOContext(),
                /*doFsync=*/true);
            EntryCounts e54321(merge21);
            if (initEra)
            {
                // New-style, we should get a second mutual annihilation.
                CHECK(e54321.nMeta == 1);
                CHECK(e54321.nInit == 0);
                CHECK(e54321.nLive == 0);
                CHECK(e54321.nDead == 0);
            }
            else
            {
                // Old-style, the tombstone should clobber the live entry.
                CHECK(e54321.nMeta == 0);
                CHECK(e54321.nInit == 0);
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
            auto level1 =
                Bucket::fresh(bm, vers, {}, {}, {deadEntry},
                              /*countMergeEvents=*/true, clock.getIOContext(),
                              /*doFsync=*/true);
            auto level2 =
                Bucket::fresh(bm, vers, {}, {liveEntry}, {},
                              /*countMergeEvents=*/true, clock.getIOContext(),
                              /*doFsync=*/true);
            auto level3 =
                Bucket::fresh(bm, vers, {initEntry}, {}, {},
                              /*countMergeEvents=*/true, clock.getIOContext(),
                              /*doFsync=*/true);

            // Do a merge between levels 3 and 2, with shadow from 1, risking
            // shadowing-out the init on level 3. Level 2 is a placeholder here,
            // just to be a thing-to-merge-level-3-with in the presence of
            // shadowing from 1.
            auto merge32 =
                Bucket::merge(bm, cfg.LEDGER_PROTOCOL_VERSION, level3, level2,
                              /*shadows=*/{level1},
                              /*keepDeadEntries=*/true,
                              /*countMergeEvents=*/true, clock.getIOContext(),
                              /*doFsync=*/true);
            EntryCounts e32(merge32);
            if (initEra)
            {
                // New-style, we preserve the init entry.
                CHECK(e32.nMeta == 1);
                CHECK(e32.nInit == 1);
                CHECK(e32.nLive == 0);
                CHECK(e32.nDead == 0);
            }
            else
            {
                // Old-style, we shadowed-out the live and init entries.
                CHECK(e32.nMeta == 0);
                CHECK(e32.nInit == 0);
                CHECK(e32.nLive == 0);
                CHECK(e32.nDead == 0);
            }

            // Now do a merge between that 3+2 merge and level 1, and we risk
            // collecting tombstones in the lower levels, which we're expressly
            // trying to _stop_ doing by adding INIT.
            auto merge321 =
                Bucket::merge(bm, cfg.LEDGER_PROTOCOL_VERSION, merge32, level1,
                              /*shadows=*/{},
                              /*keepDeadEntries=*/true,
                              /*countMergeEvents=*/true, clock.getIOContext(),
                              /*doFsync=*/true);
            EntryCounts e321(merge321);
            if (initEra)
            {
                // New-style, init meets dead and they annihilate.
                CHECK(e321.nMeta == 1);
                CHECK(e321.nInit == 0);
                CHECK(e321.nLive == 0);
                CHECK(e321.nDead == 0);
            }
            else
            {
                // Old-style, init was already shadowed-out, so dead
                // accumulates.
                CHECK(e321.nMeta == 0);
                CHECK(e321.nInit == 0);
                CHECK(e321.nLive == 0);
                CHECK(e321.nDead == 1);
            }
        }
    });
}

TEST_CASE("bucket apply", "[bucket]")
{
    VirtualClock clock;
    Config cfg(getTestConfig());
    for_versions_with_differing_bucket_logic(cfg, [&](Config const& cfg) {
        Application::pointer app = createTestApplication(clock, cfg);
        app->start();

        std::vector<LedgerEntry> live(10), noLive;
        std::vector<LedgerKey> dead, noDead;

        for (auto& e : live)
        {
            e.data.type(ACCOUNT);
            auto& a = e.data.account();
            a = LedgerTestUtils::generateValidAccountEntry(5);
            a.balance = 1000000000;
            dead.emplace_back(LedgerEntryKey(e));
        }

        std::shared_ptr<Bucket> birth = Bucket::fresh(
            app->getBucketManager(), getAppLedgerVersion(app), {}, live, noDead,
            /*countMergeEvents=*/true, clock.getIOContext(),
            /*doFsync=*/true);

        std::shared_ptr<Bucket> death = Bucket::fresh(
            app->getBucketManager(), getAppLedgerVersion(app), {}, noLive, dead,
            /*countMergeEvents=*/true, clock.getIOContext(),
            /*doFsync=*/true);

        CLOG_INFO(Bucket, "Applying bucket with {} live entries", live.size());
        birth->apply(*app);
        {
            auto count = app->getLedgerTxnRoot().countObjects(ACCOUNT);
            REQUIRE(count == live.size() + 1 /* root account */);
        }

        CLOG_INFO(Bucket, "Applying bucket with {} dead entries", dead.size());
        death->apply(*app);
        {
            auto count = app->getLedgerTxnRoot().countObjects(ACCOUNT);
            REQUIRE(count == 1 /* root account */);
        }
    });
}

TEST_CASE("bucket apply bench", "[bucketbench][!hide]")
{
    auto runtest = [](Config::TestDbMode mode) {
        VirtualClock clock;
        Config cfg(getTestConfig(0, mode));
        Application::pointer app = createTestApplication(clock, cfg);
        app->start();

        std::vector<LedgerEntry> live(100000);
        std::vector<LedgerKey> noDead;

        for (auto& l : live)
        {
            l.data.type(ACCOUNT);
            auto& a = l.data.account();
            a = LedgerTestUtils::generateValidAccountEntry(5);
        }

        std::shared_ptr<Bucket> birth = Bucket::fresh(
            app->getBucketManager(), getAppLedgerVersion(app), {}, live, noDead,
            /*countMergeEvents=*/true, clock.getIOContext(),
            /*doFsync=*/true);

        CLOG_INFO(Bucket, "Applying bucket with {} live entries", live.size());
        // note: we do not wrap the `apply` call inside a transaction
        // as bucket applicator commits to the database incrementally
        birth->apply(*app);
    };

    SECTION("sqlite")
    {
        runtest(Config::TESTDB_ON_DISK_SQLITE);
    }
#ifdef USE_POSTGRES
    SECTION("postgresql")
    {
        runtest(Config::TESTDB_POSTGRESQL);
    }
#endif
}
