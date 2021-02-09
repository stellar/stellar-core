// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketInputIterator.h"
#include "bucket/Bucket.h"
#include <Tracy.hpp>

namespace stellar
{
/**
 * Helper class that reads from the file underlying a bucket, keeping the bucket
 * alive for the duration of its existence.
 */
void
BucketInputIterator::loadEntry()
{
    ZoneScoped;
    if (mIn.readOne(mEntry))
    {
        mEntryPtr = &mEntry;
        if (mEntry.type() == METAENTRY)
        {
            // There should only be one METAENTRY in the input stream
            // and it should be the first record.
            if (mSeenMetadata)
            {
                throw std::runtime_error(
                    "Malformed bucket: multiple META entries.");
            }
            if (mSeenOtherEntries)
            {
                throw std::runtime_error(
                    "Malformed bucket: META after other entries.");
            }
            mMetadata = mEntry.metaEntry();
            mSeenMetadata = true;
            loadEntry();
        }
        else
        {
            mSeenOtherEntries = true;
            if (mSeenMetadata)
            {
                Bucket::checkProtocolLegality(mEntry, mMetadata.ledgerVersion);
            }
        }
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

bool
BucketInputIterator::seenMetadata() const
{
    return mSeenMetadata;
}

BucketMetadata const&
BucketInputIterator::getMetadata() const
{
    return mMetadata;
}

BucketInputIterator::BucketInputIterator(std::shared_ptr<Bucket const> bucket)
    : mBucket(bucket), mEntryPtr(nullptr), mSeenMetadata(false)
{
    // In absence of metadata, we treat every bucket as though it is from ledger
    // protocol 0, which is the protocol of the genesis ledger. At very least
    // some empty buckets and the bucket containing the initial genesis account
    // entry really _are_ from protocol 0, so it's a lower bound for the real
    // protocol of a pre-protocol-11 bucket, and we have to use as conservative
    // a default as possible to avoid spurious attempted-downgrade errors.
    mMetadata.ledgerVersion = 0;
    if (!mBucket->getFilename().empty())
    {
        CLOG_TRACE(Bucket, "BucketInputIterator opening file to read: {}",
                   mBucket->getFilename());
        mIn.open(mBucket->getFilename());
        loadEntry();
    }
}

BucketInputIterator::~BucketInputIterator()
{
    mIn.close();
}

BucketInputIterator&
BucketInputIterator::operator++()
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
