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
#include "ledger/LedgerTxn.h"
#include "ledger/test/LedgerTestUtils.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Fs.h"
#include "util/Logging.h"
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
        CLOG(DEBUG, "Bucket") << "Generating 10000 random ledger entries";
        std::vector<LedgerEntry> live(9000);
        std::vector<LedgerKey> dead(1000);
        for (auto& e : live)
            e = LedgerTestUtils::generateValidLedgerEntry(3);
        for (auto& e : dead)
            e = deadGen(3);
        CLOG(DEBUG, "Bucket") << "Hashing entries";
        std::shared_ptr<Bucket> b1 = Bucket::fresh(
            app->getBucketManager(), getAppLedgerVersion(app), {}, live, dead,
            /*countMergeEvents=*/true);
        for (uint32_t i = 0; i < 5; ++i)
        {
            CLOG(DEBUG, "Bucket") << "Merging 10000 new ledger entries into "
                                  << (i * 10000) << " entry bucket";
            for (auto& e : live)
                e = LedgerTestUtils::generateValidLedgerEntry(3);
            for (auto& e : dead)
                e = deadGen(3);
            {
                b1 = Bucket::merge(app->getBucketManager(),
                                   app->getConfig().LEDGER_PROTOCOL_VERSION, b1,
                                   Bucket::fresh(app->getBucketManager(),
                                                 getAppLedgerVersion(app), {},
                                                 live, dead,
                                                 /*countMergeEvents=*/true),
                                   /*shadows=*/{},
                                   /*keepDeadEntries=*/true,
                                   /*countMergeEvents=*/true);
            }
        }
        CLOG(DEBUG, "Bucket")
            << "Spill file size: " << fileSize(b1->getFilename());
    });
}

TEST_CASE("merging bucket entries", "[bucket]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    for_versions_with_differing_bucket_logic(cfg, [&](Config const& cfg) {
        Application::pointer app = createTestApplication(clock, cfg);

        LedgerEntry liveEntry;
        LedgerKey deadEntry;

        autocheck::generator<bool> flip;

        SECTION("dead account entry annihilates live account entry")
        {
            liveEntry.data.type(ACCOUNT);
            liveEntry.data.account() =
                LedgerTestUtils::generateValidAccountEntry(10);
            deadEntry.type(ACCOUNT);
            deadEntry.account().accountID = liveEntry.data.account().accountID;
            std::vector<LedgerEntry> live{liveEntry};
            std::vector<LedgerKey> dead{deadEntry};
            std::shared_ptr<Bucket> b1 =
                Bucket::fresh(app->getBucketManager(), getAppLedgerVersion(app),
                              {}, live, dead, /*countMergeEvents=*/true);
            CHECK(countEntries(b1) == 1);
        }

        SECTION("dead trustline entry annihilates live trustline entry")
        {
            liveEntry.data.type(TRUSTLINE);
            liveEntry.data.trustLine() =
                LedgerTestUtils::generateValidTrustLineEntry(10);
            deadEntry.type(TRUSTLINE);
            deadEntry.trustLine().accountID =
                liveEntry.data.trustLine().accountID;
            deadEntry.trustLine().asset = liveEntry.data.trustLine().asset;
            std::vector<LedgerEntry> live{liveEntry};
            std::vector<LedgerKey> dead{deadEntry};
            std::shared_ptr<Bucket> b1 =
                Bucket::fresh(app->getBucketManager(), getAppLedgerVersion(app),
                              {}, live, dead, /*countMergeEvents=*/true);
            CHECK(countEntries(b1) == 1);
        }

        SECTION("dead offer entry annihilates live offer entry")
        {
            liveEntry.data.type(OFFER);
            liveEntry.data.offer() =
                LedgerTestUtils::generateValidOfferEntry(10);
            deadEntry.type(OFFER);
            deadEntry.offer().sellerID = liveEntry.data.offer().sellerID;
            deadEntry.offer().offerID = liveEntry.data.offer().offerID;
            std::vector<LedgerEntry> live{liveEntry};
            std::vector<LedgerKey> dead{deadEntry};
            std::shared_ptr<Bucket> b1 =
                Bucket::fresh(app->getBucketManager(), getAppLedgerVersion(app),
                              {}, live, dead, /*countMergeEvents=*/true);
            CHECK(countEntries(b1) == 1);
        }

        SECTION("random dead entries annihilates live entries")
        {
            std::vector<LedgerEntry> live(100);
            std::vector<LedgerKey> dead;
            for (auto& e : live)
            {
                e = LedgerTestUtils::generateValidLedgerEntry(10);
                if (flip())
                {
                    dead.push_back(LedgerEntryKey(e));
                }
            }
            std::shared_ptr<Bucket> b1 =
                Bucket::fresh(app->getBucketManager(), getAppLedgerVersion(app),
                              {}, live, dead, /*countMergeEvents=*/true);
            EntryCounts e(b1);
            CHECK(e.sum() == live.size());
            CLOG(DEBUG, "Bucket") << "post-merge live count: " << e.nLive
                                  << " of " << live.size();
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
            std::shared_ptr<Bucket> b1 =
                Bucket::fresh(app->getBucketManager(), getAppLedgerVersion(app),
                              {}, live, dead, /*countMergeEvents=*/true);
            std::random_shuffle(live.begin(), live.end());
            size_t liveCount = live.size();
            for (auto& e : live)
            {
                if (flip())
                {
                    e = LedgerTestUtils::generateValidLedgerEntry(10);
                    ++liveCount;
                }
            }
            std::shared_ptr<Bucket> b2 =
                Bucket::fresh(app->getBucketManager(), getAppLedgerVersion(app),
                              {}, live, dead, /*countMergeEvents=*/true);
            std::shared_ptr<Bucket> b3 =
                Bucket::merge(app->getBucketManager(),
                              app->getConfig().LEDGER_PROTOCOL_VERSION, b1, b2,
                              /*shadows=*/{}, /*keepDeadEntries=*/true,
                              /*countMergeEvents=*/true);
            CHECK(countEntries(b3) == liveCount);
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

        std::shared_ptr<Bucket> birth =
            Bucket::fresh(app->getBucketManager(), getAppLedgerVersion(app), {},
                          live, noDead, /*countMergeEvents=*/true);

        std::shared_ptr<Bucket> death =
            Bucket::fresh(app->getBucketManager(), getAppLedgerVersion(app), {},
                          noLive, dead, /*countMergeEvents=*/true);

        CLOG(INFO, "Bucket")
            << "Applying bucket with " << live.size() << " live entries";
        birth->apply(*app);
        {
            auto count = app->getLedgerTxnRoot().countObjects(ACCOUNT);
            REQUIRE(count == live.size() + 1 /* root account */);
        }

        CLOG(INFO, "Bucket")
            << "Applying bucket with " << dead.size() << " dead entries";
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

        std::shared_ptr<Bucket> birth =
            Bucket::fresh(app->getBucketManager(), getAppLedgerVersion(app), {},
                          live, noDead, /*countMergeEvents=*/true);

        CLOG(INFO, "Bucket")
            << "Applying bucket with " << live.size() << " live entries";
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
