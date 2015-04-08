// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/Bucket.h"
// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"
#include "bucket/BucketManager.h"
#include "bucket/LedgerCmp.h"
#include "crypto/Hex.h"
#include "crypto/Random.h"
#include "crypto/SHA.h"
#include "main/Application.h"
#include "util/Fs.h"
#include "util/Logging.h"
#include "util/TmpDir.h"
#include "util/XDRStream.h"
#include "xdrpp/message.h"
#include "database/Database.h"
#include "ledger/EntryFrame.h"
#include "ledger/LedgerDelta.h"
#include "medida/medida.h"
#include <cassert>
#include <future>

namespace stellar
{

static std::string
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

LedgerKey
LedgerEntryKey(LedgerEntry const& e)
{
    LedgerKey k;
    switch (e.type())
    {

    case ACCOUNT:
        k.type(ACCOUNT);
        k.account().accountID = e.account().accountID;
        break;

    case TRUSTLINE:
        k.type(TRUSTLINE);
        k.trustLine().accountID = e.trustLine().accountID;
        k.trustLine().currency = e.trustLine().currency;
        break;

    case OFFER:
        k.type(OFFER);
        k.offer().accountID = e.offer().accountID;
        k.offer().offerID = e.offer().offerID;
        break;
    }
    return k;
}

Bucket::Bucket(std::string const& filename, uint256 const& hash)
    : mFilename(filename), mHash(hash)
{
    assert(filename.empty() || fs::exists(filename));
    if (!filename.empty())
    {
        CLOG(TRACE, "Bucket")
            << "Bucket::Bucket() created, file exists : "
            << mFilename;
    }
}

Bucket::~Bucket()
{
    if (!mFilename.empty() && !mRetain)
    {
        CLOG(TRACE, "Bucket") << "Bucket::~Bucket removing file: "
                              << mFilename;
        std::remove(mFilename.c_str());
    }
}

Bucket::Bucket()
{
}

uint256 const&
Bucket::getHash() const
{
    return mHash;
}

std::string const&
Bucket::getFilename() const
{
    return mFilename;
}

void
Bucket::setRetain(bool r)
{
    mRetain = r;
}

/**
 * Helper class that reads from the file underlying a bucket, keeping the bucket
 * alive for the duration of its existence.
 */
class Bucket::InputIterator
{
    std::shared_ptr<Bucket const> mBucket;

    // Validity and current-value of the iterator is funneled into a pointer. If
    // non-null, it points to mEntry.
    BucketEntry const* mEntryPtr;
    XDRInputFileStream mIn;
    BucketEntry mEntry;

    void
    loadEntry()
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

  public:
    operator bool() const
    {
        return mEntryPtr != nullptr;
    }

    BucketEntry const& operator*()
    {
        return *mEntryPtr;
    }

    InputIterator(std::shared_ptr<Bucket const> bucket)
        : mBucket(bucket), mEntryPtr(nullptr)
    {
        if (!mBucket->mFilename.empty())
        {
            CLOG(TRACE, "Bucket") << "Bucket::InputIterator opening file to read: "
                               << mBucket->mFilename;
            mIn.open(mBucket->mFilename);
            loadEntry();
        }
    }

    ~InputIterator()
    {
        mIn.close();
    }

    InputIterator& operator++()
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
};

/**
 * Helper class that points to an output tempfile. Absorbs BucketEntries and
 * hashes them while writing to either destination. Produces a Bucket when done.
 */
class Bucket::OutputIterator
{
    std::string mFilename;
    XDROutputFileStream mOut;
    std::unique_ptr<SHA256> mHasher;
    size_t mBytesPut{0};
    size_t mObjectsPut{0};

  public:
    OutputIterator(std::string const& tmpDir)
        : mFilename(randomBucketName(tmpDir))
        , mHasher(SHA256::create())
    {
        CLOG(TRACE, "Bucket") << "Bucket::OutputIterator opening file to write: "
                           << mFilename;
        mOut.open(mFilename);
    }

    void
    put(BucketEntry const& e)
    {
        mOut.writeOne(e, mHasher.get(), &mBytesPut);
        mObjectsPut++;
    }

