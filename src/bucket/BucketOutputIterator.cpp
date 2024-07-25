// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketOutputIterator.h"
#include "bucket/Bucket.h"
#include "bucket/BucketIndex.h"
#include "bucket/BucketManager.h"
#include "util/GlobalChecks.h"
#include "util/ProtocolVersion.h"
#include "xdr/Stellar-ledger.h"
#include <Tracy.hpp>
#include <filesystem>

namespace stellar
{

/**
 * Helper class that points to an output tempfile. Absorbs BucketEntries and
 * hashes them while writing to either destination. Produces a Bucket when done.
 */
template <typename BucketT>
BucketOutputIterator<BucketT>::BucketOutputIterator(
    std::string const& tmpDir, bool keepDeadEntries, BucketMetadata const& meta,
    MergeCounters& mc, asio::io_context& ctx, bool doFsync)
    : mFilename(Bucket::randomBucketName(tmpDir))
    , mOut(ctx, doFsync)
    , mBuf(nullptr)
    , mKeepDeadEntries(keepDeadEntries)
    , mMeta(meta)
    , mMergeCounters(mc)
{
    ZoneScoped;
    CLOG_TRACE(Bucket, "BucketOutputIterator opening file to write: {}",
               mFilename);
    // Will throw if unable to open the file
    mOut.open(mFilename.string());

    if (protocolVersionStartsFrom(
            meta.ledgerVersion,
            LiveBucket::FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY))
    {

        if constexpr (std::is_same_v<BucketT, LiveBucket>)
        {
            BucketEntry bme;
            bme.type(METAENTRY);
            bme.metaEntry() = mMeta;
            put(bme);
        }
        else
        {
            releaseAssertOrThrow(protocolVersionStartsFrom(
                meta.ledgerVersion, ProtocolVersion::V_22));

            HotArchiveBucketEntry bme;
            bme.type(HA_METAENTRY);
            bme.metaEntry() = mMeta;
            put(bme);
        }

        mPutMeta = true;
    }
}

template <typename BucketT>
void
BucketOutputIterator<BucketT>::put(BucketEntryT const& e)
{
    ZoneScoped;

    if constexpr (std::is_same_v<BucketT, LiveBucket>)
    {
        LiveBucket::checkProtocolLegality(e, mMeta.ledgerVersion);
        if (e.type() == METAENTRY)
        {
            if (mPutMeta)
            {
                throw std::runtime_error(
                    "putting META entry in bucket after initial entry");
            }
        }

        if (!mKeepDeadEntries && e.type() == DEADENTRY)
        {
            ++mMergeCounters.mOutputIteratorTombstoneElisions;
            return;
        }
    }
    else
    {
        if (e.type() == HA_METAENTRY)
        {
            if (mPutMeta)
            {
                throw std::runtime_error(
                    "putting META entry in bucket after initial entry");
            }
        }

        // RESTORED entries are dropped in the last bucket level (similar to
        // DEADENTRY) on live BucketLists
        if (!mKeepDeadEntries && e.type() == HA_RESTORED)
        {
            ++mMergeCounters.mOutputIteratorTombstoneElisions;
            return;
        }
    }

    // Check to see if there's an existing buffered entry.
    if (mBuf)
    {
        // mCmp(e, *mBuf) means e < *mBuf; this should never be true since
        // it would mean that we're getting entries out of order.
        releaseAssert(!mCmp(e, *mBuf));

        // Check to see if the new entry should flush (greater identity), or
        // merely replace (same identity), the buffered entry.
        if (mCmp(*mBuf, e))
        {
            ++mMergeCounters.mOutputIteratorActualWrites;
            mOut.writeOne(*mBuf, &mHasher, &mBytesPut);
            mObjectsPut++;
        }
    }
    else
    {
        mBuf = std::make_unique<BucketEntryT>();
    }

    // In any case, replace *mBuf with e.
    ++mMergeCounters.mOutputIteratorBufferUpdates;
    *mBuf = e;
}

template <typename BucketT>
std::shared_ptr<BucketT>
BucketOutputIterator<BucketT>::getBucket(BucketManager& bucketManager,
                                         bool shouldSynchronouslyIndex,
                                         MergeKey* mergeKey)
{
    ZoneScoped;
    if (mBuf)
    {
        mOut.writeOne(*mBuf, &mHasher, &mBytesPut);
        mObjectsPut++;
        mBuf.reset();
    }

    mOut.close();
    if (mObjectsPut == 0 || mBytesPut == 0)
    {
        releaseAssert(mObjectsPut == 0);
        releaseAssert(mBytesPut == 0);
        CLOG_DEBUG(Bucket, "Deleting empty bucket file {}", mFilename);
        std::filesystem::remove(mFilename);
        if (mergeKey)
        {
            bucketManager.noteEmptyMergeOutput(*mergeKey);
        }
        return std::make_shared<BucketT>();
    }

    auto hash = mHasher.finish();
    std::unique_ptr<BucketIndex const> index{};

    // If this bucket needs to be indexed and is not already indexed
    if (shouldSynchronouslyIndex)
    {
        // either it's a new bucket or we just reconstructed a bucket
        // we already have, in any case ensure we have an index
        if (auto b = bucketManager.getBucketIfExists(hash);
            !b || !b->isIndexed())
        {
            index = BucketIndex::createIndex(bucketManager, mFilename, hash);
        }
    }

    if constexpr (std::is_same_v<BucketT, LiveBucket>)
    {
        return bucketManager.adoptFileAsLiveBucket(mFilename.string(), hash,
                                                   mergeKey, std::move(index));
    }
    else
    {
        // TODO:
        releaseAssert(false);
        return std::shared_ptr<HotArchiveBucket>();
    }
}

template class BucketOutputIterator<LiveBucket>;
template class BucketOutputIterator<HotArchiveBucket>;
}
