// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketInputIterator.h"
#include "bucket/HotArchiveBucket.h"
#include "bucket/LiveBucket.h"
#include "xdr/Stellar-ledger.h"
#include <Tracy.hpp>
#include <type_traits>

namespace stellar
{
/**
 * Helper class that reads from the file underlying a bucket, keeping the bucket
 * alive for the duration of its existence.
 */
template <typename BucketT>
void
BucketInputIterator<BucketT>::loadEntry()
{
    ZoneScoped;
    if (mIn.readOne(mEntry))
    {
        mEntryPtr = &mEntry;
        bool isMeta;
        if constexpr (std::is_same_v<BucketT, LiveBucket>)
        {
            isMeta = mEntry.type() == METAENTRY;
        }
        else
        {
            static_assert(std::is_same_v<BucketT, HotArchiveBucket>,
                          "unexpected bucket type");
            isMeta = mEntry.type() == HOT_ARCHIVE_METAENTRY;
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

            if constexpr (std::is_same_v<BucketT, HotArchiveBucketEntry>)
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
                if constexpr (std::is_same_v<BucketT, LiveBucket>)
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

template <typename BucketT>
std::streamoff
BucketInputIterator<BucketT>::pos()
{
    return mIn.pos();
}

template <typename BucketT>
size_t
BucketInputIterator<BucketT>::size() const
{
    return mIn.size();
}

template <typename BucketT> BucketInputIterator<BucketT>::operator bool() const
{
    return mEntryPtr != nullptr;
}

template <typename BucketT>
typename BucketT::EntryT const&
BucketInputIterator<BucketT>::operator*()
{
    return *mEntryPtr;
}

template <typename BucketT>
bool
BucketInputIterator<BucketT>::seenMetadata() const
{
    return mSeenMetadata;
}

template <typename BucketT>
BucketMetadata const&
BucketInputIterator<BucketT>::getMetadata() const
{
    return mMetadata;
}

template <typename BucketT>
BucketInputIterator<BucketT>::BucketInputIterator(
    std::shared_ptr<BucketT const> bucket)
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

template <typename BucketT> BucketInputIterator<BucketT>::~BucketInputIterator()
{
    mIn.close();
}

template <typename BucketT>
BucketInputIterator<BucketT>&
BucketInputIterator<BucketT>::operator++()
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

template <typename BucketT>
void
BucketInputIterator<BucketT>::seek(std::streamoff offset)
{
    mIn.seek(offset);
    loadEntry();
}

template class BucketInputIterator<LiveBucket>;
template class BucketInputIterator<HotArchiveBucket>;
}