    std::shared_ptr<Bucket>
    getBucket(BucketManager& bucketManager)
    {
        assert(mOut);
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
};

bool
Bucket::containsBucketIdentity(BucketEntry const& id) const
{
    BucketEntryIdCmp cmp;
    Bucket::InputIterator iter(shared_from_this());
    while (iter)
    {
        if (!(cmp(*iter, id) || cmp(id, *iter)))
        {
            return true;
        }
        ++iter;
    }
    return false;
}

std::pair<size_t, size_t>
Bucket::countLiveAndDeadEntries() const
{
    size_t live = 0, dead = 0;
    Bucket::InputIterator iter(shared_from_this());
    while (iter)
    {
        if ((*iter).type() == LIVEENTRY)
        {
            ++live;
        }
        else
        {
            ++dead;
        }
        ++iter;
    }
    return std::make_pair(live, dead);
}

void
Bucket::apply(Database& db) const
{
    if (getFilename().empty())
    {
        return;
    }
    BucketEntry entry;
    LedgerHeader lh; // buckets, by definition are independent from the header
    LedgerDelta delta(lh);
    XDRInputFileStream in;
    in.open(getFilename());
    while (in)
    {
        in.readOne(entry);
        if (entry.type() == LIVEENTRY)
        {
            EntryFrame::pointer ep = EntryFrame::FromXDR(entry.liveEntry());
            ep->storeAddOrChange(delta, db);
        }
        else
        {
            EntryFrame::storeDelete(delta, db, entry.deadEntry());
        }
    }
}

std::shared_ptr<Bucket>
Bucket::fresh(BucketManager& bucketManager, std::vector<LedgerEntry> const& liveEntries,
              std::vector<LedgerKey> const& deadEntries)
{
    std::vector<BucketEntry> live, dead, combined;
    live.reserve(liveEntries.size());
    dead.reserve(deadEntries.size());

    for (auto const& e : liveEntries)
    {
        BucketEntry ce;
        ce.type(LIVEENTRY);
        ce.liveEntry() = e;
        live.push_back(ce);
    }

    for (auto const& e : deadEntries)
    {
        BucketEntry ce;
        ce.type(DEADENTRY);
        ce.deadEntry() = e;
        dead.push_back(ce);
    }

    std::sort(live.begin(), live.end(), BucketEntryIdCmp());

    std::sort(dead.begin(), dead.end(), BucketEntryIdCmp());

    OutputIterator liveOut(bucketManager.getTmpDir());
    OutputIterator deadOut(bucketManager.getTmpDir());
    for (auto const& e : live)
    {
        liveOut.put(e);
    }
    for (auto const& e : dead)
    {
        deadOut.put(e);
    }

    auto liveBucket = liveOut.getBucket(bucketManager);
    auto deadBucket = deadOut.getBucket(bucketManager);
    return Bucket::merge(bucketManager, liveBucket, deadBucket);
}

inline void
maybe_put(BucketEntryIdCmp const& cmp, Bucket::OutputIterator& out,
          Bucket::InputIterator& in,
          std::vector<Bucket::InputIterator>& shadowIterators)
{
    for (auto& si : shadowIterators)
    {
        // Advance the shadowIterator while it's less than the candidate
        while (si && cmp(*si, *in))
        {
            ++si;
        }
        // We have stepped si forward to the point that either si is exhausted,
        // or else *si >= *in; we now check the opposite direction to see if we
        // have equality.
        if (si && !cmp(*in, *si))
        {
            // If so, then *in is shadowed in at least one level and we will
            // not be doing a 'put'; we return early. There is no need to
            // advance
            // the other iterators, they will advance as and if necessary in
            // future
            // calls to maybe_put.
            return;
        }
    }
    // Nothing shadowed.
    out.put(*in);
}

std::shared_ptr<Bucket>
Bucket::merge(BucketManager& bucketManager, std::shared_ptr<Bucket> const& oldBucket,
              std::shared_ptr<Bucket> const& newBucket,
              std::vector<std::shared_ptr<Bucket>> const& shadows)
{
    // This is the key operation in the scheme: merging two (read-only)
    // buckets together into a new 3rd bucket, while calculating its hash,
    // in a single pass.

    assert(oldBucket);
    assert(newBucket);

    Bucket::InputIterator oi(oldBucket);
    Bucket::InputIterator ni(newBucket);

    std::vector<Bucket::InputIterator> shadowIterators(shadows.begin(),
                                                       shadows.end());

    auto timer = bucketManager.getMergeTimer().TimeScope();
    Bucket::OutputIterator out(bucketManager.getTmpDir());

    BucketEntryIdCmp cmp;
    while (oi || ni)
    {
        if (!ni)
        {
            // Out of new entries, take old entries.
            maybe_put(cmp, out, oi, shadowIterators);
            ++oi;
        }
        else if (!oi)
        {
            // Out of old entries, take new entries.
            maybe_put(cmp, out, ni, shadowIterators);
            ++ni;
        }
        else if (cmp(*oi, *ni))
        {
            // Next old-entry has smaller key, take it.
            maybe_put(cmp, out, oi, shadowIterators);
            ++oi;
        }
        else if (cmp(*ni, *oi))
        {
            // Next new-entry has smaller key, take it.
            maybe_put(cmp, out, ni, shadowIterators);
            ++ni;
        }
        else
        {
            // Old and new are for the same key, take new.
            maybe_put(cmp, out, ni, shadowIterators);
            ++oi;
            ++ni;
        }
    }
    return out.getBucket(bucketManager);
}
}
