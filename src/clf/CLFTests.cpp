// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"

#include "main/Application.h"
#include "xdrpp/autocheck.h"
#include "clf/CLFMaster.h"
#include "clf/BucketList.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "util/Timer.h"
#include "crypto/Hex.h"
#include <future>
#include <algorithm>

using namespace stellar;

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
        LOG(DEBUG) << "Adding batches to bucket list";
        for (uint64_t i = 1; !app.getMainIOService().stopped() && i < 130; ++i)
        {
            app.crank(false);
            bl.addBatch(app, i, liveGen(8), deadGen(5));
            if (i % 10 == 0)
                LOG(DEBUG) << "Added batch " << i << ", hash=" << binToHex(bl.getHash());
            for (size_t j = 0; j < bl.numLevels(); ++j)
            {
                auto const& lev = bl.getLevel(j);
                auto currSz = lev.getCurr().getEntries().size();
                auto snapSz = lev.getSnap().getEntries().size();
                // LOG(DEBUG) << "level " << j
                //            << " curr=" << currSz
                //            << " snap=" << snapSz;
                CHECK(currSz <= BucketList::levelHalf(j) * 100);
                CHECK(snapSz <= BucketList::levelHalf(j) * 100);
            }
        }
    }
    catch (std::future_error& e)
    {
        LOG(DEBUG) << "Test caught std::future_error " << e.code() << ": "
                   << e.what();
    }
}

static std::ifstream::pos_type
fileSize(std::string const& name)
{
    std::ifstream in(name, std::ifstream::ate | std::ifstream::binary);
    return in.tellg();
}

TEST_CASE("file-backed buckets", "[clf]")
{
    TIMED_FUNC(timerObj);

    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application app(clock, cfg);
    std::string tmpDir = app.getCLFMaster().getTmpDir();

    autocheck::generator<LedgerEntry> liveGen;
    autocheck::generator<LedgerKey> deadGen;
    LOG(DEBUG) << "Generating 10000 random ledger entries";
    std::vector<LedgerEntry> live(9000);
    std::vector<LedgerKey> dead(1000);
    for (auto &e : live)
        e = liveGen(3);
    for (auto &e : dead)
        e = deadGen(3);
    LOG(DEBUG) << "Hashing entries";
    std::shared_ptr<Bucket> b1 = Bucket::fresh(tmpDir, live, dead);
    for (size_t i = 0; i < 5; ++i)
    {
        LOG(DEBUG) << "Merging 10000 new ledger entries into "
                   << (i * 10000) << " entry bucket";
        for (auto &e : live)
            e = liveGen(3);
        for (auto &e : dead)
            e = deadGen(3);
        {
            TIMED_SCOPE(timerObj2, "merge");
            b1 = Bucket::merge(tmpDir, b1,
                               Bucket::fresh(tmpDir, live, dead));
        }
    }
    CHECK(b1->isSpilledToFile());
    LOG(DEBUG) << "Spill file size: " << fileSize(b1->getFilename());
}


TEST_CASE("merging clf entries", "[clf]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application app(clock, cfg);
    std::string tmpDir = app.getCLFMaster().getTmpDir();

    LedgerEntry liveEntry;
    LedgerKey deadEntry;

    autocheck::generator<LedgerEntry> leGen;
    autocheck::generator<AccountEntry> acGen;
    autocheck::generator<TrustLineEntry> tlGen;
    autocheck::generator<OfferEntry> ofGen;
    autocheck::generator<bool> flip;

    SECTION("dead account entry annaihilates live account entry")
    {
        liveEntry.type(ACCOUNT);
        liveEntry.account() = acGen(10);
        deadEntry.type(ACCOUNT);
        deadEntry.account().accountID = liveEntry.account().accountID;
        std::vector<LedgerEntry> live { liveEntry };
        std::vector<LedgerKey> dead { deadEntry };
        std::shared_ptr<Bucket> b1 = Bucket::fresh(tmpDir, live, dead);
        CHECK(b1->getEntries().size() == 1);
    }

    SECTION("dead trustline entry annaihilates live trustline entry")
    {
        liveEntry.type(TRUSTLINE);
        liveEntry.trustLine() = tlGen(10);
        deadEntry.type(TRUSTLINE);
        deadEntry.trustLine().accountID = liveEntry.trustLine().accountID;
        deadEntry.trustLine().currency = liveEntry.trustLine().currency;
        std::vector<LedgerEntry> live { liveEntry };
        std::vector<LedgerKey> dead { deadEntry };
        std::shared_ptr<Bucket> b1 = Bucket::fresh(tmpDir, live, dead);
        CHECK(b1->getEntries().size() == 1);
    }

    SECTION("dead offer entry annaihilates live offer entry")
    {
        liveEntry.type(OFFER);
        liveEntry.offer() = ofGen(10);
        deadEntry.type(OFFER);
        deadEntry.offer().accountID = liveEntry.offer().accountID;
        deadEntry.offer().sequence = liveEntry.offer().sequence;
        std::vector<LedgerEntry> live { liveEntry };
        std::vector<LedgerKey> dead { deadEntry };
        std::shared_ptr<Bucket> b1 = Bucket::fresh(tmpDir, live, dead);
        CHECK(b1->getEntries().size() == 1);
    }

    SECTION("random dead entries annaihilate live entries")
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
        std::shared_ptr<Bucket> b1 = Bucket::fresh(tmpDir, live, dead);
        CHECK(b1->getEntries().size() == live.size());
        size_t liveCount = 0;
        for (auto const& e : b1->getEntries())
        {
            if (e.entry.type() == LIVEENTRY)
            {
                liveCount++;
            }
        }
        LOG(DEBUG) << "post-merge live count: " << liveCount << " of " << live.size();
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
        std::shared_ptr<Bucket> b1 = Bucket::fresh(tmpDir, live, dead);
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
        std::shared_ptr<Bucket> b2 = Bucket::fresh(tmpDir, live, dead);
        std::shared_ptr<Bucket> b3 = Bucket::merge(tmpDir, b1, b2);
        CHECK(b3->getEntries().size() == liveCount);
    }
}
