// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/BucketListIsConsistentWithDatabase.h"
#include "bucket/BucketInputIterator.h"
#include "bucket/BucketManager.h"
#include "bucket/LiveBucket.h"
#include "bucket/LiveBucketList.h"
#include "database/Database.h"
#include "history/HistoryArchive.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerRange.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "main/Application.h"
#include "main/PersistentState.h"
#include "medida/timer.h"
#include "util/XDRCereal.h"
#include <chrono>
#include <fmt/chrono.h>
#include <fmt/format.h>
#include <map>

namespace stellar
{

namespace
{
std::string
checkAgainstDatabase(AbstractLedgerTxn& ltx, LedgerEntry const& entry)
{
    auto fromDb = ltx.loadWithoutRecord(LedgerEntryKey(entry));
    if (!fromDb)
    {
        std::string s{
            "Inconsistent state between objects (not found in database): "};
        s += xdrToCerealString(entry, "live");
        return s;
    }

    if (fromDb.current() == entry)
    {
        return {};
    }
    else
    {
        std::string s{"Inconsistent state between objects: "};
        s += xdrToCerealString(fromDb.current(), "db");
        s += xdrToCerealString(entry, "live");
        return s;
    }
}

std::string
checkAgainstDatabase(AbstractLedgerTxn& ltx, LedgerKey const& key)
{
    auto fromDb = ltx.loadWithoutRecord(key);
    if (!fromDb)
    {
        return {};
    }

    std::string s = "Entry with type DEADENTRY found in database ";
    s += xdrToCerealString(fromDb.current(), "db");
    return s;
}

std::string
checkDbEntryCounts(Application& app, LedgerRange const& range,
                   uint64_t expectedOfferCount)
{
    std::string msg;
    auto& ltxRoot = app.getLedgerTxnRoot();
    uint64_t numInDb = ltxRoot.countOffers(range);
    if (numInDb != expectedOfferCount)
    {
        msg = fmt::format(
            FMT_STRING("Incorrect OFFER count: Bucket = {:d} Database "
                       "= {:d}"),
            expectedOfferCount, numInDb);
    }

    return msg;
}
}

std::shared_ptr<Invariant>
BucketListIsConsistentWithDatabase::registerInvariant(Application& app)
{
    return app.getInvariantManager()
        .registerInvariant<BucketListIsConsistentWithDatabase>(app);
}

BucketListIsConsistentWithDatabase::BucketListIsConsistentWithDatabase(
    Application& app)
    : Invariant(true), mApp(app)
{
}

std::string
BucketListIsConsistentWithDatabase::getName() const
{
    return "BucketListIsConsistentWithDatabase";
}

void
BucketListIsConsistentWithDatabase::checkEntireBucketlist()
{
    auto& lm = mApp.getLedgerManager();
    auto& bm = mApp.getBucketManager();
    HistoryArchiveState has = lm.getLastClosedLedgerHAS();
    std::map<LedgerKey, LedgerEntry> bucketLedgerMap =
        bm.loadCompleteLedgerState(has);
    uint64_t offerCount = 0;
    medida::Timer timer(std::chrono::microseconds(1));

    {
        LedgerTxn ltx(mApp.getLedgerTxnRoot());
        for (auto const& pair : bucketLedgerMap)
        {
            // Don't check entry types supported by BucketListDB, since they
            // won't exist in SQL
            if (!LiveBucketIndex::typeNotSupported(pair.first.type()))
            {
                continue;
            }

            ++offerCount;
            std::string s;
            timer.Time([&]() { s = checkAgainstDatabase(ltx, pair.second); });
            if (!s.empty())
            {
                throw std::runtime_error(s);
            }

            if ((offerCount & 0x7ffff) == 0)
            {
                using namespace std::chrono;
                nanoseconds ns = timer.duration_unit() *
                                 static_cast<nanoseconds::rep>(timer.mean());
                microseconds us = duration_cast<microseconds>(ns);
                CLOG_INFO(Ledger,
                          "Checked bucket-vs-DB consistency for "
                          "{} entries (mean {}/entry)",
                          offerCount, us);
            }
        }
    }

    auto range = LedgerRange::inclusive(LedgerManager::GENESIS_LEDGER_SEQ,
                                        has.currentLedger);

    auto s = checkDbEntryCounts(mApp, range, offerCount);
    if (!s.empty())
    {
        throw std::runtime_error(s);
    }

    if (mApp.getPersistentState().getState(PersistentState::kDBBackend,
                                           mApp.getDatabase().getSession()) !=
        LiveBucketIndex::DB_BACKEND_STATE)
    {
        throw std::runtime_error(
            "Corrupt DB: BucketListDB flag "
            "not set in PersistentState. Please run new-db or upgrade-db");
    }
}

std::string
BucketListIsConsistentWithDatabase::checkAfterAssumeState(uint32_t newestLedger)
{
    uint64_t offerCount = 0;
    LedgerKeySet seenKeys;

    auto perBucketCheck = [&](auto bucket, auto& ltx) {
        for (LiveBucketInputIterator iter(bucket); iter; ++iter)
        {
            auto const& e = *iter;

            if (e.type() == LIVEENTRY || e.type() == INITENTRY)
            {
                if (e.liveEntry().data.type() != OFFER)
                {
                    continue;
                }

                // If this is the newest version of the key in the BucketList,
                // check against the db
                auto key = LedgerEntryKey(e.liveEntry());
                auto [_, newKey] = seenKeys.emplace(key);
                if (newKey)
                {
                    ++offerCount;
                    auto s = checkAgainstDatabase(ltx, e.liveEntry());
                    if (!s.empty())
                    {
                        return s;
                    }
                }
            }
            else if (e.type() == DEADENTRY)
            {
                if (e.deadEntry().type() != OFFER)
                {
                    continue;
                }

                // If this is the newest version of the key in the BucketList,
                // check against the db
                auto [_, newKey] = seenKeys.emplace(e.deadEntry());
                if (newKey)
                {
                    auto s = checkAgainstDatabase(ltx, e.deadEntry());
                    if (!s.empty())
                    {
                        return s;
                    }
                }
            }
        }

        return std::string{};
    };

    {
        LedgerTxn ltx(mApp.getLedgerTxnRoot());
        auto& bl = mApp.getBucketManager().getLiveBucketList();

        for (uint32_t i = 0; i < LiveBucketList::kNumLevels; ++i)
        {
            auto const& level = bl.getLevel(i);
            for (auto const& bucket : {level.getCurr(), level.getSnap()})
            {
                auto s = perBucketCheck(bucket, ltx);
                if (!s.empty())
                {
                    return s;
                }
            }
        }
    }

    auto range =
        LedgerRange::inclusive(LedgerManager::GENESIS_LEDGER_SEQ, newestLedger);

    return checkDbEntryCounts(mApp, range, offerCount);
}

std::string
BucketListIsConsistentWithDatabase::checkOnBucketApply(
    std::shared_ptr<LiveBucket const> bucket, uint32_t oldestLedger,
    uint32_t newestLedger, std::unordered_set<LedgerKey> const& shadowedKeys)
{
    uint64_t offerCount = 0;
    {
        LedgerTxn ltx(mApp.getLedgerTxnRoot());

        bool hasPreviousEntry = false;
        BucketEntry previousEntry;
        for (LiveBucketInputIterator iter(bucket); iter; ++iter)
        {
            auto const& e = *iter;
            if (hasPreviousEntry &&
                !BucketEntryIdCmp<LiveBucket>{}(previousEntry, e))
            {
                std::string s = "Bucket has out of order entries: ";
                s += xdrToCerealString(previousEntry, "previous");
                s += xdrToCerealString(e, "current");
                return s;
            }
            previousEntry = e;
            hasPreviousEntry = true;

            if (e.type() == LIVEENTRY || e.type() == INITENTRY)
            {
                if (e.liveEntry().lastModifiedLedgerSeq < oldestLedger)
                {
                    auto s = fmt::format(
                        FMT_STRING("lastModifiedLedgerSeq beneath lower"
                                   " bound for this bucket ({:d} < {:d}): "),
                        e.liveEntry().lastModifiedLedgerSeq, oldestLedger);
                    s += xdrToCerealString(e.liveEntry(), "live");
                    return s;
                }
                if (e.liveEntry().lastModifiedLedgerSeq > newestLedger)
                {
                    auto s = fmt::format(
                        FMT_STRING("lastModifiedLedgerSeq above upper"
                                   " bound for this bucket ({:d} > {:d}): "),
                        e.liveEntry().lastModifiedLedgerSeq, newestLedger);
                    s += xdrToCerealString(e.liveEntry(), "live");
                    return s;
                }

                // Don't check DB against keys shadowed by earlier Buckets
                if (LiveBucketIndex::typeNotSupported(
                        e.liveEntry().data.type()) &&
                    shadowedKeys.find(LedgerEntryKey(e.liveEntry())) ==
                        shadowedKeys.end())
                {
                    ++offerCount;
                    auto s = checkAgainstDatabase(ltx, e.liveEntry());
                    if (!s.empty())
                    {
                        return s;
                    }
                }
            }
            else
            {
                // Only check for OFFER keys that are not shadowed by an earlier
                // bucket
                if (LiveBucketIndex::typeNotSupported(e.deadEntry().type()) &&
                    shadowedKeys.find(e.deadEntry()) == shadowedKeys.end())
                {
                    auto s = checkAgainstDatabase(ltx, e.deadEntry());
                    if (!s.empty())
                    {
                        return s;
                    }
                }
            }
        }
    }

    auto range = LedgerRange::inclusive(oldestLedger, newestLedger);
    return checkDbEntryCounts(mApp, range, offerCount);
}
}
