// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketInputIterator.h"
#include "bucket/BucketManager.h"
#include "bucket/BucketOutputIterator.h"
#include "bucket/LiveBucket.h"
#include "bucket/test/BucketTestUtils.h"
#include "catchup/ApplyBucketsWork.h"
#include "ledger/LedgerHashUtils.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "ledger/test/LedgerTestUtils.h"
#include "lib/catch.hpp"
#include "lib/util/stdrandom.h"
#include "main/Application.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "transactions/TransactionUtils.h"
#include "util/Decoder.h"
#include "util/GlobalChecks.h"
#include "util/Math.h"
#include "util/UnorderedSet.h"
#include "util/XDROperators.h"
#include "work/WorkScheduler.h"
#include "xdr/Stellar-ledger-entries.h"
#include <random>
#include <vector>

using namespace stellar;

namespace BucketListIsConsistentWithDatabaseTests
{

struct BucketListGenerator
{
    VirtualClock mClock;
    uint32_t mLedgerSeq;
    Application::pointer mAppGenerate;
    UnorderedSet<LedgerKey> mLiveKeys;

  public:
    BucketListGenerator() : mLedgerSeq(1)
    {
        auto cfg = getTestConfig();
        cfg.OVERRIDE_EVICTION_PARAMS_FOR_TESTING = true;
        cfg.TESTING_STARTING_EVICTION_SCAN_LEVEL = 1;
        mAppGenerate = createTestApplication(mClock, cfg);
        LedgerTxn ltx(mAppGenerate->getLedgerTxnRoot(), false);
        REQUIRE(mLedgerSeq == ltx.loadHeader().current().ledgerSeq);
    }

    template <typename T = ApplyBucketsWork, typename... Args>
    void
    applyBuckets(Application::pointer app, Args&&... args)
    {
        std::map<std::string, std::shared_ptr<LiveBucket>> buckets;
        auto has = getHistoryArchiveState(app);
        auto& wm = app->getWorkScheduler();
        wm.executeWork<T>(buckets, has,
                          app->getConfig().LEDGER_PROTOCOL_VERSION,
                          std::forward<Args>(args)...);
    }

    template <typename T = ApplyBucketsWork, typename... Args>
    void
    applyBuckets(Args&&... args)
    {
        VirtualClock clock;
        Application::pointer app =
            createTestApplication(clock, getTestConfig(1));
        applyBuckets<T, Args...>(app, std::forward<Args>(args)...);
    }

    void
    generateLedger()
    {
        auto& app = mAppGenerate;
        LedgerTxn ltx(app->getLedgerTxnRoot(), false);
        REQUIRE(mLedgerSeq == ltx.loadHeader().current().ledgerSeq);
        mLedgerSeq = ++ltx.loadHeader().current().ledgerSeq;

        auto dead = generateDeadEntries(ltx);
        assert(dead.size() <= mLiveKeys.size());
        for (auto const& key : dead)
        {
            auto iter = mLiveKeys.find(key);
            assert(iter != mLiveKeys.end());
            ltx.erase(key);
            mLiveKeys.erase(iter);
        }

        auto live = generateLiveEntries(ltx);
        for (auto& le : live)
        {
            auto key = LedgerEntryKey(le);
            auto iter = mLiveKeys.find(key);
            if (iter == mLiveKeys.end())
            {
                ltx.create(le);
            }
            else
            {
                ltx.load(key).current() = le;
            }
            mLiveKeys.insert(LedgerEntryKey(le));
        }

        std::vector<LedgerEntry> initEntries, liveEntries;
        std::vector<LedgerKey> deadEntries;
        auto header = ltx.loadHeader().current();
        ltx.getAllEntries(initEntries, liveEntries, deadEntries);
        BucketTestUtils::addLiveBatchAndUpdateSnapshot(
            *app, header, initEntries, liveEntries, deadEntries);
        ltx.commit();
    }

    void
    generateLedgers(uint32_t n)
    {
        uint32_t stopLedger = mLedgerSeq + n - 1;
        while (mLedgerSeq <= stopLedger)
        {
            generateLedger();
        }
    }

    virtual std::vector<LedgerEntry>
    generateLiveEntries(AbstractLedgerTxn& ltx)
    {
        auto entries =
            LedgerTestUtils::generateValidUniqueLedgerEntriesWithTypes({OFFER},
                                                                       5);
        for (auto& le : entries)
        {
            le.lastModifiedLedgerSeq = mLedgerSeq;
        }
        return entries;
    }

