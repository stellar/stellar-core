// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "bucket/BucketApplicator.h"
#include "bucket/Bucket.h"
#include "ledger/LedgerEntryReference.h"
#include "ledger/LedgerState.h"
#include "main/Application.h"
#include "util/Logging.h"

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
    LedgerState ls(mApp.getLedgerStateRoot());
    for (; mBucketIter; ++mBucketIter)
    {
        auto const& entry = *mBucketIter;
        if (entry.type() == LIVEENTRY)
        {
            try
            {
                auto key = LedgerEntryKey(entry.liveEntry());
                *ls.load(key)->entry() = entry.liveEntry();
            }
            catch (std::runtime_error& e)
            {
                ls.create(entry.liveEntry());
            }
        }
        else
        {
            try
            {
                ls.load(entry.deadEntry())->erase();
            }
            catch (std::runtime_error& e)
            {
            }
        }
        if ((++mSize & 0xff) == 0xff)
        {
            break;
        }
    }
    ls.commit();
    mApp.getDatabase().clearPreparedStatementCache();

    if (!mBucketIter || (mSize & 0xfff) == 0xfff)
    {
        CLOG(INFO, "Bucket")
            << "Bucket-apply: committed " << mSize << " entries";
    }
}
}
