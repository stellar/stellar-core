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
#include "xdrpp/marshal.h"
#include <cassert>

namespace stellar
{

/**
 * Compare two LedgerEntries for 'identity', not content.
 *
 * LedgerEntries are identified iff they have:
 *
 *   - The same type
 *     - If accounts, then with same accountID
 *     - If trustlines, then with same (accountID, currency) pair
 *     - If offers, then with same (accountID, sequence) pair
 */
struct
LedgerEntryIdCmp
{
    bool operator()(LedgerEntry const& a,
                    LedgerEntry const& b) const
    {
        LedgerType aty = a.type();
        LedgerType bty = b.type();

        if (aty < bty)
            return true;

        if (aty > bty)
            return false;

        switch (aty)
        {
        case NONE:
            return false;

        case ACCOUNT:
            return a.account().accountID < b.account().accountID;

        case TRUSTLINE:
        {
            TrustLineEntry const& atl = a.trustLine();
            TrustLineEntry const& btl = b.trustLine();
            if (atl.accountID < btl.accountID)
                return true;
            if (atl.accountID > btl.accountID)
                return false;
            {
                using xdr::operator<;
                return atl.currency < btl.currency;
            }
        }

        case OFFER:
        {
            OfferEntry const& aof = a.offer();
            OfferEntry const& bof = b.offer();
            if (aof.accountID < bof.accountID)
                return true;
            if (aof.accountID > bof.accountID)
                return false;
            return aof.sequence < bof.sequence;
        }
        }
        return false;
    }
};

/**
 * Compare two CLFEntries for identity by comparing their respective
 * LedgerEntries (ignoring their hashes, as the LedgerEntryIdCmp ignores their
 * bodies).
 */
struct
CLFEntryIdCmp
{
    LedgerEntryIdCmp mCmp;
    bool operator()(CLFEntry const& a,
                    CLFEntry const& b) const
    {
            return mCmp(a.entry, b.entry);
    }
};

Bucket::Bucket(std::vector<CLFEntry>&& entries, uint256&& hash)
    : mEntries(entries), mHash(hash)
{
}

Bucket::Bucket()
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

std::shared_ptr<Bucket>
Bucket::fresh(std::vector<LedgerEntry> const& entries)
{
    std::vector<CLFEntry> clfEntries;
    clfEntries.reserve(entries.size());
    for (auto const& e : entries)
    {
        CLFEntry ce;
        ce.entry = e;
        ce.hash = sha512_256(xdr::xdr_to_msg(e));
        clfEntries.push_back(ce);
    }

    std::sort(clfEntries.begin(), clfEntries.end(),
              CLFEntryIdCmp());

    SHA512_256 hsh;
    for (auto const& ce : clfEntries)
    {
        hsh.add(ce.hash);
    }
    return std::make_shared<Bucket>(std::move(clfEntries), hsh.finish());
}

std::shared_ptr<Bucket>
Bucket::merge(std::shared_ptr<Bucket> const& oldBucket,
              std::shared_ptr<Bucket> const& newBucket)
{
    // This is the key operation in the scheme: merging two (read-only)
    // buckets together into a new 3rd bucket, while calculating its hash,
    // in a single pass.

    assert(oldBucket);
    assert(newBucket);

    std::vector<CLFEntry>::const_iterator oi = oldBucket->mEntries.begin();
    std::vector<CLFEntry>::const_iterator ni = newBucket->mEntries.begin();

    std::vector<CLFEntry>::const_iterator oe = oldBucket->mEntries.end();
    std::vector<CLFEntry>::const_iterator ne = newBucket->mEntries.end();

    std::vector<CLFEntry> out;
    out.reserve(oldBucket->mEntries.size() + newBucket->mEntries.size());
    SHA512_256 hsh;
    CLFEntryIdCmp cmp;
    while (oi != oe || ni != ne)
    {
        std::vector<CLFEntry>::const_iterator e;
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
        else if (cmp(*oi, *ni))
        {
            // Next old-entry has smaller key, take it.
            e = oi++;
        }
        else if (cmp(*ni, *oi))
        {
            // Next new-entry has smaller key, take it.
            e = ni++;
        }
        else
        {
            // Old and new are for the same key, take new.
            e = ni++;
        }
        hsh.add(e->hash);
        out.emplace_back(*e);
    }
    return std::make_shared<Bucket>(std::move(out), hsh.finish());
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

Bucket const&
BucketLevel::getCurr() const
{
    assert(mCurr);
    return *mCurr;
}

Bucket const&
BucketLevel::getSnap() const
{
    assert(mSnap);
    return *mSnap;
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
                     std::shared_ptr<Bucket> snap)
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

    using task_t = std::packaged_task<std::shared_ptr<Bucket>()>;
    std::shared_ptr<task_t> task =
        std::make_shared<task_t>([curr, snap]()
                                 {
                                     if (curr)
                                     {
                                         // LOG(DEBUG)
                                         //<< "Worker merging " <<
                                         // snap->getEntries().size()
                                         //<< " new elements with " <<
                                         // curr->getEntries().size()
                                         //<< " existing";
                                         // TIMED_SCOPE(timer, "merge + hash");
                                         auto res = Bucket::merge(curr, snap);
                                         // LOG(DEBUG)
                                         //<< "Worker finished merging " <<
                                         // snap->getEntries().size()
                                         //<< " new elements with " <<
                                         // curr->getEntries().size()
                                         //<< " existing (new size: " <<
                                         // res->getEntries().size() << ")";
                                         return res;
                                     }
                                     else
                                         return std::shared_ptr<Bucket>(snap);
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
                     std::vector<LedgerEntry> const& batch)
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

    for (size_t i = mLevels.size() - 1; i > 0; --i)
    {
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
            mLevels[i].prepare(app, currLedger, snap);
        }
    }

    mLevels[0].prepare(app, currLedger, Bucket::fresh(batch));
    mLevels[0].commit();
}
}
