// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/BucketListIsConsistentWithDatabase.h"
#include "bucket/Bucket.h"
#include "bucket/BucketInputIterator.h"
#include "crypto/Hex.h"
#include "database/Database.h"
#include "invariant/InvariantManager.h"
#include "ledger/AccountFrame.h"
#include "ledger/DataFrame.h"
#include "ledger/OfferFrame.h"
#include "ledger/TrustFrame.h"
#include "lib/util/format.h"
#include "main/Application.h"
#include "xdrpp/printer.h"

namespace stellar
{

std::shared_ptr<Invariant>
BucketListIsConsistentWithDatabase::registerInvariant(Application& app)
{
    return app.getInvariantManager()
        .registerInvariant<BucketListIsConsistentWithDatabase>(
            app.getDatabase());
}

BucketListIsConsistentWithDatabase::BucketListIsConsistentWithDatabase(
    Database& db)
    : Invariant(true), mDb{db}
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
            auto s = EntryFrame::checkAgainstDatabase(e.liveEntry(), mDb);
            if (!s.empty())
            {
                return s;
            }
        }
        else if (e.type() == DEADENTRY)
        {
            if (EntryFrame::exists(mDb, e.deadEntry()))
            {
                auto fromDb = EntryFrame::storeLoad(e.deadEntry(), mDb);
                std::string s = "Entry with type DEADENTRY found in database ";
                s += xdr::xdr_to_string(fromDb->mEntry, "db");
                return s;
            }
        }
    }

    auto& sess = mDb.getSession();
    std::string countFormat = "Incorrect {} count: Bucket = {} Database = {}";
    uint64_t nAccountsInDb =
        AccountFrame::countObjects(sess, {oldestLedger, newestLedger});
    if (nAccountsInDb != nAccounts)
    {
        return fmt::format(countFormat, "Account", nAccounts, nAccountsInDb);
    }
    uint64_t nTrustLinesInDb =
        TrustFrame::countObjects(sess, {oldestLedger, newestLedger});
    if (nTrustLinesInDb != nTrustLines)
    {
        return fmt::format(countFormat, "TrustLine", nTrustLines,
                           nTrustLinesInDb);
    }
    uint64_t nOffersInDb =
        OfferFrame::countObjects(sess, {oldestLedger, newestLedger});
    if (nOffersInDb != nOffers)
    {
        return fmt::format(countFormat, "Offer", nOffers, nOffersInDb);
    }
    uint64_t nDataInDb =
        DataFrame::countObjects(sess, {oldestLedger, newestLedger});
    if (nDataInDb != nData)
    {
        return fmt::format(countFormat, "Data", nData, nDataInDb);
    }
    return {};
}
}