    virtual std::vector<LedgerKey>
    generateDeadEntries(AbstractLedgerTxn& ltx)
    {
        UnorderedSet<LedgerKey> liveDeletable = mLiveKeys;
        std::vector<LedgerKey> dead;
        while (dead.size() < 2 && !liveDeletable.empty())
        {
            auto dist = stellar::uniform_int_distribution<size_t>(
                0, liveDeletable.size() - 1);
            auto index = dist(gRandomEngine);
            auto iter = liveDeletable.begin();
            std::advance(iter, index);
            if (iter->type() == CONFIG_SETTING)
            {
                // Configuration can not be deleted.
                continue;
            }

            dead.push_back(*iter);
            liveDeletable.erase(iter);
        }
        return dead;
    }

    HistoryArchiveState
    getHistoryArchiveState(Application::pointer app)
    {
        auto& blGenerate = mAppGenerate->getBucketManager().getLiveBucketList();
        auto& bmApply = app->getBucketManager();
        MergeCounters mergeCounters;
        LedgerTxn ltx(mAppGenerate->getLedgerTxnRoot(), false);
        auto vers = ltx.loadHeader().current().ledgerVersion;
        for (uint32_t i = 0; i <= LiveBucketList::kNumLevels - 1; i++)
        {
            auto& level = blGenerate.getLevel(i);
            auto meta = testutil::testBucketMetadata(vers);
            auto keepDead = LiveBucketList::keepTombstoneEntries(i);

            auto writeBucketFile = [&](auto b) {
                LiveBucketOutputIterator out(bmApply.getTmpDir(), keepDead,
                                             meta, mergeCounters,
                                             mClock.getIOContext(),
                                             /*doFsync=*/true);
                for (LiveBucketInputIterator in(b); in; ++in)
                {
                    out.put(*in);
                }

                auto bucket = out.getBucket(bmApply);
            };
            writeBucketFile(level.getCurr());
            writeBucketFile(level.getSnap());

            auto& next = level.getNext();
            if (next.hasOutputHash())
            {
                auto nextBucket = next.resolve();
                REQUIRE(nextBucket);
                writeBucketFile(nextBucket);
            }
        }
        return HistoryArchiveState(
            mLedgerSeq, blGenerate,
            mAppGenerate->getConfig().NETWORK_PASSPHRASE);
    }
};

bool
doesBucketContain(std::shared_ptr<LiveBucket const> bucket,
                  const BucketEntry& be)
{
    for (LiveBucketInputIterator iter(bucket); iter; ++iter)
    {
        if (*iter == be)
        {
            return true;
        }
    }
    return false;
}

bool
doesBucketListContain(LiveBucketList& bl, const BucketEntry& be)
{
    for (uint32_t i = 0; i < LiveBucketList::kNumLevels; ++i)
    {
        auto const& level = bl.getLevel(i);
        for (auto const& bucket : {level.getCurr(), level.getSnap()})
        {
            if (doesBucketContain(bucket, be))
            {
                return true;
            }
        }
    }
    return false;
}

struct SelectBucketListGenerator : public BucketListGenerator
{
    uint32_t const mSelectLedger;
    std::shared_ptr<LedgerEntry> mSelected;

    SelectBucketListGenerator(uint32_t selectLedger)
        : mSelectLedger(selectLedger)
    {
    }

    virtual std::vector<LedgerEntry>
    generateLiveEntries(AbstractLedgerTxn& ltx)
    {
        if (mLedgerSeq == mSelectLedger)
        {
            if (!mLiveKeys.empty())
            {
                stellar::uniform_int_distribution<size_t> dist(
                    0, mLiveKeys.size() - 1);
                auto iter = mLiveKeys.begin();
                std::advance(iter, dist(gRandomEngine));

                mSelected = std::make_shared<LedgerEntry>(
                    ltx.loadWithoutRecord(*iter).current());
            }
        }

        auto live = BucketListGenerator::generateLiveEntries(ltx);

        // Selected entry must not be shadowed
        if (mSelected)
        {
            auto key = LedgerEntryKey(*mSelected);
            for (size_t i = 0; i < live.size(); ++i)
            {
                if (LedgerEntryKey(live.at(i)) == key)
                {
                    live.erase(live.begin() + i);
                    break;
                }
            }
        }

        return live;
    }

    virtual std::vector<LedgerKey>
    generateDeadEntries(AbstractLedgerTxn& ltx)
    {
        auto dead = BucketListGenerator::generateDeadEntries(ltx);
        if (mSelected)
        {
            auto key = LedgerEntryKey(*mSelected);
            auto iter = std::find(dead.begin(), dead.end(), key);
            if (iter != dead.end())
            {
                dead.erase(iter);
            }
        }
        return dead;
    }
};

class ApplyBucketsWorkAddEntry : public ApplyBucketsWork
{
  private:
    LedgerEntry const mEntry;
    bool mAdded;

