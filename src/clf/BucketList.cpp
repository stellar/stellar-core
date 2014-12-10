#include "BucketList.h"
#include "main/Application.h"
#include "lib/util/Logging.h"
#include <cassert>

namespace stellar
{

void
Hasher::update(uint8_t const* data, size_t len)
{
    size_t i = 0;
    while (len--)
    {
        mState[i] += *data;
        mState[i] ^= *data;
        ++data;
        ++i;
        i &= 0x1f;
    }
}

uint256
Hasher::finish() {
    return mState;
}

Bucket::Bucket(std::vector<Bucket::KVPair>&& entries,
               uint256&& hash)
    : mEntries(entries)
    , mHash(hash)
{}

Bucket::Bucket()
{}

std::vector<Bucket::KVPair> const&
Bucket::getEntries() const
{
    return mEntries;
}

uint256 const&
Bucket::getHash() const
{
    return mHash;
}

std::shared_ptr<Bucket>
Bucket::fresh(std::vector<Bucket::KVPair>&& entries) {

    std::sort(entries.begin(), entries.end(),
              [](KVPair const& a, KVPair const& b)
              {
                  return std::get<0>(a) < std::get<0>(b);
              });

    Hasher hsh;
    for (auto const& e : entries)
    {
        hsh.update(std::get<0>(e));
        hsh.update(std::get<1>(e));
    }
    return std::make_shared<Bucket>(std::move(entries), hsh.finish());
}


std::shared_ptr<Bucket>
Bucket::merge(std::shared_ptr<Bucket> const& oldBucket,
              std::shared_ptr<Bucket> const& newBucket)
{
    // This is the key operation in the scheme: merging two (read-only)
    // buckets together into a new 3rd bucket, while calculating its hash,
    // in a single pass.

    LOG(DEBUG) << "Starting merge of " << newBucket->mEntries.size()
               << " new elements with " << oldBucket->mEntries.size()
               << " existing";

    assert(oldBucket);
    assert(newBucket);

    std::vector<KVPair>::const_iterator oi = oldBucket->mEntries.begin();
    std::vector<KVPair>::const_iterator ni = newBucket->mEntries.begin();

    std::vector<KVPair>::const_iterator oe = oldBucket->mEntries.end();
    std::vector<KVPair>::const_iterator ne = newBucket->mEntries.end();

    std::vector<KVPair> out;
    out.reserve(oldBucket->mEntries.size() +
                newBucket->mEntries.size());
    Hasher hsh;
    while (oi != oe || ni != ne)
    {
        std::vector<KVPair>::const_iterator e;
        if (ni == ne)
        {
            // Out of new entries, take old entries.
            e = oi++;
        }
        else if (oi == oe)
        {
            // Out of old entries, take new entries.
            e = ni++;
        }
        else if (std::get<0>(*oi) < std::get<0>(*ni))
        {
            // Next old-entry has smaller key, take it.
            e = oi++;
        }
        else if (std::get<0>(*ni) < std::get<0>(*oi))
        {
            // Next new-entry has smaller key, take it.
            e = ni++;
        }
        else
        {
            // Old and new are for the same key, take new.
            e = ni++;
        }
        hsh.update(std::get<0>(*e));
        hsh.update(std::get<1>(*e));
        out.emplace_back(*e);
    }
    LOG(DEBUG) << "Finished merge of " << newBucket->mEntries.size()
               << " new elements with " << oldBucket->mEntries.size()
               << " existing (new size: " << out.size() << ")";
    return std::make_shared<Bucket>(std::move(out), hsh.finish());
}

BucketLevel::BucketLevel()
    : mCurr(std::make_shared<Bucket>())
    , mSnap(std::make_shared<Bucket>())
{}

uint256
BucketLevel::getHash() const
{
    Hasher hsh;
    hsh.update(mCurr->getHash());
    hsh.update(mSnap->getHash());
    return hsh.finish();
}

void
BucketLevel::commit()
{
    if (mNextCurr.valid())
    {
        // NB: This might block if the worker thread is slow; might want to
        // use mNextCurr.wait_for(
        mCurr = mNextCurr.get();
    }
    assert(!mNextCurr.valid());
}

void
BucketLevel::prepare(Application &app, std::shared_ptr<Bucket> snap)
{
    // If more than one absorb is pending at the same time, we have a logic
    // error in our caller (and all hell will break loose).
    assert(!mNextCurr.valid());

    auto curr = mCurr;

    using task_t = std::packaged_task<std::shared_ptr<Bucket>()>;
    std::shared_ptr<task_t> task = std::make_shared<task_t>(
        [curr, snap]()
        {
            return Bucket::merge(curr, snap);
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
    return mSnap;
}


uint64_t
BucketList::levelSize(uint64_t i)
{
    return 1ULL << (4 * (i+1));
}

uint64_t
BucketList::levelHalf(uint64_t i)
{
    return levelSize(i) >> 1;
}

uint64_t
BucketList::levelPrev(uint64_t i)
{
    assert(i > 0);
    return levelSize(i-1);
}

uint64_t
BucketList::mask(uint64_t v, uint64_t m)
{
    return v & ~(m-1);
}

size_t
BucketList::numLevels(uint64_t ledger)
{
    size_t i = 0;
    while (ledger) {
        i += 1;
        ledger >>= 4;
    }
    assert(i <= 16);
    return i;
}

uint256
BucketList::getHash() const
{
    Hasher hsh;
    for (auto const& lev : mLevels)
    {
        hsh.update(lev.getHash());
    }
    return hsh.finish();
}

void
BucketList::addBatch(Application &app, uint64_t currLedger,
                     std::vector<Bucket::KVPair>&& batch)
{
    assert(currLedger > 0);
    assert(numLevels(currLedger-1) == mLevels.size());
    size_t n = numLevels(currLedger);
    if (mLevels.size() < n)
    {
        assert(n == mLevels.size() + 1);
        mLevels.push_back(BucketLevel());
    }
    mLevels[0].prepare(app, Bucket::fresh(std::move(batch)));
    mLevels[0].commit();

    for (size_t i = 1; i < mLevels.size(); ++i)
    {
        if (currLedger == mask(currLedger, levelHalf(i)) ||
            currLedger == mask(currLedger, levelSize(i)))
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
             */
            LOG(DEBUG) << "Ledger " << currLedger
                       << " causing commit on level " << i
                       << " and prepare from level " << i-1 << " to " << i;
            mLevels[i].commit();
            mLevels[i].prepare(app, mLevels[i-1].snap());
        }
    }
}


}
