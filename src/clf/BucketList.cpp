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
#include "clf/Bucket.h"
#include "clf/CLFMaster.h"
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
    SHA256 hsh;
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
BucketLevel::setCurr(std::shared_ptr<Bucket> b)
{
    mCurr = b;
}

void
BucketLevel::setSnap(std::shared_ptr<Bucket> b)
{
    mSnap = b;
}

void
BucketLevel::commit()
{
    if (mNextCurr.valid())
    {
        // NB: This might block if the worker thread is slow; might want to
        // use mNextCurr.wait_for(
        mCurr = mNextCurr.get();
        // CLOG(DEBUG, "CLF") << "level " << mLevel << " set mCurr to "
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
            // CLOG(DEBUG, "CLF") << "level " << mLevel
            //            << " skipping pending-snapshot curr";
            curr.reset();
        }
    }

    // CLOG(DEBUG, "CLF") << "level " << mLevel << " preparing merge of mCurr="
    //            << (curr ? curr->getEntries().size() : 0) << " with snap="
    //            << snap->getEntries().size() << " elements";
    CLFMaster& clfMaster = app.getCLFMaster();
    using task_t = std::packaged_task<std::shared_ptr<Bucket>()>;
    std::shared_ptr<task_t> task =
        std::make_shared<task_t>(
            [curr, snap, &clfMaster, shadows]()
            {
                // CLOG(DEBUG, "CLF")
                //<< "Worker merging " <<
                // snap->getEntries().size()
                //<< " new elements with " <<
                // curr->getEntries().size()
                //<< " existing";
                // TIMED_SCOPE(timer, "merge + hash");
                auto res = Bucket::merge(clfMaster,
                                         (curr ? curr :
                                          std::make_shared<Bucket>()),
                                         snap, shadows);
                // CLOG(DEBUG, "CLF")
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
    // CLOG(DEBUG, "CLF") << "level " << mLevel << " set mSnap to "
    //            << mSnap->getEntries().size() << " elements";
    // CLOG(DEBUG, "CLF") << "level " << mLevel << " reset mCurr to "
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

uint256
BucketList::getHash() const
{
    SHA256 hsh;
    for (auto const& lev : mLevels)
    {
        hsh.add(lev.getHash());
    }
    return hsh.finish();
}

bool
BucketList::levelShouldSpill(uint64_t ledger, size_t level)
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
BucketList::addBatch(Application& app, uint64_t currLedger,
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

    assert(shadows.size() == 2);
    shadows.pop_back();
    shadows.pop_back();
    mLevels[0].prepare(app, currLedger,
                       Bucket::fresh(app.getCLFMaster(),
                                     liveEntries, deadEntries),
                       shadows);
    mLevels[0].commit();
}

size_t const
BucketList::kNumLevels = 5;

BucketList::BucketList()
{
    for (size_t i = 0; i < kNumLevels; ++i)
    {
        mLevels.push_back(BucketLevel(i));
    }
}

}
