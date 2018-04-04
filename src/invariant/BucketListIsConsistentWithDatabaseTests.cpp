// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"

#include "bucket/Bucket.h"
#include "bucket/BucketInputIterator.h"
#include "bucket/BucketList.h"
#include "bucket/BucketManager.h"
#include "bucket/BucketOutputIterator.h"
#include "catchup/ApplyBucketsWork.h"
#include "database/Database.h"
#include "invariant/Invariant.h"
#include "invariant/InvariantDoesNotHold.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerState.h"
#include "ledger/LedgerTestUtils.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "work/WorkManager.h"
#include <random>
#include <util/basen.h>

using namespace stellar;
using namespace std::placeholders;

namespace stellar
{
using xdr::operator<;
}

namespace BucketListIsConsistentWithDatabaseTests
{

std::vector<LedgerEntry>
generateValidLedgerEntries(uint32_t n, uint32_t ledgerSeq)
{
    auto entries = stellar::LedgerTestUtils::generateValidLedgerEntries(n);
    for (auto& le : entries)
    {
        le.lastModifiedLedgerSeq = ledgerSeq;
    }
    return entries;
}

std::vector<LedgerKey>
deleteRandomLedgerEntries(uint32_t n, const std::set<LedgerKey>& allLive,
                          std::default_random_engine& gen)
{
    std::set<LedgerKey> live(allLive);
    std::vector<LedgerKey> dead;
    while (dead.size() < n && !live.empty())
    {
        auto dist = std::uniform_int_distribution<size_t>(0, live.size() - 1);
        auto index = dist(gen);
        auto iter = live.begin();
        std::advance(iter, index);
        dead.push_back(*iter);
        live.erase(iter);
    }
    return dead;
}

uint32_t
generateLedgers(
    Application::pointer app, uint32_t ledgerSeq, uint32_t nLedgers,
    uint32_t nLive,
    std::function<std::vector<LedgerEntry>(uint32_t, uint32_t)> genLive =
        generateValidLedgerEntries,
    uint32_t nDead = 0,
    std::function<std::vector<LedgerKey>(uint32_t, std::set<LedgerKey> const&)>
        genDead = {},
    std::function<uint32_t(uint32_t)> lastModified = {})
{
    assert(nDead == 0 || genDead);

    std::default_random_engine gen;
    std::set<LedgerKey> allLive;
    uint32_t stopLedgerSeq = ledgerSeq + nLedgers - 1;
    for (; ledgerSeq <= stopLedgerSeq; ++ledgerSeq)
    {
        std::vector<LedgerKey> dead;
        if (nDead > 0)
        {
            dead = genDead(nDead, allLive);
            assert(dead.size() <=
                   (nDead < allLive.size() ? nDead : allLive.size()));
            LedgerState lsDead(app->getLedgerStateRoot());
            for (auto const& key : dead)
            {
                auto iter = allLive.find(key);
                assert(iter != allLive.end());
                auto ler = lsDead.load(key);
                ler->erase();
                ler->invalidate();
                allLive.erase(iter);
            }
            lsDead.commit();
        }

        uint32_t lastModifiedLedgerSeq =
            !lastModified ? ledgerSeq : lastModified(ledgerSeq);
        auto live = genLive(nLive, lastModifiedLedgerSeq);
        assert(live.size() == nLive);
        LedgerState lsLive(app->getLedgerStateRoot());
        for (auto& le : live)
        {
            auto ler = lsLive.load(LedgerEntryKey(le));
            if (ler)
            {
                *ler->entry() = le;
                ler->invalidate();
            }
            else
            {
                lsLive.create(le)->invalidate();
            }
            allLive.insert(LedgerEntryKey(le));
        }
        lsLive.commit();

        auto& bl = app->getBucketManager().getBucketList();
        bl.addBatch(*app, ledgerSeq, live, dead);
    }

    return stopLedgerSeq;
}

void
getHistoryArchiveState(Application::pointer appGenerate,
                       Application::pointer appApply, uint32_t ledgerSeq,
                       std::map<std::string, std::shared_ptr<Bucket>>& buckets,
                       HistoryArchiveState& has)
{
    auto& blGenerate = appGenerate->getBucketManager().getBucketList();
    auto& bmApply = appApply->getBucketManager();

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
    has = HistoryArchiveState(ledgerSeq, blGenerate);
}

template <typename T = ApplyBucketsWork, typename... Args>
void
applyBucketsAndCrankUntilDone(Application::pointer appGenerate,
                              Application::pointer appApply, uint32_t ledgerSeq,
                              Args&&... args)
{
    std::map<std::string, std::shared_ptr<Bucket>> buckets;
    HistoryArchiveState has;
    getHistoryArchiveState(appGenerate, appApply, ledgerSeq, buckets, has);

    auto& wm = appApply->getWorkManager();
    wm.executeWork<T>(buckets, has, std::forward<Args>(args)...);
}

std::vector<LedgerEntry>
generateValidLedgerEntriesAndSelectByType(
    uint32_t n, uint32_t ledgerSeq, LedgerEntryType let,
    std::map<LedgerKey, LedgerEntry>& selected)
{
    auto entries = generateValidLedgerEntries(n, ledgerSeq);
    for (auto const& le : entries)
    {
        if (le.data.type() == let)
        {
            selected[LedgerEntryKey(le)] = le;
        }
    }
    return entries;
}

std::vector<LedgerKey>
generateDeadEntriesAndSelectByType(
    uint32_t n, std::set<LedgerKey> const& allLive,
    std::default_random_engine& gen,
    std::map<LedgerKey, LedgerEntry>& selected)
{
    auto keys = deleteRandomLedgerEntries(n, allLive, gen);
    for (auto const& key : keys)
    {
        selected.erase(key);
    }
    return keys;
}

template <typename ABW, typename T>
void
checkBucketListIsConsistentWithDatabase(
    std::function<std::vector<LedgerEntry>(uint32_t, uint32_t, LedgerEntryType, T&)>
        genLive = generateValidLedgerEntries,
    std::function<std::vector<LedgerKey>(uint32_t, std::set<LedgerKey> const&,
                                         std::default_random_engine&, T&)>
        genDead = {})
{
    std::default_random_engine gen;
    for (auto t : xdr::xdr_traits<LedgerEntryType>::enum_values())
    {
        size_t nTests = 0;
        while (nTests < 10)
        {
            LedgerEntryType let = static_cast<LedgerEntryType>(t);

            VirtualClock clock;
            Application::pointer appGenerate =
                createTestApplication(clock, getTestConfig(0));
            Application::pointer appApply =
                createTestApplication(clock, getTestConfig(1));

            T selected;
            uint32_t ledgerSeq = generateLedgers(
                appGenerate, 2, 100, 5,
                std::bind(genLive, _1, _2, let, std::ref(selected)), 2,
                std::bind(genDead, _1, _2, std::ref(gen), std::ref(selected)));

            // NOTE: We only advance nTests if selected is not empty so that
            // we make sure we actually performed a test.
            if (!selected.empty())
            {
                auto dist = std::uniform_int_distribution<size_t>(
                    0, selected.size() - 1);
                auto index = dist(gen);
                auto iter = selected.cbegin();
                std::advance(iter, index);
                REQUIRE_THROWS_AS(
                    applyBucketsAndCrankUntilDone<ABW>(appGenerate, appApply,
                                                       ledgerSeq, *iter, let),
                    InvariantDoesNotHold);
                ++nTests;
            }
        }
    }
}

class ApplyBucketsWorkAddEntry : public ApplyBucketsWork
{
  private:
    uint32_t mFromLedgerSeq;
    LedgerEntryType mType;
    bool mAdded;

