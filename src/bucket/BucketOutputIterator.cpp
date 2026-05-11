// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketOutputIterator.h"
#include "bucket/BucketIndexUtils.h"
#include "bucket/BucketManager.h"
#include "bucket/HotArchiveBucket.h"
#include "bucket/LiveBucket.h"
#include "bucket/LiveBucketIndex.h"
#include "ledger/LedgerTypeUtils.h"
#include "util/GlobalChecks.h"
#include "util/ProtocolVersion.h"
#include <Tracy.hpp>
#include <filesystem>

namespace stellar
{

/**
 * Helper class that points to an output tempfile. Absorbs BucketEntries and
 * hashes them while writing to either destination. Produces a Bucket when done.
 */
template <IsBucketType BucketT>
BucketOutputIterator<BucketT>::BucketOutputIterator(std::string const& tmpDir,
                                                    bool keepTombstoneEntries,
                                                    BucketMetadata const& meta,
                                                    MergeCounters& mc,
                                                    asio::io_context& ctx,
                                                    bool doFsync)
    : mFilename(BucketT::randomBucketName(tmpDir))
    , mOut(ctx, doFsync)
    , mCtx(ctx)
    , mBuf(nullptr)
    , mKeepTombstoneEntries(keepTombstoneEntries)
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
            static_assert(std::is_same_v<BucketT, HotArchiveBucket>,
                          "unexpected bucket type");
            releaseAssertOrThrow(protocolVersionStartsFrom(
                meta.ledgerVersion,
                BucketT::FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION));

            HotArchiveBucketEntry bme;
            bme.type(HOT_ARCHIVE_METAENTRY);
            bme.metaEntry() = mMeta;
            releaseAssertOrThrow(bme.metaEntry().ext.v() == 1);
            put(bme);
        }

        mPutMeta = true;
    }
}

template <IsBucketType BucketT>
void
BucketOutputIterator<BucketT>::put(typename BucketT::EntryT const& e)
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

        if (!mKeepTombstoneEntries && BucketT::isTombstoneEntry(e))
        {
            ++mMergeCounters.mOutputIteratorTombstoneElisions;
            return;
        }
    }
    else
    {
        static_assert(std::is_same_v<BucketT, HotArchiveBucket>,
                      "unexpected bucket type");
        if (e.type() == HOT_ARCHIVE_METAENTRY)
        {
            if (mPutMeta)
            {
                throw std::runtime_error(
                    "putting META entry in bucket after initial entry");
            }
        }
        else
        {
            if (e.type() == HOT_ARCHIVE_ARCHIVED)
            {
                if (!isSorobanEntry(e.archivedEntry().data))
                {
                    throw std::runtime_error(
                        "putting non-soroban entry in hot archive bucket");
                }
            }
            else
            {
                if (!isSorobanEntry(e.key()))
                {
                    throw std::runtime_error(
                        "putting non-soroban entry in hot archive bucket");
                }
            }
        }

        // HOT_ARCHIVE_LIVE entries are dropped in the last bucket level
        // (similar to DEADENTRY) on live BucketLists
        if (!mKeepTombstoneEntries && BucketT::isTombstoneEntry(e))
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
        mBuf = std::make_unique<typename BucketT::EntryT>();
    }

    // In any case, replace *mBuf with e.
    ++mMergeCounters.mOutputIteratorBufferUpdates;
    *mBuf = e;
}

template <IsBucketType BucketT>
std::shared_ptr<BucketT>
BucketOutputIterator<BucketT>::getBucket(
    BucketManager& bucketManager, MergeKey* mergeKey,
    std::unique_ptr<std::vector<typename BucketT::EntryT>> inMemoryState)
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
            bucketManager.noteEmptyMergeOutput<BucketT>(*mergeKey);
        }
        return std::make_shared<BucketT>();
    }

    auto hash = mHasher.finish();

    // Check if a bucket with this hash already exists, and if so, grab a
    // shared_ptr to its index. This prevents a race condition where GC could
    // free the index between our check and adoptFileAsBucket:
    //
    // 1. Background merge produces bucket with hash X
    // 2. An existing bucket X may exist in BucketManager but not be in the
    //    BucketList (e.g., it left level 0 and will re-enter at level 2)
    // 3. GC on main thread sees use_count==1 and bucket not in BucketList,
    //    so it frees the index
    // 4. adoptFileAsBucket returns the existing bucket, but index is gone
    //
    // By grabbing the index as a shared_ptr here, we hold a reference that
    // prevents GC from freeing it. If the bucket doesn't exist or isn't
    // indexed, we create a new index. Note that we're not worried about GC
    // deleting the actual Bucket file, as merge creates a temp file regardless
    // of existence and does an atomic rename as part of adopt.
    std::shared_ptr<typename BucketT::IndexT const> index{};
    if (auto existingBucket = bucketManager.getBucketIfExists<BucketT>(hash);
        existingBucket)
    {
        index = BucketT::maybeGetIndexForMerge(existingBucket);
    }

    if (!index)
    {
        if constexpr (std::is_same_v<BucketT, LiveBucket>)
        {
            if (inMemoryState)
            {
                index = std::make_shared<LiveBucketIndex>(
                    bucketManager, *inMemoryState, mMeta);
            }
        }

        if (!index)
        {
            index = createIndex<BucketT>(bucketManager, mFilename, hash, mCtx,
                                         nullptr);
        }
    }

    if constexpr (std::is_same_v<BucketT, LiveBucket>)
    {
        return bucketManager.adoptFileAsBucket<BucketT>(
            mFilename.string(), hash, mergeKey, std::move(index),
            std::move(inMemoryState));
    }
    else
    {
        // HotArchiveBucket does not use in-memory state
        return bucketManager.adoptFileAsBucket<BucketT>(
            mFilename.string(), hash, mergeKey, std::move(index), nullptr);
    }
}

template class BucketOutputIterator<LiveBucket>;
template class BucketOutputIterator<HotArchiveBucket>;
}
