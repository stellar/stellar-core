// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"

#include "clf/Bucket.h"
#include "clf/CLFMaster.h"
#include "clf/LedgerCmp.h"
#include "crypto/Hex.h"
#include "crypto/Random.h"
#include "crypto/SHA.h"
#include "main/Application.h"
#include "util/Logging.h"
#include "util/TmpDir.h"
#include "util/XDRStream.h"
#include "xdrpp/message.h"

#include <cassert>
#include <future>

namespace stellar
{

static std::string
randomBucketName(std::string const& tmpDir)
{
    while (true)
    {
        std::string name = tmpDir + "/tmp-bucket-" + binToHex(randomBytes(8)) + ".xdr";
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
            k.offer().sequence = e.offer().sequence;
            break;
        }
    return k;
}

Bucket::Bucket(std::string const& filename, uint256 const& hash)
    : mFilename(filename)
    , mHash(hash)
{
    assert(filename.empty() || TmpDir::exists(filename));
}

Bucket::~Bucket()
{
    if (!mFilename.empty())
    {
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

/**
 * Helper class that reads from the file underlying a bucket, keeping the bucket
 * alive for the duration of its existence.
 */
class
Bucket::InputIterator
{
    std::shared_ptr<Bucket const> mBucket;

    // Validity and current-value of the iterator is funneled into a pointer. If
    // non-null, it points to mEntry.
    CLFEntry const* mEntryPtr;
    XDRInputFileStream mIn;
    CLFEntry mEntry;

    void loadEntry()
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

    CLFEntry const& operator*()
    {
        return *mEntryPtr;
    }

    InputIterator(std::shared_ptr<Bucket const> bucket)
        : mBucket(bucket)
        , mEntryPtr(nullptr)
    {
        if (!mBucket->mFilename.empty())
        {
            CLOG(TRACE, "CLF") << "Bucket::InputIterator opening file to read: "
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
 * Helper class that points to an output tempfile. Absorbs CLFEntries and hashes
 * them while writing to either destination. Produces a Bucket when done.
 */
class
Bucket::OutputIterator
{
    std::string mFilename;
    XDROutputFileStream mOut;
    SHA256 mHasher;

public:

    OutputIterator(std::string const& tmpDir)
    {
        mFilename = randomBucketName(tmpDir);
        CLOG(TRACE, "CLF") << "Bucket::OutputIterator opening file to write: "
                           << mFilename;
        mOut.open(mFilename);
    }

    void
    put(CLFEntry const& e)
    {
        mOut.writeOne(e, &mHasher);
    }

    std::shared_ptr<Bucket>
    getBucket(CLFMaster& clfMaster)
    {
        assert(mOut);
        mOut.close();
        return clfMaster.adoptFileAsBucket(mFilename, mHasher.finish());
    }

};

bool
Bucket::containsCLFIdentity(CLFEntry const& id) const
{
    CLFEntryIdCmp cmp;
    Bucket::InputIterator iter(shared_from_this());
    while (iter)
    {
        if (! (cmp(*iter, id) || cmp(id, *iter)))
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
    while(iter)
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

std::shared_ptr<Bucket>
Bucket::fresh(CLFMaster& clfMaster,
              std::vector<LedgerEntry> const& liveEntries,
              std::vector<LedgerKey> const& deadEntries)
{
    std::vector<CLFEntry> live, dead, combined;
    live.reserve(liveEntries.size());
    dead.reserve(deadEntries.size());

    for (auto const& e : liveEntries)
    {
        CLFEntry ce;
        ce.type(LIVEENTRY);
        ce.liveEntry() = e;
        live.push_back(ce);
    }

    for (auto const& e : deadEntries)
    {
        CLFEntry ce;
        ce.type(DEADENTRY);
        ce.deadEntry() = e;
        dead.push_back(ce);
    }

    std::sort(live.begin(), live.end(),
              CLFEntryIdCmp());

    std::sort(dead.begin(), dead.end(),
              CLFEntryIdCmp());

    OutputIterator liveOut(clfMaster.getTmpDir());
    OutputIterator deadOut(clfMaster.getTmpDir());
    for (auto const& e : live)
    {
        liveOut.put(e);
    }
    for (auto const& e : dead)
    {
        deadOut.put(e);
    }

    auto liveBucket = liveOut.getBucket(clfMaster);
    auto deadBucket = deadOut.getBucket(clfMaster);
    return Bucket::merge(clfMaster, liveBucket, deadBucket);
}

inline void
maybe_put(CLFEntryIdCmp& cmp,
          Bucket::OutputIterator& out,
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
            // not be doing a 'put'; we return early. There is no need to advance
            // the other iterators, they will advance as and if necessary in future
            // calls to maybe_put.
            return;
        }
    }
    // Nothing shadowed.
    out.put(*in);
}

std::shared_ptr<Bucket>
Bucket::merge(CLFMaster& clfMaster,
              std::shared_ptr<Bucket> const& oldBucket,
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

    Bucket::OutputIterator out(clfMaster.getTmpDir());

    SHA256 hsh;
    CLFEntryIdCmp cmp;
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
    return out.getBucket(clfMaster);
}

}
