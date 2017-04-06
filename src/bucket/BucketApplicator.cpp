// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "bucket/Bucket.h"
#include "bucket/BucketApplicator.h"
#include "ledger/LedgerDelta.h"
#include "util/Logging.h"

namespace stellar
{

BucketApplicator::BucketApplicator(Database& db,
                                   std::shared_ptr<const Bucket> bucket)
    : mDb(db), mBucket(bucket)
{
    if (!bucket->getFilename().empty())
    {
        mIn.open(bucket->getFilename());
    }
}

BucketApplicator::operator bool() const
{
    return (bool)mIn;
}

void
BucketApplicator::advance()
{
    soci::transaction sqlTx(mDb.getSession());
    BucketEntry entry;
    while (mIn && mIn.readOne(entry))
    {
        LedgerHeader lh;
        LedgerDelta delta(lh, mDb);
        if (entry.type() == LIVEENTRY)
        {
            EntryFrame::pointer ep = EntryFrame::FromXDR(entry.liveEntry());
            delta.getHeader().ledgerSeq = ep->getLastModified();
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

    if (!mIn || (mSize & 0xfff) == 0xfff)
    {
        CLOG(INFO, "Bucket") << "Bucket-apply: committed " << mSize
                             << " entries";
    }
}
}
