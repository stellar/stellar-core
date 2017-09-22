// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "bucket/Bucket.h"
#include "bucket/BucketApplicator.h"
#include "database/Database.h"
#include "database/EntryQueries.h"
#include "ledger/EntryFrame.h"
#include "ledger/LedgerEntries.h"
#include "util/Logging.h"

namespace stellar
{

BucketApplicator::BucketApplicator(LedgerEntries& entries,
                                   std::shared_ptr<const Bucket> bucket)
    : mEntries(entries), mBucket(bucket)
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
    soci::transaction sqlTx(mEntries.getDatabase().getSession());
    BucketEntry entry;
    while (mIn && mIn.readOne(entry))
    {
        LedgerHeader lh;
        if (entry.type() == LIVEENTRY)
        {
            EntryFrame live{entry.liveEntry()};
            if (entryExists(live.getKey(), mEntries.getDatabase()))
            {
                updateEntry(live.getEntry(), mEntries.getDatabase());
            }
            else
            {
                insertEntry(live.getEntry(), mEntries.getDatabase());
            }
        }
        else
        {
            deleteEntry(entry.deadEntry(), mEntries.getDatabase());
        }
        if ((++mSize & 0xff) == 0xff)
        {
            break;
        }
    }
    sqlTx.commit();
    mEntries.flushCache();
    mEntries.getDatabase().clearPreparedStatementCache();

    if (!mIn || (mSize & 0xfff) == 0xfff)
    {
        CLOG(INFO, "Bucket") << "Bucket-apply: committed " << mSize
                             << " entries";
    }
}
}
