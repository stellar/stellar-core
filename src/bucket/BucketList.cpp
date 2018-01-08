// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "BucketList.h"
#include "bucket/Bucket.h"
#include "bucket/BucketManager.h"
#include "bucket/LedgerCmp.h"
#include "crypto/Hex.h"
#include "crypto/Random.h"
#include "crypto/SHA.h"
#include "main/Application.h"
#include "util/Logging.h"
#include "util/XDRStream.h"
#include "util/types.h"
#include <cassert>

namespace stellar
{

BucketLevel::BucketLevel(uint32_t i)
    : mLevel(i)
    , mCurr(std::make_shared<Bucket>())
    , mSnap(std::make_shared<Bucket>())
{
}

uint256
BucketLevel::getHash() const
{
    auto hsh = SHA256::create();
    hsh->add(mCurr->getHash());
    hsh->add(mSnap->getHash());
    return hsh->finish();
}

FutureBucket const&
BucketLevel::getNext() const
{
    return mNextCurr;
}

FutureBucket&
BucketLevel::getNext()
{
    return mNextCurr;
}

void
BucketLevel::setNext(FutureBucket const& fb)
{
    mNextCurr = fb;
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
BucketLevel::setCurr(std::shared_ptr<Bucket> b)
{
    mNextCurr.clear();
    if (mCurr)
    {
        mCurr->setRetain(false);
    }
    mCurr = b;
    if (mCurr)
    {
        mCurr->setRetain(true);
    }
}

void
BucketLevel::setSnap(std::shared_ptr<Bucket> b)
{
    if (mSnap)
    {
        mSnap->setRetain(false);
    }
    mSnap = b;
    if (mSnap)
    {
        mSnap->setRetain(true);
    }
}

void
BucketLevel::commit()
{
    if (mNextCurr.isLive())
    {
        setCurr(mNextCurr.resolve());
        // CLOG(DEBUG, "Bucket") << "level " << mLevel << " set mCurr to "
        //            << mCurr->getEntries().size() << " elements";
    }
    assert(!mNextCurr.isMerging());
}

void
BucketLevel::prepare(Application& app, uint32_t currLedger,
                     std::shared_ptr<Bucket> snap,
                     std::vector<std::shared_ptr<Bucket>> const& shadows)
{
    // If more than one absorb is pending at the same time, we have a logic
    // error in our caller (and all hell will break loose).
    assert(!mNextCurr.isMerging());

    auto curr = mCurr;

    // Subtle: We're "preparing the next state" of this level's mCurr, which is
    // *either* mCurr merged with snap, or else just snap (if mCurr is going to
    // be snapshotted itself in the next spill). This second condition happens
    // when currLedger is one multiple of the previous levels's spill-size away
    // from a snap of its own.  Eg. level 1 at ledger 120 (2 away from
    // 8, its next snap), or level 2 at ledger 24 (8 away from 32, its
    // next snap).
    if (mLevel != 0)
    {
        uint32_t nextChangeLedger =
            currLedger + BucketList::levelHalf(mLevel - 1);
        if (BucketList::levelShouldSpill(nextChangeLedger, mLevel))
        {
            // CLOG(DEBUG, "Bucket") << "level " << mLevel
            //            << " skipping pending-snapshot curr";
            curr = std::make_shared<Bucket>();
        }
    }

    mNextCurr = FutureBucket(app, curr, snap, shadows,
                             BucketList::keepDeadEntries(mLevel));
    assert(mNextCurr.isMerging());
}

std::shared_ptr<Bucket>
BucketLevel::snap()
{
    mSnap = mCurr;
    mCurr = std::make_shared<Bucket>();
    // CLOG(DEBUG, "Bucket") << "level " << mLevel << " set mSnap to "
    //            << mSnap->getEntries().size() << " elements";
    // CLOG(DEBUG, "Bucket") << "level " << mLevel << " reset mCurr to "
    //            << mCurr->getEntries().size() << " elements";
    return mSnap;
}

BucketListDepth::BucketListDepth(uint32_t numLevels) : mNumLevels(numLevels)
{
}

BucketListDepth&
BucketListDepth::operator=(uint32_t numLevels)
{
    mNumLevels = numLevels;
    return *this;
}

BucketListDepth::operator uint32_t() const
{
    return mNumLevels;
}

uint32_t
BucketList::levelSize(uint32_t level)
{
    assert(level < kNumLevels);
    return 1UL << (2 * (level + 1));
}

uint32_t
BucketList::levelHalf(uint32_t level)
{
    return levelSize(level) >> 1;
}

uint32_t
BucketList::mask(uint32_t v, uint32_t m)
{
    return v & ~(m - 1);
}

uint32_t
BucketList::sizeOfCurr(uint32_t ledger, uint32_t level)
{
    assert(ledger != 0);
    assert(level < kNumLevels);
    if (level == 0)
    {
        return (ledger == 1) ? 1 : (1 + ledger % 2);
    }

    auto const size = levelSize(level);
    auto const half = levelHalf(level);
    if (level != BucketList::kNumLevels - 1 && mask(ledger, half) != 0)
    {
        uint32_t const sizeDelta = 1UL << (2 * level - 1);
        if (mask(ledger, half) == ledger || mask(ledger, size) == ledger)
        {
            return sizeDelta;
        }

        auto const prevSize = levelSize(level - 1);
        auto const prevHalf = levelHalf(level - 1);
        uint32_t previousRelevantLedger =
            std::max({mask(ledger - 1, prevHalf), mask(ledger - 1, prevSize),
                      mask(ledger - 1, half), mask(ledger - 1, size)});
        if (mask(ledger, prevHalf) == ledger ||
            mask(ledger, prevSize) == ledger)
        {
            return sizeOfCurr(previousRelevantLedger, level) + sizeDelta;
        }
        else
        {
            return sizeOfCurr(previousRelevantLedger, level);
        }
    }
    else
    {
        uint32_t blsize = 0;
        for (uint32_t l = 0; l < level; l++)
        {
            blsize += sizeOfCurr(ledger, l);
            blsize += sizeOfSnap(ledger, l);
        }
        return ledger - blsize;
    }
}

uint32_t
BucketList::sizeOfSnap(uint32_t ledger, uint32_t level)
{
    assert(ledger != 0);
    assert(level < kNumLevels);
    if (level == BucketList::kNumLevels - 1)
    {
        return 0;
    }
    else if (mask(ledger, levelSize(level)) != 0)
    {
        return levelHalf(level);
    }
    else
    {
        uint32_t size = 0;
        for (uint32_t l = 0; l < level; l++)
        {
            size += sizeOfCurr(ledger, l);
            size += sizeOfSnap(ledger, l);
        }
        size += sizeOfCurr(ledger, level);
        return ledger - size;
    }
}

uint32_t
BucketList::oldestLedgerInCurr(uint32_t ledger, uint32_t level)
{
    assert(ledger != 0);
    assert(level < kNumLevels);
    if (sizeOfCurr(ledger, level) == 0)
    {
        return std::numeric_limits<uint32_t>::max();
    }

    uint32_t count = ledger;
    for (uint32_t l = 0; l < level; l++)
    {
        count -= sizeOfCurr(ledger, l);
        count -= sizeOfSnap(ledger, l);
    }
    count -= sizeOfCurr(ledger, level);
    return count + 1;
}

uint32_t
BucketList::oldestLedgerInSnap(uint32_t ledger, uint32_t level)
{
    assert(ledger != 0);
    assert(level < kNumLevels);
    if (sizeOfSnap(ledger, level) == 0)
    {
        return std::numeric_limits<uint32_t>::max();
    }

    uint32_t count = ledger;
    for (uint32_t l = 0; l <= level; l++)
    {
        count -= sizeOfCurr(ledger, l);
        count -= sizeOfSnap(ledger, l);
    }
    return count + 1;
}

uint256
BucketList::getHash() const
{
    auto hsh = SHA256::create();
    for (auto const& lev : mLevels)
    {
        hsh->add(lev.getHash());
    }
    return hsh->finish();
}

bool
BucketList::levelShouldSpill(uint32_t ledger, uint32_t level)
{
    if (level == kNumLevels - 1)
    {
        // There's a max level, that never spills.
        return false;
    }

    return (ledger == mask(ledger, levelHalf(level)) ||
            ledger == mask(ledger, levelSize(level)));
}

bool
BucketList::keepDeadEntries(uint32_t level)
{
    return level < BucketList::kNumLevels - 1;
}

BucketLevel&
BucketList::getLevel(uint32_t i)
{
    return mLevels.at(i);
}

void
BucketList::addBatch(Application& app, uint32_t currLedger,
                     std::vector<LedgerEntry> const& liveEntries,
                     std::vector<LedgerKey> const& deadEntries)
{
    assert(currLedger > 0);

    std::vector<std::shared_ptr<Bucket>> shadows;
    for (auto& level : mLevels)
    {
        shadows.push_back(level.getCurr());
        shadows.push_back(level.getSnap());
    }

    // We will be counting-down from the highest-numbered level (the
    // oldest/largest level) to the lowest (youngest); each step we check for
    // shadows in all the levels _above_ the level we're merging, which means we
    // pop the current level's curr and snap buckets off the shadow list before
    // proceeding, to avoid a level shadowing itself.
    //
    // But beyond this, we also pop the *previous level's* curr and snap from
    // 'shadows' as well, for the following reason:
    //
    //   - Immediately before beginning to merge level i-1 into level i, level
    //     i-1 has [curr=A, snap=B]. These two buckets, A and B, are on the end
    //     of the shadows list.
    //
    //   - Upon calling level[i-1].snap(), level i-1 has [curr={}, snap=A]; B
    //     has been discarded since we're (by definition of restarting a merge
    //     on level[i]) done merging it.
    //
    //   - We do _not_ want to consider A or B as members of the shadow set when
    //     merging A into level i; B has already been incorporated and A is the
    //     new material we're trying to incorporate.
    //
    // So at any given level-merge i, we should be considering shadows only in
    // levels i-2 and lower. Therefore before the loop we pop the first two
    // elements of 'shadows', and then inside the loop we pop two more for each
    // iteration.

    assert(shadows.size() >= 2);
    shadows.pop_back();
    shadows.pop_back();

    for (uint32_t i = static_cast<uint32>(mLevels.size()) - 1; i != 0; --i)
    {
        assert(shadows.size() >= 2);
        shadows.pop_back();
        shadows.pop_back();

        /*
        CLOG(DEBUG, "Bucket") << "curr=" << currLedger
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

            // CLOG(DEBUG, "Bucket") << "Ledger " << currLedger
            //           << " causing commit on level " << i
            //           << " and prepare of "
            //           << snap->getEntries().size()
            //           << " element snap from level " << i-1
            //           << " to level " << i;

            mLevels[i].commit();
            mLevels[i].prepare(app, currLedger, snap, shadows);
        }
    }

    assert(shadows.size() == 0);
    mLevels[0].prepare(
        app, currLedger,
        Bucket::fresh(app.getBucketManager(), liveEntries, deadEntries),
        shadows);
    mLevels[0].commit();
}

void
BucketList::restartMerges(Application& app)
{
    for (auto i = 0; i < mLevels.size(); i++)
    {
        auto& level = mLevels[i];
        auto& next = level.getNext();
        if (next.hasHashes() && !next.isLive())
        {
            next.makeLive(app, keepDeadEntries(i));
            if (next.isMerging())
            {
                CLOG(INFO, "Bucket")
                    << "Restarted merge on BucketList level " << i;
            }
        }
    }
}

BucketListDepth BucketList::kNumLevels = 11;

BucketList::BucketList()
{
    for (uint32_t i = 0; i < kNumLevels; ++i)
    {
        mLevels.push_back(BucketLevel(i));
    }
}
}
