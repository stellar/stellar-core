// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketInputIterator.h"
#include "bucket/BucketManager.h"
#include "bucket/BucketOutputIterator.h"
#include "catchup/ApplyBucketsWork.h"
#include "database/Database.h"
#include "ledger/AccountFrame.h"
#include "ledger/DataFrame.h"
#include "ledger/EntryFrame.h"
#include "ledger/LedgerDelta.h"
#include "ledger/LedgerTestUtils.h"
#include "ledger/OfferFrame.h"
#include "ledger/TrustFrame.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "work/WorkManager.h"
#include <random>
#include <set>
#include <vector>

using namespace stellar;

struct BucketListGenerator
{
    VirtualClock mClock;
    std::shared_ptr<std::default_random_engine> mGen;
    Application::pointer mAppGenerate;
    Application::pointer mAppApply;
    uint32_t mLedgerSeq;
    std::set<LedgerKey> mLiveKeys;

  public:
    BucketListGenerator(std::shared_ptr<std::default_random_engine> const& gen)
        : mGen(gen)
        , mAppGenerate(createTestApplication(mClock, getTestConfig(0)))
        , mAppApply(createTestApplication(mClock, getTestConfig(1)))
        , mLedgerSeq(2)
    {
    }

    BucketListGenerator()
        : BucketListGenerator(std::make_shared<std::default_random_engine>())
    {
    }

    template <typename T = ApplyBucketsWork, typename... Args>
    void
    applyBuckets(Args&&... args)
    {
        std::map<std::string, std::shared_ptr<Bucket>> buckets;
        auto has = getHistoryArchiveState();
        auto& wm = mAppApply->getWorkManager();
        wm.executeWork<T>(buckets, has, std::forward<Args>(args)...);
    }

