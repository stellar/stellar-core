// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"

#include "clf/Bucket.h"
#include "clf/BucketList.h"
#include "clf/CLFMaster.h"
#include "clf/LedgerCmp.h"
#include "crypto/Hex.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/test.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include "util/TmpDir.h"
#include "xdrpp/autocheck.h"
#include <algorithm>
#include <future>

using namespace stellar;



static std::ifstream::pos_type
fileSize(std::string const& name)
{
    assert(TmpDir::exists(name));
    std::ifstream in(name, std::ifstream::ate | std::ifstream::binary);
    return in.tellg();
}

static size_t
countEntries(std::shared_ptr<Bucket> bucket)
{
    auto pair = bucket->countLiveAndDeadEntries();
    return pair.first + pair.second;
}

TEST_CASE("bucket list", "[clf]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    try
    {
        Application app(clock, cfg);
        BucketList bl;
        autocheck::generator<std::vector<LedgerEntry>> liveGen;
        autocheck::generator<std::vector<LedgerKey>> deadGen;
        CLOG(DEBUG, "CLF") << "Adding batches to bucket list";
        for (uint64_t i = 1; !app.getMainIOService().stopped() && i < 130; ++i)
        {
            app.crank(false);
            bl.addBatch(app, i, liveGen(8), deadGen(5));
            if (i % 10 == 0)
                CLOG(DEBUG, "CLF") << "Added batch " << i << ", hash=" << binToHex(bl.getHash());
            for (size_t j = 0; j < bl.numLevels(); ++j)
            {
                auto const& lev = bl.getLevel(j);
                auto currSz = countEntries(lev.getCurr());
                auto snapSz = countEntries(lev.getSnap());
                // CLOG(DEBUG, "CLF") << "level " << j
                //            << " curr=" << currSz
                //            << " snap=" << snapSz;
                CHECK(currSz <= BucketList::levelHalf(j) * 100);
                CHECK(snapSz <= BucketList::levelHalf(j) * 100);
            }
        }
    }
    catch (std::future_error& e)
    {
        CLOG(DEBUG, "CLF") << "Test caught std::future_error " << e.code() << ": "
                   << e.what();
    }
}

TEST_CASE("bucket list shadowing", "[clf]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application app(clock, cfg);
    BucketList bl;

    // Alice and Bob change in every iteration.
    autocheck::generator<AccountEntry> accountGen;
    auto alice = accountGen(5);
    auto bob = accountGen(5);

    autocheck::generator<std::vector<LedgerEntry>> liveGen;
    autocheck::generator<std::vector<LedgerKey>> deadGen;
    CLOG(DEBUG, "CLF") << "Adding batches to bucket list";

    for (uint64_t i = 1; !app.getMainIOService().stopped() && i < 1200; ++i)
    {
        app.crank(false);
        auto liveBatch = liveGen(5);

        CLFEntry CLFAlice, CLFBob;
        alice.sequence++;
        alice.balance++;
        CLFAlice.type(LIVEENTRY);
        CLFAlice.liveEntry().type(ACCOUNT);
        CLFAlice.liveEntry().account() = alice;
        liveBatch.push_back(CLFAlice.liveEntry());

        bob.sequence++;
        bob.balance++;
        CLFBob.type(LIVEENTRY);
        CLFBob.liveEntry().type(ACCOUNT);
        CLFBob.liveEntry().account() = bob;
        liveBatch.push_back(CLFBob.liveEntry());

        bl.addBatch(app, i, liveBatch, deadGen(5));
        if (i % 100 == 0)
        {
            CLOG(DEBUG, "CLF") << "Added batch " << i << ", hash=" << binToHex(bl.getHash());
            {
                // Alice and bob should be in either curr or snap of level 0
                auto curr0 = bl.getLevel(0).getCurr();
                auto snap0 = bl.getLevel(0).getSnap();
                bool hasAlice = (curr0->containsCLFIdentity(CLFAlice) ||
                                 snap0->containsCLFIdentity(CLFAlice));
                bool hasBob = (curr0->containsCLFIdentity(CLFBob) ||
                               snap0->containsCLFIdentity(CLFBob));
                CHECK(hasAlice);
                CHECK(hasBob);
            }

            // Alice and Bob should never occur in level 1 .. N because they
            // were shadowed in level 0 continuously.
            for (size_t j = 1; j < bl.numLevels(); ++j)
            {
                auto const& lev = bl.getLevel(j);
                auto curr = lev.getCurr();
                auto snap = lev.getSnap();
                bool hasAlice = (curr->containsCLFIdentity(CLFAlice) ||
                                 snap->containsCLFIdentity(CLFAlice));
                bool hasBob = (curr->containsCLFIdentity(CLFBob) ||
                               snap->containsCLFIdentity(CLFBob));
                CHECK(!hasAlice);
                CHECK(!hasBob);
            }
        }
    }
}

