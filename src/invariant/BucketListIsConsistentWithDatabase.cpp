// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/BucketListIsConsistentWithDatabase.h"
#include "bucket/Bucket.h"
#include "bucket/BucketInputIterator.h"
#include "bucket/BucketManager.h"
#include "crypto/Hex.h"
#include "history/HistoryArchive.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerRange.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "main/Application.h"
#include "medida/timer.h"
#include "util/XDRCereal.h"
#include <chrono>
#include <fmt/chrono.h>
#include <fmt/format.h>
#include <map>

namespace stellar
{

static std::string
checkAgainstDatabase(AbstractLedgerTxn& ltx, LedgerEntry const& entry)
{
    auto fromDb = ltx.loadWithoutRecord(LedgerEntryKey(entry));
    if (!fromDb)
    {
        std::string s{
            "Inconsistent state between objects (not found in database): "};
        s += xdr_to_string(entry, "live");
        return s;
    }

    if (fromDb.current() == entry)
    {
        return {};
    }
    else
    {
        std::string s{"Inconsistent state between objects: "};
        s += xdr_to_string(fromDb.current(), "db");
        s += xdr_to_string(entry, "live");
        return s;
    }
}

static std::string
checkAgainstDatabase(AbstractLedgerTxn& ltx, LedgerKey const& key)
{
    auto fromDb = ltx.loadWithoutRecord(key);
    if (!fromDb)
    {
        return {};
    }

    std::string s = "Entry with type DEADENTRY found in database ";
    s += xdr_to_string(fromDb.current(), "db");
    return s;
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

struct EntryCounts
{
    uint64_t mAccounts{0};
    uint64_t mTrustLines{0};
    uint64_t mOffers{0};
    uint64_t mData{0};
    uint64_t mClaimableBalance{0};

    uint64_t
    totalEntries() const
    {
        return mAccounts + mTrustLines + mOffers + mData + mClaimableBalance;
    }

    void
    countLiveEntry(LedgerEntry const& e)
    {
        switch (e.data.type())
        {
        case ACCOUNT:
            ++mAccounts;
            break;
        case TRUSTLINE:
            ++mTrustLines;
            break;
        case OFFER:
            ++mOffers;
            break;
        case DATA:
            ++mData;
            break;
        case CLAIMABLE_BALANCE:
            ++mClaimableBalance;
            break;
        default:
            throw std::runtime_error(
                fmt::format("unknown ledger entry type: {}",
                            static_cast<uint32_t>(e.data.type())));
        }
    }

    std::string
    checkDbEntryCounts(Application& app, LedgerRange const& range)
    {
        std::string countFormat =
            "Incorrect {} count: Bucket = {} Database = {}";
        auto& ltxRoot = app.getLedgerTxnRoot();
        uint64_t nAccountsInDb = ltxRoot.countObjects(ACCOUNT, range);
        if (nAccountsInDb != mAccounts)
        {
            return fmt::format(countFormat, "Account", mAccounts,
                               nAccountsInDb);
        }
        uint64_t nTrustLinesInDb = ltxRoot.countObjects(TRUSTLINE, range);
        if (nTrustLinesInDb != mTrustLines)
        {
            return fmt::format(countFormat, "TrustLine", mTrustLines,
                               nTrustLinesInDb);
        }
        uint64_t nOffersInDb = ltxRoot.countObjects(OFFER, range);
        if (nOffersInDb != mOffers)
        {
            return fmt::format(countFormat, "Offer", mOffers, nOffersInDb);
        }
        uint64_t nDataInDb = ltxRoot.countObjects(DATA, range);
        if (nDataInDb != mData)
        {
            return fmt::format(countFormat, "Data", mData, nDataInDb);
        }
        uint64_t nClaimableBalanceInDb =
            ltxRoot.countObjects(CLAIMABLE_BALANCE, range);
        if (nClaimableBalanceInDb != mClaimableBalance)
        {
            return fmt::format(countFormat, "ClaimableBalance",
                               mClaimableBalance, nClaimableBalanceInDb);
        }
        return {};
    }
};

void
BucketListIsConsistentWithDatabase::checkEntireBucketlist()
{
    auto& lm = mApp.getLedgerManager();
    auto& bm = mApp.getBucketManager();
    HistoryArchiveState has = lm.getLastClosedLedgerHAS();
    std::map<LedgerKey, LedgerEntry> bucketLedgerMap =
        bm.loadCompleteLedgerState(has);
    EntryCounts counts;
    medida::Timer timer(std::chrono::microseconds(1));

    {
        LedgerTxn ltx(mApp.getLedgerTxnRoot());
        for (auto const& pair : bucketLedgerMap)
        {
            counts.countLiveEntry(pair.second);
            std::string s;
            timer.Time([&]() { s = checkAgainstDatabase(ltx, pair.second); });
            if (!s.empty())
            {
                throw std::runtime_error(s);
            }
            auto i = counts.totalEntries();
            if ((i & 0x7ffff) == 0)
            {
                using namespace std::chrono;
                nanoseconds ns = timer.duration_unit() *
                                 static_cast<nanoseconds::rep>(timer.mean());
                microseconds us = duration_cast<microseconds>(ns);
                CLOG_INFO(Ledger,
                          "Checked bucket-vs-DB consistency for "
                          "{} entries (mean {}/entry)",
                          i, us);
            }
        }
    }
    auto range = LedgerRange::inclusive(LedgerManager::GENESIS_LEDGER_SEQ,
                                        has.currentLedger);
    auto s = counts.checkDbEntryCounts(mApp, range);
    if (!s.empty())
    {
        throw std::runtime_error(s);
    }
}

std::string
BucketListIsConsistentWithDatabase::checkOnBucketApply(
    std::shared_ptr<Bucket const> bucket, uint32_t oldestLedger,
    uint32_t newestLedger)
{
    EntryCounts counts;
    {
        LedgerTxn ltx(mApp.getLedgerTxnRoot());

        bool hasPreviousEntry = false;
        BucketEntry previousEntry;
        for (BucketInputIterator iter(bucket); iter; ++iter)
        {
            auto const& e = *iter;
            if (hasPreviousEntry && !BucketEntryIdCmp{}(previousEntry, e))
            {
                std::string s = "Bucket has out of order entries: ";
                s += xdr_to_string(previousEntry, "previous");
                s += xdr_to_string(e, "current");
                return s;
            }
            previousEntry = e;
            hasPreviousEntry = true;

            if (e.type() == LIVEENTRY || e.type() == INITENTRY)
            {
                if (e.liveEntry().lastModifiedLedgerSeq < oldestLedger)
                {
                    auto s = fmt::format("lastModifiedLedgerSeq beneath lower"
                                         " bound for this bucket ({} < {}): ",
                                         e.liveEntry().lastModifiedLedgerSeq,
                                         oldestLedger);
                    s += xdr_to_string(e.liveEntry(), "live");
                    return s;
                }
                if (e.liveEntry().lastModifiedLedgerSeq > newestLedger)
                {
                    auto s = fmt::format("lastModifiedLedgerSeq above upper"
                                         " bound for this bucket ({} > {}): ",
                                         e.liveEntry().lastModifiedLedgerSeq,
                                         newestLedger);
                    s += xdr_to_string(e.liveEntry(), "live");
                    return s;
                }

                counts.countLiveEntry(e.liveEntry());
                auto s = checkAgainstDatabase(ltx, e.liveEntry());
                if (!s.empty())
                {
                    return s;
                }
            }
            else if (e.type() == DEADENTRY)
            {
                auto s = checkAgainstDatabase(ltx, e.deadEntry());
                if (!s.empty())
                {
                    return s;
                }
            }
        }
    }

    auto range = LedgerRange::inclusive(oldestLedger, newestLedger);
    return counts.checkDbEntryCounts(mApp, range);
}
}