  public:
    ApplyBucketsWorkAddEntry(
        Application& app,
        std::map<std::string, std::shared_ptr<LiveBucket>> const& buckets,
        HistoryArchiveState const& applyState, uint32_t maxProtocolVersion,
        LedgerEntry const& entry)
        : ApplyBucketsWork(app, buckets, applyState, maxProtocolVersion)
        , mEntry(entry)
        , mAdded{false}
    {
        REQUIRE(entry.lastModifiedLedgerSeq >= 2);
    }

    BasicWork::State
    doWork() override
    {
        if (!mAdded)
        {
            uint32_t minLedger = mEntry.lastModifiedLedgerSeq;
            uint32_t maxLedger = std::numeric_limits<int32_t>::max() - 1;
            auto& ltxRoot = mApp.getLedgerTxnRoot();

            auto count = ltxRoot.countOffers(
                LedgerRange::inclusive(minLedger, maxLedger));

            if (count > 0)
            {
                LedgerTxn ltx(ltxRoot, false);
                ltx.create(mEntry);
                ltx.commit();
                mAdded = true;
            }
        }
        auto r = ApplyBucketsWork::doWork();
        if (r == State::WORK_SUCCESS)
        {
            REQUIRE(mAdded);
        }
        return r;
    }
};

class ApplyBucketsWorkDeleteEntry : public ApplyBucketsWork
{
  private:
    LedgerKey const mKey;
    LedgerEntry const mEntry;
    bool mDeleted;

  public:
    ApplyBucketsWorkDeleteEntry(
        Application& app,
        std::map<std::string, std::shared_ptr<LiveBucket>> const& buckets,
        HistoryArchiveState const& applyState, uint32_t maxProtocolVersion,
        LedgerEntry const& target)
        : ApplyBucketsWork(app, buckets, applyState, maxProtocolVersion)
        , mKey(LedgerEntryKey(target))
        , mEntry(target)
        , mDeleted{false}
    {
    }

    BasicWork::State
    doWork() override
    {
        if (!mDeleted)
        {
            LedgerTxn ltx(mApp.getLedgerTxnRoot(), false);
            auto entry = ltx.load(mKey);
            if (entry && entry.current() == mEntry)
            {
                entry.erase();
                ltx.commit();
                mDeleted = true;
            }
        }
        auto r = ApplyBucketsWork::doWork();
        if (r == State::WORK_SUCCESS)
        {
            REQUIRE(mDeleted);
        }
        return r;
    }
};

class ApplyBucketsWorkModifyEntry : public ApplyBucketsWork
{
  private:
    LedgerKey const mKey;
    LedgerEntry const mEntry;
    bool mModified;

    void
    modifyOfferEntry(LedgerEntry& entry)
    {
        OfferEntry const& offer = mEntry.data.offer();
        entry.lastModifiedLedgerSeq = mEntry.lastModifiedLedgerSeq;
        entry.data.offer() = LedgerTestUtils::generateValidOfferEntry(5);
        entry.data.offer().sellerID = offer.sellerID;
        entry.data.offer().offerID = offer.offerID;
    }

  public:
    ApplyBucketsWorkModifyEntry(
        Application& app,
        std::map<std::string, std::shared_ptr<LiveBucket>> const& buckets,
        HistoryArchiveState const& applyState, uint32_t maxProtocolVersion,
        LedgerEntry const& target)
        : ApplyBucketsWork(app, buckets, applyState, maxProtocolVersion)
        , mKey(LedgerEntryKey(target))
        , mEntry(target)
        , mModified{false}
    {
    }

    BasicWork::State
    doWork() override
    {
        if (!mModified)
        {
            LedgerTxn ltx(mApp.getLedgerTxnRoot(), false);
            auto entry = ltx.load(mKey);
            while (entry && entry.current() == mEntry)
            {
                releaseAssert(
                    LiveBucketIndex::typeNotSupported(mEntry.data.type()));

                modifyOfferEntry(entry.current());
                mModified = true;
            }

            if (mModified)
            {
                ltx.commit();
            }
        }
        auto r = ApplyBucketsWork::doWork();
        if (r == State::WORK_SUCCESS)
        {
            REQUIRE(mModified);
        }
        return r;
    }
};
}

using namespace BucketListIsConsistentWithDatabaseTests;

TEST_CASE("BucketListIsConsistentWithDatabase succeed",
          "[invariant][bucketlistconsistent]")
{
    BucketListGenerator blg;
    blg.generateLedgers(100);
    REQUIRE_NOTHROW(blg.applyBuckets());
}