TEST_CASE("file-backed buckets", "[clf]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application app(clock, cfg);

    autocheck::generator<LedgerEntry> liveGen;
    autocheck::generator<LedgerKey> deadGen;
    CLOG(DEBUG, "CLF") << "Generating 10000 random ledger entries";
    std::vector<LedgerEntry> live(9000);
    std::vector<LedgerKey> dead(1000);
    for (auto &e : live)
        e = liveGen(3);
    for (auto &e : dead)
        e = deadGen(3);
    CLOG(DEBUG, "CLF") << "Hashing entries";
    std::shared_ptr<Bucket> b1 = Bucket::fresh(app.getCLFMaster(), live, dead);
    for (size_t i = 0; i < 5; ++i)
    {
        CLOG(DEBUG, "CLF") << "Merging 10000 new ledger entries into "
                   << (i * 10000) << " entry bucket";
        for (auto &e : live)
            e = liveGen(3);
        for (auto &e : dead)
            e = deadGen(3);
        {
            TIMED_SCOPE(timerObj2, "merge");
            b1 = Bucket::merge(app.getCLFMaster(), b1,
                               Bucket::fresh(app.getCLFMaster(), live, dead));
        }
    }
    CLOG(DEBUG, "CLF") << "Spill file size: " << fileSize(b1->getFilename());
}


TEST_CASE("merging clf entries", "[clf]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application app(clock, cfg);

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
        std::vector<LedgerEntry> live { liveEntry };
        std::vector<LedgerKey> dead { deadEntry };
        std::shared_ptr<Bucket> b1 = Bucket::fresh(app.getCLFMaster(), live, dead);
        CHECK(countEntries(b1) == 1);
    }

    SECTION("dead trustline entry annihilates live trustline entry")
    {
        liveEntry.type(TRUSTLINE);
        liveEntry.trustLine() = tlGen(10);
        deadEntry.type(TRUSTLINE);
        deadEntry.trustLine().accountID = liveEntry.trustLine().accountID;
        deadEntry.trustLine().currency = liveEntry.trustLine().currency;
        std::vector<LedgerEntry> live { liveEntry };
        std::vector<LedgerKey> dead { deadEntry };
        std::shared_ptr<Bucket> b1 = Bucket::fresh(app.getCLFMaster(), live, dead);
        CHECK(countEntries(b1) == 1);
    }

    SECTION("dead offer entry annihilates live offer entry")
    {
        liveEntry.type(OFFER);
        liveEntry.offer() = ofGen(10);
        deadEntry.type(OFFER);
        deadEntry.offer().accountID = liveEntry.offer().accountID;
        deadEntry.offer().sequence = liveEntry.offer().sequence;
        std::vector<LedgerEntry> live { liveEntry };
        std::vector<LedgerKey> dead { deadEntry };
        std::shared_ptr<Bucket> b1 = Bucket::fresh(app.getCLFMaster(), live, dead);
        CHECK(countEntries(b1) == 1);
    }

    SECTION("random dead entries annihilates live entries")
    {
        std::vector<LedgerEntry> live(100);
        std::vector<LedgerKey> dead;
        for (auto &e : live)
        {
            e = leGen(10);
            if (flip())
            {
                dead.push_back(LedgerEntryKey(e));
            }
        }
        std::shared_ptr<Bucket> b1 = Bucket::fresh(app.getCLFMaster(), live, dead);
        CHECK(countEntries(b1) == live.size());
        auto liveCount = b1->countLiveAndDeadEntries().first;
        CLOG(DEBUG, "CLF") << "post-merge live count: " << liveCount << " of " << live.size();
        CHECK(liveCount == live.size() - dead.size());
    }

    SECTION("random live entries overwrite live entries in any order")
    {
        std::vector<LedgerEntry> live(100);
        std::vector<LedgerKey> dead;
        for (auto &e : live)
        {
            e = leGen(10);
        }
        std::shared_ptr<Bucket> b1 = Bucket::fresh(app.getCLFMaster(), live, dead);
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
        std::shared_ptr<Bucket> b2 = Bucket::fresh(app.getCLFMaster(), live, dead);
        std::shared_ptr<Bucket> b3 = Bucket::merge(app.getCLFMaster(), b1, b2);
        CHECK(countEntries(b3) == liveCount);
    }
}


