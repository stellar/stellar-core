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
#include "main/PersistentState.h"
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
    uint64_t mLiquidityPool{0};
    uint64_t mContractData{0};
    uint64_t mContractCode{0};
    uint64_t mConfigSettings{0};

    uint64_t
    totalEntries() const
    {
        return mAccounts + mTrustLines + mOffers + mData + mClaimableBalance +
               mLiquidityPool + mContractData + mConfigSettings;
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
        case LIQUIDITY_POOL:
            ++mLiquidityPool;
            break;
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
        case CONTRACT_DATA:
            ++mContractData;
            break;
        case CONTRACT_CODE:
            ++mContractCode;
            break;
        case CONFIG_SETTING:
            ++mConfigSettings;
            break;
#endif
        default:
            throw std::runtime_error(
                fmt::format(FMT_STRING("unknown ledger entry type: {:d}"),
                            static_cast<uint32_t>(e.data.type())));
        }
    }

    std::string
    checkDbEntryCounts(Application& app, LedgerRange const& range,
                       std::function<bool(LedgerEntryType)> entryTypeFilter)
    {
        std::string msg;
        auto check = [&](LedgerEntryType let, uint64_t numInBucket) {
            if (entryTypeFilter(let))
            {
                auto& ltxRoot = app.getLedgerTxnRoot();
                uint64_t numInDb = ltxRoot.countObjects(let, range);
                if (numInDb != numInBucket)
                {
                    msg = fmt::format(
                        FMT_STRING("Incorrect {} count: Bucket = {:d} Database "
                                   "= {:d}"),
                        xdr::xdr_traits<LedgerEntryType>::enum_name(let),
                        numInBucket, numInDb);
                    return false;
                }
            }
            return true;
        };

        // Uses short-circuiting to make this compact
        check(ACCOUNT, mAccounts) && check(TRUSTLINE, mTrustLines) &&
            check(OFFER, mOffers) && check(DATA, mData) &&
            check(CLAIMABLE_BALANCE, mClaimableBalance) &&
            check(LIQUIDITY_POOL, mLiquidityPool)
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
            && check(CONTRACT_DATA, mContractData) &&
            check(CONTRACT_CODE, mContractCode) &&
            check(CONFIG_SETTING, mConfigSettings)
#endif
            ;
        return msg;
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
            // Don't check entry types in BucketListDB when enabled
            if (mApp.getConfig().isUsingBucketListDB() &&
                !BucketIndex::typeNotSupported(pair.first.type()))
            {
                continue;
            }

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

    // Count functionality does not support in-memory LedgerTxn
    if (!mApp.getConfig().isInMemoryMode())
    {
        auto range = LedgerRange::inclusive(LedgerManager::GENESIS_LEDGER_SEQ,
                                            has.currentLedger);

        // If BucketListDB enabled, only types not supported by BucketListDB
        // should be in SQL DB
        std::function<bool(LedgerEntryType)> filter;
        if (mApp.getConfig().isUsingBucketListDB())
        {
            filter = BucketIndex::typeNotSupported;
        }
        else
        {
            filter = [](LedgerEntryType) { return true; };
        }

        auto s = counts.checkDbEntryCounts(mApp, range, filter);
        if (!s.empty())
        {
            throw std::runtime_error(s);
        }
    }

    if (mApp.getConfig().isUsingBucketListDB() &&
        mApp.getPersistentState().getState(PersistentState::kDBBackend) !=
            BucketIndex::DBBackendState)
    {
        throw std::runtime_error("BucketListDB enabled but BucketListDB flag "
                                 "not set in PersistentState.");
    }
}

std::string
BucketListIsConsistentWithDatabase::checkOnBucketApply(
    std::shared_ptr<Bucket const> bucket, uint32_t oldestLedger,
    uint32_t newestLedger, std::function<bool(LedgerEntryType)> entryTypeFilter)
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
                    auto s = fmt::format(
                        FMT_STRING("lastModifiedLedgerSeq beneath lower"
                                   " bound for this bucket ({:d} < {:d}): "),
                        e.liveEntry().lastModifiedLedgerSeq, oldestLedger);
                    s += xdr_to_string(e.liveEntry(), "live");
                    return s;
                }
                if (e.liveEntry().lastModifiedLedgerSeq > newestLedger)
                {
                    auto s = fmt::format(
                        FMT_STRING("lastModifiedLedgerSeq above upper"
                                   " bound for this bucket ({:d} > {:d}): "),
                        e.liveEntry().lastModifiedLedgerSeq, newestLedger);
                    s += xdr_to_string(e.liveEntry(), "live");
                    return s;
                }

                if (entryTypeFilter(e.liveEntry().data.type()))
                {
                    counts.countLiveEntry(e.liveEntry());
                    auto s = checkAgainstDatabase(ltx, e.liveEntry());
                    if (!s.empty())
                    {
                        return s;
                    }
                }
            }
            else if (e.type() == DEADENTRY)
            {
                if (entryTypeFilter(e.deadEntry().type()))
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
    return counts.checkDbEntryCounts(mApp, range, entryTypeFilter);
}
}
