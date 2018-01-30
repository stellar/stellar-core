// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketOutputIterator.h"
#include "bucket/Bucket.h"
#include "bucket/BucketManager.h"
#include "crypto/Random.h"
#include "util/make_unique.h"

namespace stellar
{

namespace
{
std::string
randomBucketName(std::string const& tmpDir)
{
    for (;;)
    {
        std::string name =
            tmpDir + "/tmp-bucket-" + binToHex(randomBytes(8)) + ".xdr";
        std::ifstream ifile(name);
        if (!ifile)
        {
            return name;
        }
    }
}
}

/**
 * Helper class that points to an output tempfile. Absorbs BucketEntries and
 * hashes them while writing to either destination. Produces a Bucket when done.
 */
BucketOutputIterator::BucketOutputIterator(std::string const& tmpDir,
                                           bool keepDeadEntries)
    : mFilename(randomBucketName(tmpDir))
    , mBuf(nullptr)
    , mHasher(SHA256::create())
    , mKeepDeadEntries(keepDeadEntries)
{
    CLOG(TRACE, "Bucket") << "BucketOutputIterator opening file to write: "
                          << mFilename;
    mOut.open(mFilename);
}

void
BucketOutputIterator::put(BucketEntry const& e)
{
    if (!mKeepDeadEntries && e.type() == DEADENTRY)
    {
        return;
    }

    // Check to see if there's an existing buffered entry.
    if (mBuf)
    {
        // mCmp(e, *mBuf) means e < *mBuf; this should never be true since
        // it would mean that we're getting entries out of order.
        assert(!mCmp(e, *mBuf));

        // Check to see if the new entry should flush (greater identity), or
        // merely replace (same identity), the buffered entry.
        if (mCmp(*mBuf, e))
        {
            mOut.writeOne(*mBuf, mHasher.get(), &mBytesPut);
            mObjectsPut++;
        }
    }
    else
    {
        mBuf = make_unique<BucketEntry>();
    }

    // In any case, replace *mBuf with e.
    *mBuf = e;
}

std::shared_ptr<Bucket>
BucketOutputIterator::getBucket(BucketManager& bucketManager)
{
    assert(mOut);
    if (mBuf)
    {
        mOut.writeOne(*mBuf, mHasher.get(), &mBytesPut);
        mObjectsPut++;
        mBuf.reset();
    }

    mOut.close();
    if (mObjectsPut == 0 || mBytesPut == 0)
    {
        assert(mObjectsPut == 0);
        assert(mBytesPut == 0);
        CLOG(DEBUG, "Bucket") << "Deleting empty bucket file " << mFilename;
        std::remove(mFilename.c_str());
        return std::make_shared<Bucket>();
    }
    return bucketManager.adoptFileAsBucket(mFilename, mHasher->finish(),
                                           mObjectsPut, mBytesPut);
}
}