    void
    generateLedger()
    {
        auto& app = mAppGenerate;
        ++mLedgerSeq;

        LedgerHeader lh;
        LedgerDelta ld(lh, app->getDatabase(), false);

        auto dead = generateDeadEntries();
        assert(dead.size() <= mLiveKeys.size());
        for (auto const& key : dead)
        {
            auto iter = mLiveKeys.find(key);
            assert(iter != mLiveKeys.end());
            EntryFrame::storeDelete(ld, app->getDatabase(), key);
            mLiveKeys.erase(iter);
        }

        auto live = generateLiveEntries();
        for (auto& ef : live)
        {
            ef->storeAddOrChange(ld, app->getDatabase());
            mLiveKeys.insert(LedgerEntryKey(ef->mEntry));
        }

        app->getBucketManager().addBatch(*app, mLedgerSeq, ld.getLiveEntries(),
                                         ld.getDeadEntries());
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

    virtual std::vector<EntryFrame::pointer>
    generateLiveEntries()
    {
        auto entries = LedgerTestUtils::generateValidLedgerEntries(5);
        std::vector<EntryFrame::pointer> frames;
        std::transform(entries.begin(), entries.end(),
                       std::back_inserter(frames), [this](LedgerEntry& le) {
                           le.lastModifiedLedgerSeq = mLedgerSeq;
                           return EntryFrame::FromXDR(le);
                       });
        return frames;
    }

    virtual std::vector<LedgerKey>
    generateDeadEntries()
    {
        std::set<LedgerKey> live(mLiveKeys);
        std::vector<LedgerKey> dead;
        while (dead.size() < 2 && !live.empty())
        {
            auto dist =
                std::uniform_int_distribution<size_t>(0, live.size() - 1);
            auto index = dist(*mGen);
            auto iter = live.begin();
            std::advance(iter, index);
            dead.push_back(*iter);
            live.erase(iter);
        }
        return dead;
    }

    HistoryArchiveState
    getHistoryArchiveState() const
    {
        auto& blGenerate = mAppGenerate->getBucketManager().getBucketList();
        auto& bmApply = mAppApply->getBucketManager();

        for (uint32_t i = 0; i <= BucketList::kNumLevels - 1; i++)
        {
            auto& level = blGenerate.getLevel(i);
            {
                BucketOutputIterator out(bmApply.getTmpDir(), true);
                for (BucketInputIterator in (level.getCurr()); in; ++in)
                {
                    out.put(*in);
                }
                out.getBucket(bmApply);
            }
            {
                BucketOutputIterator out(bmApply.getTmpDir(), true);
                for (BucketInputIterator in (level.getSnap()); in; ++in)
                {
                    out.put(*in);
                }
                out.getBucket(bmApply);
            }
        }
        return HistoryArchiveState(mLedgerSeq, blGenerate);
    }
};

bool
doesBucketContain(std::shared_ptr<Bucket const> bucket, const BucketEntry& be)
{
    for (BucketInputIterator iter(bucket); iter; ++iter)
    {
        if (*iter == be)
        {
            return true;
        }
    }
    return false;
}

bool
doesBucketListContain(BucketList& bl, const BucketEntry& be)
{
    for (uint32_t i = 0; i < BucketList::kNumLevels; ++i)
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
    LedgerEntryType const mType;
    std::shared_ptr<LedgerEntry> mSelected;

    SelectBucketListGenerator(uint32_t selectLedger, LedgerEntryType type)
        : mSelectLedger(selectLedger), mType(type)
    {
    }

    virtual std::vector<EntryFrame::pointer>
    generateLiveEntries()
    {
        if (mLedgerSeq == mSelectLedger)
        {
            std::set<LedgerKey> filteredKeys;
            std::copy_if(
                mLiveKeys.begin(), mLiveKeys.end(),
                std::inserter(filteredKeys, filteredKeys.end()),
                [this](LedgerKey const& key) { return key.type() == mType; });

            if (!filteredKeys.empty())
            {
                std::uniform_int_distribution<uint32_t> dist(
                    0, filteredKeys.size() - 1);
                auto iter = filteredKeys.begin();
                std::advance(iter, dist(*mGen));

                auto& db = mAppGenerate->getDatabase();
                mSelected = std::make_shared<LedgerEntry>(
                    EntryFrame::storeLoad(*iter, db)->mEntry);
            }
        }
        return BucketListGenerator::generateLiveEntries();
    }

    virtual std::vector<LedgerKey>
    generateDeadEntries()
    {
        auto dead = BucketListGenerator::generateDeadEntries();
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
    LedgerEntry mEntry;
    bool mAdded;

  public:
    ApplyBucketsWorkAddEntry(
        Application& app, WorkParent& parent,
        std::map<std::string, std::shared_ptr<Bucket>> const& buckets,
        HistoryArchiveState const& applyState, LedgerEntry const& entry)
        : ApplyBucketsWork(app, parent, buckets, applyState)
        , mEntry(entry)
        , mAdded{false}
    {
        REQUIRE(entry.lastModifiedLedgerSeq >= 2);
    }

    Work::State
    onSuccess() override
    {
        if (!mAdded)
        {
            auto& db = mApp.getDatabase();
            auto& sess = db.getSession();
            uint32_t minLedger = mEntry.lastModifiedLedgerSeq;
            uint32_t maxLedger = std::numeric_limits<int32_t>::max();
            size_t count =
                AccountFrame::countObjects(sess, {minLedger, maxLedger}) +
                TrustFrame::countObjects(sess, {minLedger, maxLedger}) +
                OfferFrame::countObjects(sess, {minLedger, maxLedger}) +
                DataFrame::countObjects(sess, {minLedger, maxLedger});

            if (count > 0)
            {
                LedgerHeader lh;
                LedgerDelta ld(lh, db, false);
                EntryFrame::FromXDR(mEntry)->storeAdd(ld, db);
                mAdded = true;
            }
        }
        auto r = ApplyBucketsWork::onSuccess();
        if (r == WORK_SUCCESS)
        {
            REQUIRE(mAdded);
        }
        return r;
    }
};

class ApplyBucketsWorkDeleteEntry : public ApplyBucketsWork
{
  private:
    LedgerKey mKey;
    LedgerEntry mEntry;
    bool mDeleted;

  public:
    ApplyBucketsWorkDeleteEntry(
        Application& app, WorkParent& parent,
        std::map<std::string, std::shared_ptr<Bucket>> const& buckets,
        HistoryArchiveState const& applyState, LedgerEntry const& target)
        : ApplyBucketsWork(app, parent, buckets, applyState)
        , mKey(LedgerEntryKey(target))
        , mEntry(target)
        , mDeleted{false}
    {
    }

    Work::State
    onSuccess() override
    {
        if (!mDeleted)
        {
            auto& db = mApp.getDatabase();
            auto ef = EntryFrame::storeLoad(mKey, db);
            if (ef && ef->mEntry == mEntry)
            {
                LedgerHeader lh;
                LedgerDelta ld(lh, db, false);
                ef->storeDelete(ld, db);
                mDeleted = true;
            }
        }
        auto r = ApplyBucketsWork::onSuccess();
        if (r == WORK_SUCCESS)
        {
            REQUIRE(mDeleted);
        }
        return r;
    }
};

class ApplyBucketsWorkModifyEntry : public ApplyBucketsWork
{
  private:
    LedgerKey mKey;
    LedgerEntry mEntry;
    bool mModified;

    void
    modifyAccountEntry(LedgerEntry& entry)
    {
        AccountEntry const& account = mEntry.data.account();
        entry.lastModifiedLedgerSeq = mEntry.lastModifiedLedgerSeq;
        entry.data.account() = LedgerTestUtils::generateValidAccountEntry(5);
        entry.data.account().accountID = account.accountID;
    }

    void
    modifyTrustLineEntry(LedgerEntry& entry)
    {
        TrustLineEntry const& trustLine = mEntry.data.trustLine();
        entry.lastModifiedLedgerSeq = mEntry.lastModifiedLedgerSeq;
        entry.data.trustLine() =
            LedgerTestUtils::generateValidTrustLineEntry(5);
        entry.data.trustLine().accountID = trustLine.accountID;
        entry.data.trustLine().asset = trustLine.asset;
    }

    void
    modifyOfferEntry(LedgerEntry& entry)
    {
        OfferEntry const& offer = mEntry.data.offer();
        entry.lastModifiedLedgerSeq = mEntry.lastModifiedLedgerSeq;
        entry.data.offer() = LedgerTestUtils::generateValidOfferEntry(5);
        entry.data.offer().sellerID = offer.sellerID;
        entry.data.offer().offerID = offer.offerID;
    }

    void
    modifyDataEntry(LedgerEntry& entry)
    {
        DataEntry const& data = mEntry.data.data();
        entry.lastModifiedLedgerSeq = mEntry.lastModifiedLedgerSeq;
        do
        {
            entry.data.data() = LedgerTestUtils::generateValidDataEntry(5);
        } while (entry.data.data().dataValue == data.dataValue);
        entry.data.data().accountID = data.accountID;
        entry.data.data().dataName = data.dataName;
    }

  public:
    ApplyBucketsWorkModifyEntry(
        Application& app, WorkParent& parent,
        std::map<std::string, std::shared_ptr<Bucket>> const& buckets,
        HistoryArchiveState const& applyState, LedgerEntry const& target)
        : ApplyBucketsWork(app, parent, buckets, applyState)
        , mKey(LedgerEntryKey(target))
        , mEntry(target)
        , mModified{false}
    {
    }

    Work::State
    onSuccess() override
    {
        if (!mModified)
        {
            auto& db = mApp.getDatabase();
            LedgerKey key = LedgerEntryKey(mEntry);
            auto ef = EntryFrame::storeLoad(key, db);
            if (ef && ef->mEntry == mEntry)
            {
                switch (mEntry.data.type())
                {
                case ACCOUNT:
                    modifyAccountEntry(ef->mEntry);
                    break;
                case TRUSTLINE:
                    modifyTrustLineEntry(ef->mEntry);
                    break;
                case OFFER:
                    modifyOfferEntry(ef->mEntry);
                    break;
                case DATA:
                    modifyDataEntry(ef->mEntry);
                    break;
                default:
                    REQUIRE(false);
                }
                LedgerHeader lh;
                LedgerDelta ld(lh, db, false);
                ef->storeChange(ld, db);
                mModified = true;
            }
        }
        auto r = ApplyBucketsWork::onSuccess();
        if (r == WORK_SUCCESS)
        {
            REQUIRE(mModified);
        }
        return r;
    }
};

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
        virtual std::vector<EntryFrame::pointer>
        generateLiveEntries()
        {
            return {};
        }

        virtual std::vector<LedgerKey>
        generateDeadEntries()
        {
            return {};
        }
    };

