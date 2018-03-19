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
            auto key = LedgerEntryKey(entry.liveEntry());
            auto ler = ls.load(key);
            if (ler)
            {
                *ler->entry() = entry.liveEntry();
            }
            else
            {
                ls.create(entry.liveEntry());
            }
        }
        else
        {
            auto ler = ls.load(entry.deadEntry());
            if (ler)
            {
                ler->erase();
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