TEST_CASE("clfmaster ownership", "[clf][ownershipclf]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application app(clock, cfg);

    autocheck::generator<LedgerEntry> leGen;
    std::vector<LedgerEntry> live { leGen(10) };
    std::vector<LedgerKey> dead { };

    std::shared_ptr<Bucket> b1;

    {
        std::shared_ptr<Bucket> b2 = Bucket::fresh(app.getCLFMaster(), live, dead);
        b1 = b2;

        // Bucket is referenced by b1, b2 and the CLFMaster.
        CHECK(b1.use_count() == 3);

        std::shared_ptr<Bucket> b3 = Bucket::fresh(app.getCLFMaster(), live, dead);
        std::shared_ptr<Bucket> b4 = Bucket::fresh(app.getCLFMaster(), live, dead);
        // Bucket is referenced by b1, b2, b3, b4 and the CLFMaster.
        CHECK(b1.use_count() == 5);
    }

    // Bucket is now only referenced by b1 and the CLFMaster.
    CHECK(b1.use_count() == 2);

    // Drop CLFMaster's reference, down to just b1.
    app.getCLFMaster().forgetUnreferencedBuckets();
    CHECK(b1.use_count() == 1);

    // Drop it too.
    std::string filename = b1->getFilename();
    CHECK(TmpDir::exists(filename));
    b1.reset();
    CHECK(!TmpDir::exists(filename));

    // Try adding a bucket to the CLFMaster's bucketlist
    auto& bl = app.getCLFMaster().getBucketList();
    bl.addBatch(app, 1, live, dead);
    b1 = bl.getLevel(0).getCurr();

    // Bucket should be referenced by bucketlist itself, CLFMaster cache and b1.
    CHECK(b1.use_count() == 3);

    // This shouldn't change if we forget unreferenced buckets since it's
    // referenced by bucketlist.
    app.getCLFMaster().forgetUnreferencedBuckets();
    CHECK(b1.use_count() == 3);

    // But if we mutate the curr bucket of the bucketlist, it should.
    live[0] = leGen(10);
    bl.addBatch(app, 1, live, dead);
    CHECK(b1.use_count() == 2);
    app.getCLFMaster().forgetUnreferencedBuckets();
    CHECK(b1.use_count() == 1);

    // Drop it again.
    filename = b1->getFilename();
    CHECK(TmpDir::exists(filename));
    b1.reset();
    CHECK(!TmpDir::exists(filename));



}
