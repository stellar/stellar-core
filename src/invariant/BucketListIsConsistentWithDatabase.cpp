// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/BucketListIsConsistentWithDatabase.h"
#include "bucket/Bucket.h"
#include "bucket/BucketInputIterator.h"
#include "crypto/Hex.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerRange.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "lib/util/format.h"
#include "main/Application.h"
#include "xdrpp/printer.h"

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
        s += xdr::xdr_to_string(entry, "live");
        return s;
    }

    if (fromDb.current() == entry)
    {
        return {};
    }
    else
    {
        std::string s{"Inconsistent state between objects: "};
        s += xdr::xdr_to_string(fromDb.current(), "db");
        s += xdr::xdr_to_string(entry, "live");
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
    s += xdr::xdr_to_string(fromDb.current(), "db");
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

std::string
BucketListIsConsistentWithDatabase::checkOnBucketApply(
    std::shared_ptr<Bucket const> bucket, uint32_t oldestLedger,
    uint32_t newestLedger)
{
    uint64_t nAccounts = 0, nTrustLines = 0, nOffers = 0, nData = 0;
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
                s += xdr::xdr_to_string(previousEntry, "previous");
                s += xdr::xdr_to_string(e, "current");
                return s;
            }
            previousEntry = e;
            hasPreviousEntry = true;

            if (e.type() == LIVEENTRY)
            {
                if (e.liveEntry().lastModifiedLedgerSeq < oldestLedger)
                {
                    auto s = fmt::format("lastModifiedLedgerSeq beneath lower"
                                         " bound for this bucket ({} < {}): ",
                                         e.liveEntry().lastModifiedLedgerSeq,
                                         oldestLedger);
                    s += xdr::xdr_to_string(e.liveEntry(), "live");
                    return s;
                }
                if (e.liveEntry().lastModifiedLedgerSeq > newestLedger)
                {
                    auto s = fmt::format("lastModifiedLedgerSeq above upper"
                                         " bound for this bucket ({} > {}): ",
                                         e.liveEntry().lastModifiedLedgerSeq,
                                         newestLedger);
                    s += xdr::xdr_to_string(e.liveEntry(), "live");
                    return s;
                }

                switch (e.liveEntry().data.type())
                {
                case ACCOUNT:
                    ++nAccounts;
                    break;
                case TRUSTLINE:
                    ++nTrustLines;
                    break;
                case OFFER:
                    ++nOffers;
                    break;
                case DATA:
                    ++nData;
                    break;
                default:
                    abort();
                }
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

    LedgerRange range{oldestLedger, newestLedger};
    std::string countFormat = "Incorrect {} count: Bucket = {} Database = {}";
    auto& ltxRoot = mApp.getLedgerTxnRoot();
    uint64_t nAccountsInDb = ltxRoot.countObjects(ACCOUNT, range);
    if (nAccountsInDb != nAccounts)
    {
        return fmt::format(countFormat, "Account", nAccounts, nAccountsInDb);
    }
    uint64_t nTrustLinesInDb = ltxRoot.countObjects(TRUSTLINE, range);
    if (nTrustLinesInDb != nTrustLines)
    {
        return fmt::format(countFormat, "TrustLine", nTrustLines,
                           nTrustLinesInDb);
    }
    uint64_t nOffersInDb = ltxRoot.countObjects(OFFER, range);
    if (nOffersInDb != nOffers)
    {
        return fmt::format(countFormat, "Offer", nOffers, nOffersInDb);
    }
    uint64_t nDataInDb = ltxRoot.countObjects(DATA, range);
    if (nDataInDb != nData)
    {
        return fmt::format(countFormat, "Data", nData, nDataInDb);
    }

    return {};
}
}
