// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketOutputIterator.h"
#include "bucket/Bucket.h"
#include "bucket/BucketManager.h"
#include "crypto/Random.h"
#include "util/GlobalChecks.h"
#include <Tracy.hpp>

namespace stellar
{

namespace
{

// TODO: Remove .v2 extension
std::filesystem::path
randomBucketName(std::string const& tmpDir, bool isExperimental)
{
    ZoneScoped;
    for (;;)
    {
        std::string name =
            tmpDir + "/tmp-bucket-" + binToHex(randomBytes(8)) + ".xdr";
        if (isExperimental)
        {
            name = Bucket::getExperimentalFilename(name);
        }

        std::ifstream ifile(name);
        if (!ifile)
        {
            return name;
        }
    }
}
}

bool
BucketOutputIterator::cmp(BucketEntry const& a, BucketEntry const& b) const
{
    if (mType == BucketSortOrder::SortByAccount)
    {
        return BucketEntryIdCmp<BucketSortOrder::SortByAccount>(a, b);
    }
    else
    {
        return BucketEntryIdCmp<BucketSortOrder::SortByType>(a, b);
    }
}

/**
 * Helper class that points to an output tempfile. Absorbs BucketEntries and
 * hashes them while writing to either destination. Produces a Bucket when done.
 */
BucketOutputIterator::BucketOutputIterator(std::string const& tmpDir,
                                           bool keepDeadEntries,
                                           BucketMetadata const& meta,
                                           MergeCounters& mc,
                                           asio::io_context& ctx, bool doFsync,
                                           BucketSortOrder type)
    : mFilename(
          randomBucketName(tmpDir, type == BucketSortOrder::SortByAccount))
    , mOut(ctx, doFsync)
    , mBuf(nullptr)
    , mKeepDeadEntries(keepDeadEntries)
    , mMeta(meta)
    , mMergeCounters(mc)
    , mType(type)
{
    ZoneScoped;
    CLOG_TRACE(Bucket, "BucketOutputIterator opening file to write: {}",
               mFilename);
    // Will throw if unable to open the file
    mOut.open(mFilename);

    if (protocolVersionStartsFrom(
            meta.ledgerVersion,
            Bucket::FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY))
    {
        BucketEntry bme;
        bme.type(METAENTRY);
        bme.metaEntry() = mMeta;
        put(bme);
        mPutMeta = true;
    }
}

void
BucketOutputIterator::put(BucketEntry const& e)
{
    ZoneScoped;
    Bucket::checkProtocolLegality(e, mMeta.ledgerVersion);
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

    // Check to see if there's an existing buffered entry.
    if (mBuf)
    {
        // cmp(e, *mBuf) means e < *mBuf; this should never be true since
        // it would mean that we're getting entries out of order.
        releaseAssert(!cmp(e, *mBuf));

        // Check to see if the new entry should flush (greater identity), or
        // merely replace (same identity), the buffered entry.
        if (cmp(*mBuf, e))
        {
            ++mMergeCounters.mOutputIteratorActualWrites;
            mOut.writeOne(*mBuf, &mHasher, &mBytesPut);
            mObjectsPut++;
        }
    }
    else
    {
        mBuf = std::make_unique<BucketEntry>();
    }

    // In any case, replace *mBuf with e.
    ++mMergeCounters.mOutputIteratorBufferUpdates;
    *mBuf = e;
}

std::shared_ptr<Bucket>
BucketOutputIterator::getBucket(BucketManager& bucketManager,
                                MergeKey* mergeKey,
                                BucketOutputIterator* auxFileIter)
{
    ZoneScoped;
    this->close();
    if (auxFileIter)
    {
        releaseAssert(auxFileIter->mType != this->mType);
        auxFileIter->close();
    }

    if (mObjectsPut == 0 || mBytesPut == 0)
    {
        if (mergeKey)
        {
            bucketManager.noteEmptyMergeOutput(*mergeKey);
        }
        return std::make_shared<Bucket>();
    }

    auto b = bucketManager.adoptFileAsBucket(
        mFilename, mHasher.finish(), mObjectsPut, mBytesPut, mType, mergeKey);

    if (auxFileIter)
    {
        // TODO: Calculate hash
        uint256 hash = b->getHash();
        bucketManager.addFileToBucket(b, auxFileIter->getFilename(),
                                      /*hash=*/hash, auxFileIter->mType);
    }

    return b;
}

void
BucketOutputIterator::close()
{
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
        std::remove(mFilename.c_str());
    }
}

// TODO: Make this std::filesystem::path
std::string
BucketOutputIterator::getFilename() const
{
    return mFilename;
}

bool
BucketOutputIterator::empty() const
{
    return mObjectsPut == 0 || mBytesPut == 0;
}
}