    EmptyBucketListGenerator blg;
    blg.generateLedgers(100);
    REQUIRE_NOTHROW(blg.applyBuckets());
}

TEST_CASE("BucketListIsConsistentWithDatabase multiple applies",
          "[invariant][bucketlistconsistent]")
{
    BucketListGenerator blg;
    blg.generateLedgers(100);
    REQUIRE_NOTHROW(blg.applyBuckets());
    blg.generateLedgers(100);
    REQUIRE_NOTHROW(blg.applyBuckets());
    blg.generateLedgers(100);
    REQUIRE_NOTHROW(blg.applyBuckets());
}

TEST_CASE("BucketListIsConsistentWithDatabase test root account",
          "[invariant][bucketlistconsistent]")
{
    struct TestRootBucketListGenerator : public BucketListGenerator
    {
        uint32_t const mTargetLedger;
        bool mModifiedRoot;

        TestRootBucketListGenerator()
            : BucketListGenerator()
            , mTargetLedger(
                  std::uniform_int_distribution<size_t>(2, 100)(*mGen))
            , mModifiedRoot(false)
        {
        }

        virtual std::vector<EntryFrame::pointer>
        generateLiveEntries()
        {
            if (mLedgerSeq == mTargetLedger)
            {
                mModifiedRoot = true;
                auto& app = mAppGenerate;
                auto skey = SecretKey::fromSeed(app->getNetworkID());
                auto root = skey.getPublicKey();
                auto& db = app->getDatabase();
                return {AccountFrame::loadAccount(root, db)};
            }
            else
            {
                return BucketListGenerator::generateLiveEntries();
            }
        }

        virtual std::vector<LedgerKey>
        generateDeadEntries()
        {
            return {};
        }
    };

    TestRootBucketListGenerator blg;
    blg.generateLedgers(100);
    REQUIRE(blg.mModifiedRoot);
    REQUIRE_NOTHROW(blg.applyBuckets());
}

