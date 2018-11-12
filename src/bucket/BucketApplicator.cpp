// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "bucket/BucketApplicator.h"
#include "bucket/Bucket.h"
#include "ledger/LedgerState.h"
#include "ledger/LedgerStateEntry.h"
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

void
BucketApplicator::advance()
{
    LedgerState ls(mApp.getLedgerStateRoot(), false);
    for (; mBucketIter; ++mBucketIter)
    {
        if ((*mBucketIter).type() == LIVEENTRY)
        {
            auto const& bucketEntry = (*mBucketIter).liveEntry();
            auto key = LedgerEntryKey(bucketEntry);
            auto entry = ls.load(key);
            if (entry)
            {
                entry.current() = bucketEntry;
            }
            else
            {
                ls.create(bucketEntry);
            }
        }
        else
        {
            auto entry = ls.load((*mBucketIter).deadEntry());
            if (entry)
            {
                entry.erase();
            }
        }
        if ((++mSize & 0xff) == 0xff)
        {
            break;
        }
    }
    ls.commit();

    if (!mBucketIter || (mSize & 0xfff) == 0xfff)
    {
        CLOG(INFO, "Bucket")
            << "Bucket-apply: committed " << mSize << " entries";
    }
}
}
