// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"

#include "BucketList.h"
#include "main/Application.h"
#include "util/Logging.h"
#include "crypto/SHA.h"
#include "crypto/Hex.h"
#include "crypto/Random.h"
#include "util/XDRStream.h"
#include "clf/CLFMaster.h"
#include "clf/LedgerCmp.h"
#include <cassert>

namespace stellar
{

static std::string
randomBucketName(std::string const& tmpDir)
{
    while (true)
    {
        std::string name = tmpDir + "/bucket-" + binToHex(randomBytes(8)) + ".xdr";
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

/**
 * This ctor will spill to a file if it was given too many entries. This is
 * where the _first_ spill decision is made; subsequent spilled buckets are made
 * as the output of a merge with an already-spilled bucket as an input.
 */
Bucket::Bucket(std::string const& tmpDir,
               std::vector<CLFEntry> const& entries, uint256 const& hash)
    : mSpilledToFile(entries.size() > kMaxMemoryObjectsPerBucket)
    , mEntries(mSpilledToFile ? std::vector<CLFEntry>() : entries)
    , mHash(hash)
    , mFilename(mSpilledToFile ? randomBucketName(tmpDir) : std::string(""))
{
    if (mSpilledToFile)
    {
        XDROutputFileStream out;
        out.open(mFilename);
        LOG(DEBUG) << "Bucket spilling " << entries.size()
                   << " CLFEntries to file "
                   << mFilename;
        for (auto const& e : entries)
        {
            if (!out.writeOne(e))
            {
                throw std::runtime_error("failed writing XDR to bucket");
            }
        }
    }
}

Bucket::Bucket(std::string const& filename, uint256 const& hash)
    : mSpilledToFile(true), mHash(hash), mFilename(filename)
{
}

Bucket::~Bucket()
{
    if (!mFilename.empty())
    {
        std::remove(mFilename.c_str());
    }
}

Bucket::Bucket()
    : mSpilledToFile(false)
{
}

std::vector<CLFEntry> const&
Bucket::getEntries() const
{
    return mEntries;
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

bool
Bucket::isSpilledToFile() const
{
    return mSpilledToFile;
}


/**
 * Helper class that either reads from to an input file or an in-memory
 * vector iterator, depending on the underlying Bucket it's attached to.
 */
class
Bucket::InputIterator
{
    std::shared_ptr<Bucket const> mBucket;

    // Validity and current-value of the iterator is funneled into a pointer. If
    // non-null, it either points to mEntry, or to the referent of mVecIter.
    CLFEntry const* mEntryPtr;

    std::vector<CLFEntry>::const_iterator mVecIter;
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
        if (mBucket->mSpilledToFile)
        {
            LOG(DEBUG) << "Bucket::InputIterator opening file to read: "
                       << mBucket->mFilename;
            mIn.open(mBucket->mFilename);
            loadEntry();
        }
        else
        {
            mVecIter = mBucket->mEntries.begin();
            if (mVecIter != mBucket->mEntries.end())
            {
                mEntryPtr = &(*mVecIter);
            }
        }
    }

    ~InputIterator()
    {
        if (mBucket->mSpilledToFile)
        {
            mIn.close();
        }
    }

    InputIterator& operator++()
    {
        if (mBucket->mSpilledToFile && mIn)
        {
            loadEntry();
        }
        else if (++mVecIter != mBucket->mEntries.end())
        {
            mEntryPtr = &(*mVecIter);
        }
        else
        {
            mEntryPtr = nullptr;
        }
        return *this;
    }
};

/**
 * Helper class that either points to an output file or an in-memory
 * vector of CLFEntries. Absorbs CLFEntries and hashes them while
 * writing to either destination. Produces a Bucket when done.
 */
class
Bucket::OutputIterator
{
    bool mWriteToFile;
    std::string mFilename;
    std::vector<CLFEntry> mEntries;
    XDROutputFileStream mOut;
    SHA512_256 mHasher;

public:

    OutputIterator(std::string const& tmpDir, bool writeToFile)
        : mWriteToFile(writeToFile)
    {
        if (mWriteToFile)
        {
            mFilename = randomBucketName(tmpDir);
            LOG(DEBUG) << "Bucket::OutputIterator opening file to write: "
                       << mFilename;
            mOut.open(mFilename);
        }
    }

    void
    put(CLFEntry const& e)
    {
        mHasher.add(e.hash);
        if (mWriteToFile)
        {
            mOut.writeOne(e);
        }
        else
        {
            mEntries.emplace_back(e);
        }
    }

    std::shared_ptr<Bucket>
    getBucket(std::string const& tmpDir)
    {
        if (mWriteToFile)
        {
            return std::make_shared<Bucket>(mFilename, mHasher.finish());
        }
        else
        {
            return std::make_shared<Bucket>(tmpDir, mEntries, mHasher.finish());
        }
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

std::shared_ptr<Bucket>
Bucket::fresh(std::string const& tmpDir,
              std::vector<LedgerEntry> const& liveEntries,
              std::vector<LedgerKey> const& deadEntries)
{
    std::vector<CLFEntry> live, dead, combined;
    live.reserve(liveEntries.size());
    dead.reserve(deadEntries.size());

    for (auto const& e : liveEntries)
    {
        CLFEntry ce;
        ce.entry.type(LIVEENTRY);
        ce.entry.liveEntry() = e;
        ce.hash = sha256(xdr::xdr_to_msg(ce.entry));
        live.push_back(ce);
    }

    for (auto const& e : deadEntries)
    {
        CLFEntry ce;
        ce.entry.type(DEADENTRY);
        ce.entry.deadEntry() = e;
        ce.hash = sha256(xdr::xdr_to_msg(ce.entry));
        dead.push_back(ce);
    }

    std::sort(live.begin(), live.end(),
              CLFEntryIdCmp());

    std::sort(dead.begin(), dead.end(),
              CLFEntryIdCmp());

    uint256 dummyHash;
    auto liveBucket = std::make_shared<Bucket>(tmpDir, live, dummyHash);
    auto deadBucket = std::make_shared<Bucket>(tmpDir, dead, dummyHash);
    return Bucket::merge(tmpDir, liveBucket, deadBucket);
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
Bucket::merge(std::string const& tmpDir,
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

    Bucket::OutputIterator out(tmpDir,
                               oldBucket->isSpilledToFile() ||
                               newBucket->isSpilledToFile());

    SHA512_256 hsh;
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
    return out.getBucket(tmpDir);
}

BucketLevel::BucketLevel(size_t i)
    : mLevel(i)
    , mCurr(std::make_shared<Bucket>())
    , mSnap(std::make_shared<Bucket>())
{
}

uint256
BucketLevel::getHash() const
{
    SHA512_256 hsh;
    hsh.add(mCurr->getHash());
    hsh.add(mSnap->getHash());
    return hsh.finish();
}

std::shared_ptr<Bucket>
BucketLevel::getCurr() const
{
    return mCurr;
}

std::shared_ptr<Bucket>
BucketLevel::getSnap() const
{
    return mSnap;
}

void
BucketLevel::commit()
{
    if (mNextCurr.valid())
    {
        // NB: This might block if the worker thread is slow; might want to
        // use mNextCurr.wait_for(
        mCurr = mNextCurr.get();
        // LOG(DEBUG) << "level " << mLevel << " set mCurr to "
        //            << mCurr->getEntries().size() << " elements";
    }
    assert(!mNextCurr.valid());
}

void
BucketLevel::prepare(Application& app, uint64_t currLedger,
                     std::shared_ptr<Bucket> snap,
                     std::vector<std::shared_ptr<Bucket>> const& shadows)
{
    // If more than one absorb is pending at the same time, we have a logic
    // error in our caller (and all hell will break loose).
    assert(!mNextCurr.valid());

    auto curr = mCurr;

    // Subtle: We're "preparing the next state" of this level's mCurr, which is
    // *either* mCurr merged with snap, or else just snap (if mCurr is going to
    // be snapshotted itself in the next spill). This second condition happens
    // when currLedger is one multiple of the previous levels's spill-size away
    // from a snap of its own.  Eg. level 1 at ledger 120 (8 away from
    // 128, its next snap), or level 2 at ledger 1920 (128 away from 2048, its
    // next snap).
    if (mLevel > 0)
    {
        uint64_t nextChangeLedger =
            currLedger + BucketList::levelHalf(mLevel - 1);
        if (BucketList::levelShouldSpill(nextChangeLedger, mLevel))
        {
            // LOG(DEBUG) << "level " << mLevel
            //            << " skipping pending-snapshot curr";
            curr.reset();
        }
    }

    // LOG(DEBUG) << "level " << mLevel << " preparing merge of mCurr="
    //            << (curr ? curr->getEntries().size() : 0) << " with snap="
    //            << snap->getEntries().size() << " elements";
    std::string tmpDir = app.getCLFMaster().getTmpDir();
    using task_t = std::packaged_task<std::shared_ptr<Bucket>()>;
    std::shared_ptr<task_t> task =
        std::make_shared<task_t>(
            [curr, snap, tmpDir, shadows]()
            {
                // LOG(DEBUG)
                //<< "Worker merging " <<
                // snap->getEntries().size()
                //<< " new elements with " <<
                // curr->getEntries().size()
                //<< " existing";
                // TIMED_SCOPE(timer, "merge + hash");
                auto res = Bucket::merge(tmpDir,
                                         (curr ? curr :
                                          std::make_shared<Bucket>()),
                                         snap, shadows);
                // LOG(DEBUG)
                //<< "Worker finished merging " <<
                // snap->getEntries().size()
                //<< " new elements with " <<
                // curr->getEntries().size()
                //<< " existing (new size: " <<
                // res->getEntries().size() << ")";
                return res;
            });

    mNextCurr = task->get_future();
    app.getWorkerIOService().post(bind(&task_t::operator(), task));

    assert(mNextCurr.valid());
}

std::shared_ptr<Bucket>
BucketLevel::snap()
{
    mSnap = mCurr;
    mCurr = std::make_shared<Bucket>();
    // LOG(DEBUG) << "level " << mLevel << " set mSnap to "
    //            << mSnap->getEntries().size() << " elements";
    // LOG(DEBUG) << "level " << mLevel << " reset mCurr to "
    //            << mCurr->getEntries().size() << " elements";
    return mSnap;
}

uint64_t
BucketList::levelSize(size_t level)
{
    return 1ULL << (4 * (static_cast<uint64_t>(level) + 1));
}

uint64_t
BucketList::levelHalf(size_t level)
{
    return levelSize(level) >> 1;
}

uint64_t
BucketList::mask(uint64_t v, uint64_t m)
{
    return v & ~(m - 1);
}

size_t
BucketList::numLevels(uint64_t ledger)
{
    // Multiply ledger by 2 first, because we want the level-number to increment
    // as soon as we're at the _half way_ point for each level.
    ledger <<= 1;
    size_t i = 0;
    while (ledger)
    {
        i += 1;
        ledger >>= 4;
    }
    assert(i <= 16);
    return i;
}

uint256
BucketList::getHash() const
{
    SHA512_256 hsh;
    for (auto const& lev : mLevels)
    {
        hsh.add(lev.getHash());
    }
    return hsh.finish();
}

bool
BucketList::levelShouldSpill(uint64_t ledger, size_t level)
{
    return (ledger == mask(ledger, levelHalf(level)) ||
            ledger == mask(ledger, levelSize(level)));
}

size_t
BucketList::numLevels() const
{
    return mLevels.size();
}

BucketLevel const&
BucketList::getLevel(size_t i) const
{
    return mLevels.at(i);
}

void
BucketList::addBatch(Application& app, uint64_t currLedger,
                     std::vector<LedgerEntry> const& liveEntries,
                     std::vector<LedgerKey> const& deadEntries)
{
    assert(currLedger > 0);
    assert(numLevels(currLedger - 1) == mLevels.size());
    size_t n = numLevels(currLedger);
    // LOG(DEBUG) << "numlevels(" << currLedger << ") = " << n;
    if (mLevels.size() < n)
    {
        // LOG(DEBUG) << "adding level!";
        assert(n == mLevels.size() + 1);
        mLevels.push_back(BucketLevel(n - 1));
    }

    std::vector<std::shared_ptr<Bucket>> shadows;
    for (auto& level : mLevels)
    {
        shadows.push_back(level.getCurr());
        shadows.push_back(level.getSnap());
    }

    for (size_t i = mLevels.size() - 1; i > 0; --i)
    {
        // We are counting-down from the highest-numbered level (the
        // oldest/largest level) to the lowest (youngest); each step we check
        // for shadows in all the levels _above_ us, which means we pop the
        // current level's curr and snap buckets off the shadow list before
        // proceeding.
        assert(shadows.size() >= 2);
        shadows.pop_back();
        shadows.pop_back();

        /*
        LOG(DEBUG) << "curr=" << currLedger
                   << ", half(i-1)=" << levelHalf(i-1)
                   << ", size(i-1)=" << levelSize(i-1)
                   << ", mask(curr,half)=" << mask(currLedger, levelHalf(i-1))
                   << ", mask(curr,size)=" << mask(currLedger, levelSize(i-1));
        */
        if (levelShouldSpill(currLedger, i - 1))
        {
            /**
             * At every ledger, level[0] prepares the new batch and commits
             * it.
             *
             * At ledger multiples of 8, level[0] snaps, level[1] commits
             * existing and prepares the new level[0] snap
             *
             * At ledger multiples of 128, level[1] snaps, level[2] commits
             * existing and prepares the new level[1] snap
             *
             * All these have to be done in _reverse_ order (counting down
             * levels) because we want a 'curr' to be pulled out of the way into
             * a 'snap' the moment it's half-a-level full, not have anything
             * else spilled/added to it.
             */
            auto snap = mLevels[i - 1].snap();
            // LOG(DEBUG) << "Ledger " << currLedger
            //           << " causing commit on level " << i
            //           << " and prepare of "
            //           << snap->getEntries().size()
            //           << " element snap from level " << i-1
            //           << " to level " << i;
            mLevels[i].commit();
            mLevels[i].prepare(app, currLedger, snap, shadows);
        }
    }

    assert(shadows.size() == 2);
    shadows.pop_back();
    shadows.pop_back();
    mLevels[0].prepare(app, currLedger,
                       Bucket::fresh(app.getCLFMaster().getTmpDir(),
                                     liveEntries, deadEntries),
                       shadows);
    mLevels[0].commit();
}
}
