// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketInputIterator.h"
#include "bucket/Bucket.h"
#include "xdr/Stellar-ledger.h"
#include <Tracy.hpp>

namespace stellar
{
/**
 * Helper class that reads from the file underlying a bucket, keeping the bucket
 * alive for the duration of its existence.
 */
template <typename T>
void
BucketInputIterator<T>::loadEntry()
{
    ZoneScoped;
    if (mIn.readOne(mEntry))
    {
        mEntryPtr = &mEntry;
        bool isMeta;
        if constexpr (std::is_same<T, BucketEntry>::value)
        {
            isMeta = mEntry.type() == METAENTRY;
        }
        else
        {
            isMeta = mEntry.type() == HA_METAENTRY;
        }

        if (isMeta)
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

            if constexpr (std::is_same<T, HotArchiveBucketEntry>::value)
            {
                if (mMetadata.ext.v() != 1 ||
                    mMetadata.ext.bucketListType() != HOT_ARCHIVE)
                {
                    throw std::runtime_error(
                        "Malformed bucket: META entry with incorrect bucket "
                        "list type.");
                }
            }

            mSeenMetadata = true;
            loadEntry();
        }
        else
        {
            mSeenOtherEntries = true;
            if (mSeenMetadata)
            {
                if constexpr (std::is_same_v<T, LiveBucket>)
                {
                    LiveBucket::checkProtocolLegality(mEntry,
                                                      mMetadata.ledgerVersion);
                }
            }
        }
    }
    else
    {
        mEntryPtr = nullptr;
    }
}

template <typename T>
std::streamoff
BucketInputIterator<T>::pos()
{
    return mIn.pos();
}

template <typename T>
size_t
BucketInputIterator<T>::size() const
{
    return mIn.size();
}

template <typename T> BucketInputIterator<T>::operator bool() const
{
    return mEntryPtr != nullptr;
}

template <typename T>
typename BucketInputIterator<T>::BucketEntryT const&
BucketInputIterator<T>::operator*()
{
    return *mEntryPtr;
}

template <typename T>
bool
BucketInputIterator<T>::seenMetadata() const
{
    return mSeenMetadata;
}

template <typename T>
BucketMetadata const&
BucketInputIterator<T>::getMetadata() const
{
    return mMetadata;
}

template <typename T>
BucketInputIterator<T>::BucketInputIterator(std::shared_ptr<T const> bucket)
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
        mIn.open(mBucket->getFilename().string());
        loadEntry();
    }
}

template <typename T> BucketInputIterator<T>::~BucketInputIterator()
{
    mIn.close();
}

template <typename T>
BucketInputIterator<T>&
BucketInputIterator<T>::operator++()
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

template <typename T>
void
BucketInputIterator<T>::seek(std::streamoff offset)
{
    mIn.seek(offset);
    loadEntry();
}

template class BucketInputIterator<LiveBucket>;
template class BucketInputIterator<HotArchiveBucket>;
}