TEST_CASE("BucketListIsConsistentWithDatabase added entries",
          "[invariant][bucketlistconsistent]")
{
    for (size_t nTests = 0; nTests < 40; ++nTests)
    {
        BucketListGenerator blg;
        blg.generateLedgers(100);

        std::uniform_int_distribution<uint32_t> addAtLedgerDist(2,
                                                                blg.mLedgerSeq);
        auto le = LedgerTestUtils::generateValidLedgerEntry(5);
        le.lastModifiedLedgerSeq = addAtLedgerDist(*blg.mGen);

        REQUIRE_THROWS_AS(blg.applyBuckets<ApplyBucketsWorkAddEntry>(le),
                          InvariantDoesNotHold);
    }
}

TEST_CASE("BucketListIsConsistentWithDatabase deleted entries",
          "[invariant][bucketlistconsistent]")
{
    for (auto t : xdr::xdr_traits<LedgerEntryType>::enum_values())
    {
        size_t nTests = 0;
        while (nTests < 10)
        {
            SelectBucketListGenerator blg(100, static_cast<LedgerEntryType>(t));
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
}

TEST_CASE("BucketListIsConsistentWithDatabase modified entries",
          "[invariant][bucketlistconsistent]")
{
    for (auto t : xdr::xdr_traits<LedgerEntryType>::enum_values())
    {
        size_t nTests = 0;
        while (nTests < 10)
        {
            SelectBucketListGenerator blg(100, static_cast<LedgerEntryType>(t));
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
}

TEST_CASE("BucketListIsConsistentWithDatabase bucket bounds",
          "[invariant][bucketlistconsistent]")
{
    struct LastModifiedBucketListGenerator : public BucketListGenerator
    {
        uint32_t const mTargetLedger;
        uint32_t const mChangeLedgerTo;
        uint32_t mModifiedLedger;

        LastModifiedBucketListGenerator(
            std::shared_ptr<std::default_random_engine> gen,
            uint32_t targetLedger, uint32_t changeLedgerTo)
            : BucketListGenerator(gen)
            , mTargetLedger(targetLedger)
            , mChangeLedgerTo(changeLedgerTo)
            , mModifiedLedger(false)
        {
        }

        virtual std::vector<EntryFrame::pointer>
        generateLiveEntries()
        {
            auto entries = BucketListGenerator::generateLiveEntries();
            if (mLedgerSeq == mTargetLedger)
            {
                mModifiedLedger = true;
                for (auto& ef : entries)
                {
                    ef->mEntry.lastModifiedLedgerSeq = mChangeLedgerTo;
                }
            }
            return entries;
        }
    };

    auto gen = std::make_shared<std::default_random_engine>();
    for (uint32_t level = 0; level < BucketList::kNumLevels; ++level)
    {
        uint32_t oldestLedger = BucketList::oldestLedgerInSnap(101, level);
        if (oldestLedger == std::numeric_limits<uint32_t>::max())
        {
            break;
        }
        uint32_t newestLedger = BucketList::oldestLedgerInCurr(101, level) +
                                BucketList::sizeOfCurr(101, level) - 1;
        std::uniform_int_distribution<uint32_t> ledgerToModifyDist(
            std::max(2u, oldestLedger), newestLedger);

        for (uint32_t i = 0; i < 10; ++i)
        {
            uint32_t ledgerToModify = ledgerToModifyDist(*gen);
            uint32_t maxLowTargetLedger = 0;
            uint32_t minHighTargetLedger = 0;
            if (ledgerToModify >= BucketList::oldestLedgerInCurr(101, level))
            {
                maxLowTargetLedger =
                    BucketList::oldestLedgerInCurr(101, level) - 1;
                minHighTargetLedger =
                    BucketList::oldestLedgerInCurr(101, level) +
                    BucketList::sizeOfCurr(101, level);
            }
            else
            {
                maxLowTargetLedger =
                    BucketList::oldestLedgerInSnap(101, level) - 1;
                minHighTargetLedger =
                    BucketList::oldestLedgerInCurr(101, level);
            }
            std::uniform_int_distribution<uint32_t> lowTargetLedgerDist(
                1, maxLowTargetLedger);
            std::uniform_int_distribution<uint32_t> highTargetLedgerDist(
                minHighTargetLedger, std::numeric_limits<int32_t>::max());

            uint32_t lowTarget = lowTargetLedgerDist(*gen);
            uint32_t highTarget = highTargetLedgerDist(*gen);
            for (auto target : {lowTarget, highTarget})
            {
                LastModifiedBucketListGenerator blg(gen, ledgerToModify,
                                                    target);
                blg.generateLedgers(100);
                REQUIRE_THROWS_AS(blg.applyBuckets(), InvariantDoesNotHold);
            }
        }
    }
}

TEST_CASE("BucketListIsConsistentWithDatabase merged LIVEENTRY and DEADENTRY",
          "[invariant][bucketlistconsistent]")
{
    struct MergeBucketListGenerator : public SelectBucketListGenerator
    {
        uint32_t const mTargetLedger;

        MergeBucketListGenerator(LedgerEntryType let)
            : SelectBucketListGenerator(25, let), mTargetLedger(110)
        {
        }

        virtual std::vector<LedgerKey>
        generateDeadEntries()
        {
            if (mLedgerSeq == mTargetLedger)
            {
                return {LedgerEntryKey(*mSelected)};
            }
            else
            {
                return SelectBucketListGenerator::generateDeadEntries();
            }
        }
    };

    testutil::BucketListDepthModifier bldm(3);
    for (auto t : xdr::xdr_traits<LedgerEntryType>::enum_values())
    {
        uint32_t nTests = 0;
        while (nTests < 5)
        {
            MergeBucketListGenerator blg(static_cast<LedgerEntryType>(t));
            auto& blGenerate =
                blg.mAppGenerate->getBucketManager().getBucketList();
            auto& blApply = blg.mAppApply->getBucketManager().getBucketList();

            blg.generateLedgers(100);
            if (!blg.mSelected)
            {
                continue;
            }

            BucketEntry dead(DEADENTRY);
            dead.deadEntry() = LedgerEntryKey(*blg.mSelected);
            BucketEntry live(LIVEENTRY);
            live.liveEntry() = *blg.mSelected;

            REQUIRE_NOTHROW(blg.applyBuckets());
            REQUIRE(EntryFrame::exists(blg.mAppGenerate->getDatabase(),
                                       LedgerEntryKey(*blg.mSelected)));
            REQUIRE(EntryFrame::exists(blg.mAppApply->getDatabase(),
                                       LedgerEntryKey(*blg.mSelected)));

            blg.generateLedgers(10);
            REQUIRE(doesBucketListContain(blGenerate, dead));
            REQUIRE(doesBucketListContain(blGenerate, live));

            blg.generateLedgers(100);
            REQUIRE(!doesBucketListContain(blGenerate, dead));
            REQUIRE(!doesBucketListContain(blGenerate, live));
            REQUIRE(!EntryFrame::exists(blg.mAppGenerate->getDatabase(),
                                        LedgerEntryKey(*blg.mSelected)));
            REQUIRE_NOTHROW(blg.applyBuckets());
            REQUIRE(!doesBucketListContain(blApply, dead));
            REQUIRE(!doesBucketListContain(blApply, live));
            REQUIRE(!EntryFrame::exists(blg.mAppApply->getDatabase(),
                                        LedgerEntryKey(*blg.mSelected)));

            ++nTests;
        }
    }
}
