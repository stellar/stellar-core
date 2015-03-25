// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "BucketList.h"
// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"
#include "main/Application.h"
#include "util/Logging.h"
#include "util/types.h"
#include "crypto/SHA.h"
#include "crypto/Hex.h"
#include "crypto/Random.h"
#include "util/XDRStream.h"
#include "clf/Bucket.h"
#include "clf/CLFManager.h"
#include "clf/LedgerCmp.h"
#include <cassert>

namespace stellar
{

BucketLevel::BucketLevel(size_t i)
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
    clearPendingMerge();
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
BucketLevel::clearPendingMerge()
{
    // NB: MSVC future<> implementation doesn't purge the task lambda (and
    // its captures) on invalidation (due to get()); must explicitly reset.
    mNextCurr = std::future<std::shared_ptr<Bucket>>();
}

void
BucketLevel::commit()
{
    if (mNextCurr.valid())
    {
        // NB: This might block if the worker thread is slow; might want to
        // use mNextCurr.wait_for()
        setCurr(mNextCurr.get());
        // CLOG(DEBUG, "CLF") << "level " << mLevel << " set mCurr to "
        //            << mCurr->getEntries().size() << " elements";
    }
    assert(!mNextCurr.valid());
}

void
BucketLevel::prepare(Application& app, uint32_t currLedger,
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
    // from a snap of its own.  Eg. level 1 at ledger 120 (2 away from
    // 8, its next snap), or level 2 at ledger 24 (8 away from 32, its
    // next snap).
    if (mLevel > 0)
    {
        uint32_t nextChangeLedger =
            currLedger + BucketList::levelHalf(mLevel - 1);
        if (BucketList::levelShouldSpill(nextChangeLedger, mLevel))
        {
            // CLOG(DEBUG, "CLF") << "level " << mLevel
            //            << " skipping pending-snapshot curr";
            curr.reset();
        }
    }

    //CLOG(INFO, "CLF")
    //    << "Worker preparing merge of " << snap->getFilename() << " with " << snap->getFilename();

    CLFManager& clfManager = app.getCLFManager();
    using task_t = std::packaged_task<std::shared_ptr<Bucket>()>;
    std::shared_ptr<task_t> task = std::make_shared<task_t>(
        [curr, snap, &clfManager, shadows]()
        {
            //CLOG(INFO, "CLF")
            //    << "Worker merging " << snap->getFilename() << " with " << snap->getFilename();

             auto res = Bucket::merge(clfManager,
                                     (curr ? curr : std::make_shared<Bucket>()),
                                     snap, shadows);
             //CLOG(INFO, "CLF")
             //    << "Worker finished merging " << snap->getFilename() << " with " << snap->getFilename();
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
    // CLOG(DEBUG, "CLF") << "level " << mLevel << " set mSnap to "
    //            << mSnap->getEntries().size() << " elements";
    // CLOG(DEBUG, "CLF") << "level " << mLevel << " reset mCurr to "
    //            << mCurr->getEntries().size() << " elements";
    return mSnap;
}

uint32_t
BucketList::levelSize(size_t level)
{
    assert(level < kNumLevels);
    return 1UL << (2 * (static_cast<uint32_t>(level) + 1));
}

uint32_t
BucketList::levelHalf(size_t level)
{
    return levelSize(level) >> 1;
}

uint32_t
BucketList::mask(uint32_t v, uint32_t m)
{
    return v & ~(m - 1);
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
BucketList::levelShouldSpill(uint32_t ledger, size_t level)
{
    if (level == kNumLevels - 1)
    {
        // There's a max level, that never spills.
        return false;
    }

    return (ledger == mask(ledger, levelHalf(level)) ||
            ledger == mask(ledger, levelSize(level)));
}

size_t
BucketList::numLevels() const
{
    return kNumLevels;
}

BucketLevel&
BucketList::getLevel(size_t i)
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

    for (size_t i = mLevels.size() - 1; i > 0; --i)
    {
        assert(shadows.size() >= 2);
        shadows.pop_back();
        shadows.pop_back();

        /*
        CLOG(DEBUG, "CLF") << "curr=" << currLedger
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

            // CLOG(DEBUG, "CLF") << "Ledger " << currLedger
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
        Bucket::fresh(app.getCLFManager(), liveEntries, deadEntries), shadows);
    mLevels[0].commit();
}

void
BucketList::restartMerges(Application& app, uint32_t currLedger)
{
    // Scan the bucketlist, kill all existing merges and start a new merge between any
    // nonzero snap and its subsequent level.

    std::vector<std::shared_ptr<Bucket>> shadows;
    for (auto& level : mLevels)
    {
        shadows.push_back(level.getCurr());
        shadows.push_back(level.getSnap());
    }

    assert(shadows.size() >= 2);
    shadows.pop_back();
    shadows.pop_back();

    for (size_t i = mLevels.size() - 1; i > 0; --i)
    {
        assert(shadows.size() >= 2);
        shadows.pop_back();
        shadows.pop_back();

        mLevels[i].clearPendingMerge();

        // Restart merges on _all_ nonzero-snap levels, assuming
        // they should already have been running.
        auto snap = mLevels[i - 1].getSnap();
        if (!isZero(snap->getHash()))
        {
            mLevels[i].prepare(app, currLedger, snap, shadows);
        }
    }
}

size_t const BucketList::kNumLevels = 11;

BucketList::BucketList()
{
    for (size_t i = 0; i < kNumLevels; ++i)
    {
        mLevels.push_back(BucketLevel(i));
    }
}
}