  public:
    ApplyBucketsWorkAddEntry(
        Application& app, WorkParent& parent,
        std::map<std::string, std::shared_ptr<Bucket>> const& buckets,
        HistoryArchiveState const& applyState, uint32_t addAtLedgerSeq,
        LedgerEntryType addType)
        : ApplyBucketsWork(app, parent, buckets, applyState)
        , mFromLedgerSeq{addAtLedgerSeq}
        , mType{addType}
        , mAdded{false}
    {
    }

    Work::State
    onSuccess() override
    {
        if (!mAdded)
        {
            uint32_t minLedger = mFromLedgerSeq == 1 ? 2 : mFromLedgerSeq;
            uint32_t maxLedger = std::numeric_limits<int32_t>::max();
            uint64_t count =
                mApp.countAccounts({minLedger, maxLedger}) +
                mApp.countTrustLines({minLedger, maxLedger}) +
                mApp.countOffers({minLedger, maxLedger}) +
                mApp.countData({minLedger, maxLedger});

            if (count > 0)
            {
                LedgerEntry entry;
                entry.data.type(mType);
                entry.lastModifiedLedgerSeq = mFromLedgerSeq;
                switch (mType)
                {
                case ACCOUNT:
                    entry.data.account() =
                        LedgerTestUtils::generateValidAccountEntry(5);
                    break;
                case TRUSTLINE:
                    entry.data.trustLine() =
                        LedgerTestUtils::generateValidTrustLineEntry(5);
                    break;
                case OFFER:
                    entry.data.offer() =
                        LedgerTestUtils::generateValidOfferEntry(5);
                    break;
                case DATA:
                    entry.data.data() =
                        LedgerTestUtils::generateValidDataEntry(5);
                    break;
                default:
                    REQUIRE(false);
                }

                LedgerState ls(mApp.getLedgerStateRoot());
                ls.create(entry);
                ls.commit();
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
        HistoryArchiveState const& applyState,
        std::pair<LedgerKey, LedgerEntry> const& target,
        LedgerEntryType)
        : ApplyBucketsWork(app, parent, buckets, applyState)
        , mKey(target.first)
        , mEntry(target.second)
        , mDeleted{false}
    {
    }

    Work::State
    onSuccess() override
    {
        if (!mDeleted)
        {
            LedgerState ls(mApp.getLedgerStateRoot());
            auto ler = ls.load(mKey);
            if (ler)
            {
                if (*ler->entry() == mEntry)
                {
                    ler->erase();
                    ls.commit();
                    mDeleted = true;
                }
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
    modifyAccountEntry()
    {
        AccountEntry account = mEntry.data.account();
        LedgerEntry& entry = mEntry;
        entry.lastModifiedLedgerSeq = mEntry.lastModifiedLedgerSeq;
        entry.data.account() = LedgerTestUtils::generateValidAccountEntry(5);
        entry.data.account().accountID = account.accountID;
    }

    void
    modifyTrustLineEntry()
    {
        TrustLineEntry trustLine = mEntry.data.trustLine();
        LedgerEntry& entry = mEntry;
        entry.lastModifiedLedgerSeq = mEntry.lastModifiedLedgerSeq;
        entry.data.trustLine() =
            LedgerTestUtils::generateValidTrustLineEntry(5);
        entry.data.trustLine().accountID = trustLine.accountID;
        entry.data.trustLine().asset = trustLine.asset;
    }

    void
    modifyOfferEntry()
    {
        OfferEntry offer = mEntry.data.offer();
        LedgerEntry& entry = mEntry;
        entry.lastModifiedLedgerSeq = mEntry.lastModifiedLedgerSeq;
        entry.data.offer() = LedgerTestUtils::generateValidOfferEntry(5);
        entry.data.offer().sellerID = offer.sellerID;
        entry.data.offer().offerID = offer.offerID;
    }

    void
    modifyDataEntry()
    {
        DataEntry data = mEntry.data.data();
        LedgerEntry& entry = mEntry;
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
        HistoryArchiveState const& applyState,
        std::pair<LedgerKey, LedgerEntry> const& target,
        LedgerEntryType)
        : ApplyBucketsWork(app, parent, buckets, applyState)
        , mKey(target.first)
        , mEntry(target.second)
        , mModified{false}
    {
    }

    Work::State
    onSuccess() override
    {
        if (!mModified)
        {
            LedgerState ls(mApp.getLedgerStateRoot());
            auto ler = ls.load(mKey);
            if (ler)
            {
                if (*ler->entry() == mEntry)
                {
                    switch (mEntry.data.type())
                    {
                    case ACCOUNT:
                        modifyAccountEntry();
                        break;
                    case TRUSTLINE:
                        modifyTrustLineEntry();
                        break;
                    case OFFER:
                        modifyOfferEntry();
                        break;
                    case DATA:
                        modifyDataEntry();
                        break;
                    default:
                        REQUIRE(false);
                    }

                    *ler->entry() = mEntry;
                    ls.commit();
                    mModified = true;
                }
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
}

using namespace BucketListIsConsistentWithDatabaseTests;

TEST_CASE("BucketListIsConsistentWithDatabase succeed",
          "[invariant][bucketlistconsistent]")
{
    std::default_random_engine gen;
    VirtualClock clock;
    Application::pointer appGenerate =
        createTestApplication(clock, getTestConfig(0));
    Application::pointer appApply =
        createTestApplication(clock, getTestConfig(1));
    uint32_t ledgerSeq = generateLedgers(
        appGenerate, 2, 100, 5, generateValidLedgerEntries, 2,
        std::bind(deleteRandomLedgerEntries, _1, _2, std::ref(gen)));
    REQUIRE_NOTHROW(
        applyBucketsAndCrankUntilDone(appGenerate, appApply, ledgerSeq));
}

TEST_CASE("BucketListIsConsistentWithDatabase empty ledgers",
          "[invariant][bucketlistconsistent]")
{
    VirtualClock clock;
    Application::pointer appGenerate =
        createTestApplication(clock, getTestConfig(0));
    Application::pointer appApply =
        createTestApplication(clock, getTestConfig(1));
    uint32_t ledgerSeq = generateLedgers(appGenerate, 2, 100, 0);
    REQUIRE_NOTHROW(
        applyBucketsAndCrankUntilDone(appGenerate, appApply, ledgerSeq));
}

TEST_CASE("BucketListIsConsistentWithDatabase multiple applies",
          "[invariant][bucketlistconsistent]")
{
    std::default_random_engine gen;
    VirtualClock clock;
    Application::pointer appGenerate =
        createTestApplication(clock, getTestConfig(0));
    Application::pointer appApply =
        createTestApplication(clock, getTestConfig(1));
    uint32_t ledgerSeq = 1;
    for (int i = 0; i < 3; ++i)
    {
        ledgerSeq = generateLedgers(
            appGenerate, ++ledgerSeq, 100, 5, generateValidLedgerEntries, 2,
            std::bind(deleteRandomLedgerEntries, _1, _2, std::ref(gen)));
        REQUIRE_NOTHROW(
            applyBucketsAndCrankUntilDone(appGenerate, appApply, ledgerSeq));
    }
}

TEST_CASE("BucketListIsConsistentWithDatabase test non-root account",
          "[invariant][bucketlistconsistent]")
{
    VirtualClock clock;
    Application::pointer appGenerate =
        createTestApplication(clock, getTestConfig(0));
    Application::pointer appApply =
        createTestApplication(clock, getTestConfig(1));
    LedgerKey account;
    uint32_t ledgerSeq =
        generateLedgers(appGenerate, 2, 1, 1,
            [appGenerate, &account](uint32_t n, uint32_t ledgerSeq)
                    -> std::vector<LedgerEntry> {
                LedgerEntry entry;
                entry.lastModifiedLedgerSeq = ledgerSeq;
                entry.data.type(ACCOUNT);
                entry.data.account() =
                    LedgerTestUtils::generateValidAccountEntry(5);
                account = LedgerEntryKey(entry);
                return {entry};
            });
    REQUIRE_NOTHROW(
        applyBucketsAndCrankUntilDone(appGenerate, appApply, ledgerSeq));

    ledgerSeq = generateLedgers(
        appGenerate, ++ledgerSeq, 100, 1,
        [appGenerate, account](uint32_t n, uint32_t ledgerSeq)
                -> std::vector<LedgerEntry> {
            LedgerState ls(appGenerate->getLedgerStateRoot());
            auto ler = ls.load(account);
            ler->entry()->lastModifiedLedgerSeq = ledgerSeq;
            return {*ler->entry()};
        });
    REQUIRE_NOTHROW(
        applyBucketsAndCrankUntilDone(appGenerate, appApply, ledgerSeq));
}

TEST_CASE("BucketListIsConsistentWithDatabase test root account",
          "[invariant][bucketlistconsistent]")
{
    VirtualClock clock;
    Application::pointer appGenerate =
        createTestApplication(clock, getTestConfig(0));
    Application::pointer appApply =
        createTestApplication(clock, getTestConfig(1));
    uint32_t ledgerSeq = generateLedgers(appGenerate, 2, 1, 1);
    ledgerSeq = generateLedgers(
        appGenerate, ++ledgerSeq, 100, 1,
        [appGenerate](uint32_t n, uint32_t ledgerSeq)
                -> std::vector<LedgerEntry> {
            auto skey = SecretKey::fromSeed(appGenerate->getNetworkID());
            LedgerKey rootKey(ACCOUNT);
            rootKey.account().accountID = skey.getPublicKey();
            LedgerState ls(appGenerate->getLedgerStateRoot());
            auto ler = ls.load(rootKey);
            ler->entry()->lastModifiedLedgerSeq = ledgerSeq;
            return {*ler->entry()};
        });
    REQUIRE_NOTHROW(
        applyBucketsAndCrankUntilDone(appGenerate, appApply, ledgerSeq));
}

TEST_CASE("BucketListIsConsistentWithDatabase added entries",
          "[invariant][bucketlistconsistent]")
{
    checkBucketListIsConsistentWithDatabase<ApplyBucketsWorkAddEntry,
                                            std::vector<uint32_t>>(
        [](uint32_t n, uint32_t ledgerSeq,
           LedgerEntryType let, std::vector<uint32_t>& selected) {
            selected.push_back(static_cast<uint32_t>(ledgerSeq));
            auto entries = generateValidLedgerEntries(n, ledgerSeq);
            return entries;
        },
        std::bind(deleteRandomLedgerEntries, _1, _2, _3));
}

TEST_CASE("BucketListIsConsistentWithDatabase deleted entries",
          "[invariant][bucketlistconsistent]")
{
    checkBucketListIsConsistentWithDatabase<
        ApplyBucketsWorkDeleteEntry, std::map<LedgerKey, LedgerEntry>>(
        generateValidLedgerEntriesAndSelectByType,
        generateDeadEntriesAndSelectByType);
}

TEST_CASE("BucketListIsConsistentWithDatabase modified entries",
          "[invariant][bucketlistconsistent]")
{
    checkBucketListIsConsistentWithDatabase<
        ApplyBucketsWorkModifyEntry, std::map<LedgerKey, LedgerEntry>>(
        generateValidLedgerEntriesAndSelectByType,
        generateDeadEntriesAndSelectByType);
}

TEST_CASE("BucketListIsConsistentWithDatabase bucket bounds",
          "[invariant][bucketlistconsistent]")
{
    std::default_random_engine gen;
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
            uint32_t ledgerToModify = ledgerToModifyDist(gen);
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

            uint32_t lowTarget = lowTargetLedgerDist(gen);
            uint32_t highTarget = highTargetLedgerDist(gen);
            for (auto target : {lowTarget, highTarget})
            {
                VirtualClock clock;
                Application::pointer appGenerate =
                    createTestApplication(clock, getTestConfig(0));
                Application::pointer appApply =
                    createTestApplication(clock, getTestConfig(1));
                uint32_t ledgerSeq = generateLedgers(
                    appGenerate, 2, 100, 5, generateValidLedgerEntries, 2,
                    std::bind(deleteRandomLedgerEntries, _1, _2, std::ref(gen)),
                    [ledgerToModify, target](uint32_t ledger) {
                        return (ledger != ledgerToModify) ? ledger : target;
                    });
                REQUIRE_THROWS_AS(applyBucketsAndCrankUntilDone(
                                      appGenerate, appApply, ledgerSeq),
                                  InvariantDoesNotHold);
            }
        }
    }
}

TEST_CASE("BucketListIsConsistentWithDatabase merged LIVEENTRY and DEADENTRY",
          "[invariant][bucketlistconsistent]")
{
    testutil::BucketListDepthModifier bldm(3);
    std::default_random_engine gen;

    for (auto t : xdr::xdr_traits<LedgerEntryType>::enum_values())
    {
        uint32_t nTests = 0;
        while (nTests < 5)
        {
            LedgerEntryType let = static_cast<LedgerEntryType>(t);

            VirtualClock clock;
            Application::pointer appGenerate =
                Application::create(clock, getTestConfig(0));
            Application::pointer appApply =
                Application::create(clock, getTestConfig(1));
            auto& blGenerate = appGenerate->getBucketManager().getBucketList();

            // Generate initial entries and select one to delete
            std::map<LedgerKey, LedgerEntry> selected;
            uint32_t ledgerSeq = generateLedgers(
                appGenerate, 2, 10, 5,
                std::bind(generateValidLedgerEntriesAndSelectByType, _1, _2, let,
                          std::ref(selected)),
                2,
                std::bind(generateDeadEntriesAndSelectByType, _1, _2,
                          std::ref(gen), std::ref(selected)));
            if (selected.empty())
            {
                continue;
            }
            LedgerEntry entry;
            LedgerKey key;
            {
                auto dist = std::uniform_int_distribution<size_t>(
                    0, selected.size() - 1);
                auto iter = selected.cbegin();
                std::advance(iter, dist(gen));
                entry = iter->second;
                key = LedgerEntryKey(entry);
            }

            // Generate a random number of entries to push the selected entry
            // into a random bucket
            std::uniform_int_distribution<uint32_t> dist(1, 32);
            ledgerSeq = generateLedgers(
                appGenerate, ++ledgerSeq, dist(gen), 5,
                generateValidLedgerEntries, 2,
                [&key, &gen](uint32_t n, const std::set<LedgerKey>& allLive) {
                    auto keys = deleteRandomLedgerEntries(n, allLive, gen);
                    std::remove(keys.begin(), keys.end(), key);
                    return keys;
                });

            // Catch up to a point where the selected entry exists
            REQUIRE_NOTHROW(applyBucketsAndCrankUntilDone(appGenerate, appApply,
                                                          ledgerSeq));
            {
                LedgerState ls(appApply->getLedgerStateRoot());
                REQUIRE(ls.load(key));
            }

            // Generate entries to push the selected entry into the lowest
            // level of the BucketList
            ledgerSeq = generateLedgers(
                appGenerate, ++ledgerSeq, 100, 5, generateValidLedgerEntries, 2,
                [&key, &gen](uint32_t n, const std::set<LedgerKey>& allLive) {
                    auto keys = deleteRandomLedgerEntries(n, allLive, gen);
                    std::remove(keys.begin(), keys.end(), key);
                    return keys;
                });

            // Delete the selected entry
            {
                LedgerState ls(appGenerate->getLedgerStateRoot());
                ls.load(key)->erase();
                ls.commit();
                blGenerate.addBatch(*appGenerate, ++ledgerSeq, {}, {key});
            }

            // Check that the BucketList contains a DEADENTRY and LIVEENTRY
            // for the selected entry
            BucketEntry dead(DEADENTRY);
            dead.deadEntry() = key;
            REQUIRE(doesBucketListContain(blGenerate, dead));
            BucketEntry live(LIVEENTRY);
            live.liveEntry() = entry;
            REQUIRE(doesBucketListContain(blGenerate, live));

            // Generate entries to push the DEADENTRY for the selected entry
            // into the lowest level of the BucketList
            while (doesBucketListContain(blGenerate, dead))
            {
                ledgerSeq = generateLedgers(appGenerate, ++ledgerSeq, 1, 5,
                                            generateValidLedgerEntries);
            }

            REQUIRE(!doesBucketListContain(blGenerate, dead));
            REQUIRE(!doesBucketListContain(blGenerate, live));
            REQUIRE_NOTHROW(applyBucketsAndCrankUntilDone(appGenerate, appApply,
                                                          ledgerSeq));
            {
                LedgerState ls(appApply->getLedgerStateRoot());
                REQUIRE(!ls.load(key));
            }
            ++nTests;
        }
    }
}