TEST_CASE("BucketListIsConsistentWithDatabase empty ledgers",
          "[invariant][bucketlistconsistent]")
{
    class EmptyBucketListGenerator : public BucketListGenerator
    {
        virtual std::vector<LedgerEntry>
        generateLiveEntries(AbstractLedgerTxn& ltx)
        {
            return {};
        }

        virtual std::vector<LedgerKey>
        generateDeadEntries(AbstractLedgerTxn& ltx)
        {
            return {};
        }
    };

    EmptyBucketListGenerator blg;
    blg.generateLedgers(100);
    REQUIRE_NOTHROW(blg.applyBuckets());
}

TEST_CASE("BucketListIsConsistentWithDatabase added entries",
          "[invariant][bucketlistconsistent][acceptance]")
{
    for (size_t nTests = 0; nTests < 40; ++nTests)
    {
        BucketListGenerator blg;
        blg.generateLedgers(100);

        stellar::uniform_int_distribution<uint32_t> addAtLedgerDist(
            2, blg.mLedgerSeq);
        auto le =
            LedgerTestUtils::generateValidLedgerEntryWithTypes({OFFER}, 10);
        le.lastModifiedLedgerSeq = addAtLedgerDist(gRandomEngine);
        REQUIRE_THROWS_AS(blg.applyBuckets<ApplyBucketsWorkAddEntry>(le),
                          InvariantDoesNotHold);
    }
}

TEST_CASE("BucketListIsConsistentWithDatabase deleted entries",
          "[invariant][bucketlistconsistent][acceptance]")
{
    size_t nTests = 0;
    while (nTests < 10)
    {
        SelectBucketListGenerator blg(100);
        blg.generateLedgers(100);
        if (!blg.mSelected)
        {
            continue;
        }

        REQUIRE_THROWS_AS(
            blg.applyBuckets<ApplyBucketsWorkDeleteEntry>(*blg.mSelected),
            InvariantDoesNotHold);
        ++nTests;
    }
}

TEST_CASE("BucketListIsConsistentWithDatabase modified entries",
          "[invariant][bucketlistconsistent][acceptance]")
{
    size_t nTests = 0;
    while (nTests < 10)
    {
        SelectBucketListGenerator blg(100);
        blg.generateLedgers(100);
        if (!blg.mSelected)
        {
            continue;
        }

        REQUIRE_THROWS_AS(
            blg.applyBuckets<ApplyBucketsWorkModifyEntry>(*blg.mSelected),
            InvariantDoesNotHold);
        ++nTests;
    }
}

TEST_CASE("BucketListIsConsistentWithDatabase bucket bounds",
          "[invariant][bucketlistconsistent][acceptance]")
{
    struct LastModifiedBucketListGenerator : public BucketListGenerator
    {
        uint32_t const mTargetLedger;
        uint32_t const mChangeLedgerTo;
        uint32_t mModifiedLedger;

        LastModifiedBucketListGenerator(uint32_t targetLedger,
                                        uint32_t changeLedgerTo)
            : mTargetLedger(targetLedger)
            , mChangeLedgerTo(changeLedgerTo)
            , mModifiedLedger(false)
        {
        }

        virtual std::vector<LedgerEntry>
        generateLiveEntries(AbstractLedgerTxn& ltx)
        {
            auto entries = BucketListGenerator::generateLiveEntries(ltx);
            if (mLedgerSeq == mTargetLedger)
            {
                mModifiedLedger = true;
                for (auto& le : entries)
                {
                    le.lastModifiedLedgerSeq = mChangeLedgerTo;
                }
            }
            return entries;
        }

        virtual std::vector<LedgerKey>
        generateDeadEntries(AbstractLedgerTxn& ltx)
        {
            return {};
        }
    };

    for (uint32_t level = 0; level < LiveBucketList::kNumLevels; ++level)
    {
        uint32_t oldestLedger = LiveBucketList::oldestLedgerInSnap(101, level);
        if (oldestLedger == std::numeric_limits<uint32_t>::max())
        {
            break;
        }
        uint32_t newestLedger = LiveBucketList::oldestLedgerInCurr(101, level) +
                                LiveBucketList::sizeOfCurr(101, level) - 1;
        stellar::uniform_int_distribution<uint32_t> ledgerToModifyDist(
            std::max(2u, oldestLedger), newestLedger);

        for (uint32_t i = 0; i < 10; ++i)
        {
            uint32_t ledgerToModify = ledgerToModifyDist(gRandomEngine);
            uint32_t maxLowTargetLedger = 0;
            uint32_t minHighTargetLedger = 0;
            if (ledgerToModify >=
                LiveBucketList::oldestLedgerInCurr(101, level))
            {
                maxLowTargetLedger =
                    LiveBucketList::oldestLedgerInCurr(101, level) - 1;
                minHighTargetLedger =
                    LiveBucketList::oldestLedgerInCurr(101, level) +
                    LiveBucketList::sizeOfCurr(101, level);
            }
            else
            {
                maxLowTargetLedger =
                    LiveBucketList::oldestLedgerInSnap(101, level) - 1;
                minHighTargetLedger =
                    LiveBucketList::oldestLedgerInCurr(101, level);
            }
            stellar::uniform_int_distribution<uint32_t> lowTargetLedgerDist(
                1, maxLowTargetLedger);
            stellar::uniform_int_distribution<uint32_t> highTargetLedgerDist(
                minHighTargetLedger, std::numeric_limits<int32_t>::max());

            uint32_t lowTarget = lowTargetLedgerDist(gRandomEngine);
            uint32_t highTarget = highTargetLedgerDist(gRandomEngine);
            for (auto target : {lowTarget, highTarget})
            {
                LastModifiedBucketListGenerator blg(ledgerToModify, target);
                blg.generateLedgers(100);
                REQUIRE_THROWS_AS(blg.applyBuckets(), InvariantDoesNotHold);
            }
        }
    }
}

