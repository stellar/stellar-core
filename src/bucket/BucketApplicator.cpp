// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "bucket/BucketApplicator.h"
#include "bucket/Bucket.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "main/Application.h"
#include "util/Logging.h"
#include "util/types.h"

namespace stellar
{

BucketApplicator::BucketApplicator(Application& app,
                                   std::shared_ptr<const Bucket> bucket)
    : mApp(app), mBucketIter(bucket)
{
}

BucketApplicator::operator bool() const
{
    return (bool)mBucketIter;
}

size_t
BucketApplicator::pos()
{
    return mBucketIter.pos();
}

size_t
BucketApplicator::size() const
{
    return mBucketIter.size();
}

size_t
BucketApplicator::advance()
{
    size_t count = 0;

    LedgerTxn ltx(mApp.getLedgerTxnRoot(), false);
    for (; mBucketIter; ++mBucketIter)
    {
        if ((*mBucketIter).type() == LIVEENTRY)
        {
            auto const& bucketEntry = (*mBucketIter).liveEntry();
            auto key = LedgerEntryKey(bucketEntry);
            auto entry = ltx.load(key);
            if (entry)
            {
                entry.current() = bucketEntry;
            }
            else
            {
                ltx.create(bucketEntry);
            }
        }
        else
        {
            auto entry = ltx.load((*mBucketIter).deadEntry());
            if (entry)
            {
                entry.erase();
            }
        }

        if ((++count & 0xff) == 0xff)
        {
            break;
        }
    }
    ltx.commit();

    mCount += count;
    return count;
}
}
