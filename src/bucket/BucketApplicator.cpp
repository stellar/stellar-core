// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "bucket/BucketApplicator.h"
#include "bucket/Bucket.h"
#include "ledger/LedgerDelta.h"
#include "util/Logging.h"

namespace stellar
{

BucketApplicator::BucketApplicator(Database& db,
                                   std::shared_ptr<const Bucket> bucket)
    : mDb(db), mBucketIter(bucket)
{
}

BucketApplicator::operator bool() const
{
    return (bool)mBucketIter;
}

void
BucketApplicator::advance()
{
    soci::transaction sqlTx(mDb.getSession());
    for (; mBucketIter; ++mBucketIter)
    {
        LedgerHeader lh;
        LedgerDelta delta(lh, mDb, false);

        auto const& entry = *mBucketIter;
        if (entry.type() == LIVEENTRY)
        {
            EntryFrame::pointer ep = EntryFrame::FromXDR(entry.liveEntry());
            ep->storeAddOrChange(delta, mDb);
        }
        else
        {
            EntryFrame::storeDelete(delta, mDb, entry.deadEntry());
        }
        // No-op, just to avoid needless rollback.
        delta.commit();
        if ((++mSize & 0xff) == 0xff)
        {
            break;
        }
    }
    sqlTx.commit();
    mDb.clearPreparedStatementCache();

    if (!mBucketIter || (mSize & 0xfff) == 0xfff)
    {
        CLOG(INFO, "Bucket")
            << "Bucket-apply: committed " << mSize << " entries";
    }
}
}