TEST_CASE("BucketListIsConsistentWithDatabase merged LIVEENTRY and DEADENTRY",
          "[invariant][bucketlistconsistent][acceptance]")
{
    struct MergeBucketListGenerator : public SelectBucketListGenerator
    {
        uint32_t const mTargetLedger;

        MergeBucketListGenerator()
            : SelectBucketListGenerator(25), mTargetLedger(110)
        {
        }

        virtual std::vector<LedgerKey>
        generateDeadEntries(AbstractLedgerTxn& ltx)
        {
            if (mLedgerSeq == mTargetLedger)
            {
                return {LedgerEntryKey(*mSelected)};
            }
            else
            {
                return SelectBucketListGenerator::generateDeadEntries(ltx);
            }
        }
    };

    auto exists = [](Application& app, LedgerEntry const& le) {
        LedgerTxn ltx(app.getLedgerTxnRoot());
        return (bool)ltx.load(LedgerEntryKey(le));
    };

    auto cfg = getTestConfig(1);
    cfg.OVERRIDE_EVICTION_PARAMS_FOR_TESTING = true;
    cfg.TESTING_STARTING_EVICTION_SCAN_LEVEL = 1;

    testutil::BucketListDepthModifier<LiveBucket> bldm(3);
    uint32_t nTests = 0;
    while (nTests < 5)
    {
        MergeBucketListGenerator blg;
        auto& blGenerate =
            blg.mAppGenerate->getBucketManager().getLiveBucketList();

        blg.generateLedgers(100);
        if (!blg.mSelected)
        {
            continue;
        }

        BucketEntry dead(DEADENTRY);
        dead.deadEntry() = LedgerEntryKey(*blg.mSelected);
        BucketEntry live(LIVEENTRY);
        live.liveEntry() = *blg.mSelected;
        BucketEntry init(INITENTRY);
        init.liveEntry() = *blg.mSelected;

        {
            VirtualClock clock;
            Application::pointer appApply = createTestApplication(clock, cfg);
            REQUIRE_NOTHROW(blg.applyBuckets(appApply));
            REQUIRE(exists(*blg.mAppGenerate, *blg.mSelected));
            REQUIRE(exists(*appApply, *blg.mSelected));
        }

        blg.generateLedgers(10);
        REQUIRE(doesBucketListContain(blGenerate, dead));
        REQUIRE((doesBucketListContain(blGenerate, live) ||
                 doesBucketListContain(blGenerate, init)));

        blg.generateLedgers(100);
        REQUIRE(!doesBucketListContain(blGenerate, dead));
        REQUIRE(!(doesBucketListContain(blGenerate, live) ||
                  doesBucketListContain(blGenerate, init)));
        REQUIRE(!exists(*blg.mAppGenerate, *blg.mSelected));

        {
            VirtualClock clock;
            Application::pointer appApply = createTestApplication(clock, cfg);
            REQUIRE_NOTHROW(blg.applyBuckets(appApply));
            auto& blApply = appApply->getBucketManager().getLiveBucketList();
            REQUIRE(!doesBucketListContain(blApply, dead));
            REQUIRE(!(doesBucketListContain(blApply, live) ||
                      doesBucketListContain(blApply, init)));
            REQUIRE(!exists(*appApply, *blg.mSelected));
        }

        ++nTests;
    }
}
