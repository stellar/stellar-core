// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketInputIterator.h"
#include "bucket/Bucket.h"

namespace stellar
{
/**
 * Helper class that reads from the file underlying a bucket, keeping the bucket
 * alive for the duration of its existence.
 */
void
BucketInputIterator::loadEntry()
{
    if (mIn.readOne(mEntry))
    {
        mEntryPtr = &mEntry;
    }
    else
    {
        mEntryPtr = nullptr;
    }
}

size_t
BucketInputIterator::pos()
{
    return mIn.pos();
}

size_t
BucketInputIterator::size() const
{
    return mIn.size();
}

BucketInputIterator::operator bool() const
{
    return mEntryPtr != nullptr;
}

BucketEntry const& BucketInputIterator::operator*()
{
    return *mEntryPtr;
}

BucketInputIterator::BucketInputIterator(std::shared_ptr<Bucket const> bucket)
    : mBucket(bucket), mEntryPtr(nullptr)
{
    if (!mBucket->getFilename().empty())
    {
        CLOG(TRACE, "Bucket") << "BucketInputIterator opening file to read: "
                              << mBucket->getFilename();
        mIn.open(mBucket->getFilename());
        loadEntry();
    }
}

BucketInputIterator::~BucketInputIterator()
{
    mIn.close();
}

BucketInputIterator& BucketInputIterator::operator++()
{
    if (mIn)
    {
        loadEntry();
    }
    else
    {
        mEntryPtr = nullptr;
    }
    return *this;
}
}
