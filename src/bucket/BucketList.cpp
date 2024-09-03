// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "BucketList.h"
#include "bucket/Bucket.h"
#include "bucket/BucketInputIterator.h"
#include "bucket/BucketManager.h"
#include "bucket/BucketSnapshot.h"
#include "crypto/SHA.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "main/Application.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/ProtocolVersion.h"
#include "util/types.h"

#include "medida/counter.h"

#include <Tracy.hpp>
#include <fmt/format.h>

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
    SHA256 hsh;
    hsh.add(mCurr->getHash());
    hsh.add(mSnap->getHash());
    return hsh.finish();
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
    releaseAssert(threadIsMain());
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
    releaseAssert(threadIsMain());
    mNextCurr.clear();
    mCurr = b;
}

bool
BucketList::shouldMergeWithEmptyCurr(uint32_t ledger, uint32_t level)
{

    if (level != 0)
    {
        // Round down the current ledger to when the merge was started, and
        // re-start the merge via prepare, mimicking the logic in `addBatch`
        auto mergeStartLedger =
            roundDown(ledger, BucketList::levelHalf(level - 1));

        // Subtle: We're "preparing the next state" of this level's mCurr, which
        // is *either* mCurr merged with snap, or else just snap (if mCurr is
        // going to be snapshotted itself in the next spill). This second
        // condition happens when currLedger is one multiple of the previous
        // levels's spill-size away from a snap of its own.  Eg. level 1 at
        // ledger 6 (2 away from 8, its next snap), or level 2 at ledger 24 (8
        // away from 32, its next snap). See diagram above.
        uint32_t nextChangeLedger = mergeStartLedger + levelHalf(level - 1);
        if (levelShouldSpill(nextChangeLedger, level))
        {
            return true;
        }
    }
    return false;
}

void
BucketLevel::setSnap(std::shared_ptr<Bucket> b)
{
    releaseAssert(threadIsMain());
    mSnap = b;
}

void
BucketLevel::commit()
{
    if (mNextCurr.isLive())
    {
        setCurr(mNextCurr.resolve());
    }
    releaseAssert(!mNextCurr.isMerging());
}

// prepare builds a FutureBucket for the _next state_ of the current level,
// kicking off a merge that will finish sometime later.
//
// Depending on the current ledger number, this will _either_ be a merge between
// this level's mCurr and the previous level's snap, or a plain scan/compaction
// of the previous level's snap on its own.
//
// The "only previous level's snap" cases happen once every 4 `prepare`s: when
// we're one incoming-spill away from snapshotting mCurr ourselves. In other
// words, when the `currLedger` plus N is considered a spill event for this
// level, where N is the previous level's spill-size. This is a bit subtle
// so we spell out the first 8 ledger numbers where it happens here for each of
// levels 1-4: merge(Lev) is a list of ledger numbers where Lev gets a full
// mCurr+prevSnap merge, snap(Lev) is a list of ledger numbers where Lev just
// gets a propagated prevSnap. We've lined the numbers up in space to make the
// pattern a little more obvious:
//
// clang-format off
//
// ----------------------------------------------------------------------------------------
// merge(1) 0   2=0x02,   4=0x004,              8=0x008,  10=0x00a,  12=0x00c,
//  snap(1)                          6=0x006,                                   14=0x00e,
// ----------------------------------------------------------------------------------------
// merge(2) 0   8=0x08,  16=0x010,             32=0x020,  40=0x028,  48=0x030,
//  snap(2)                         24=0x018,                                   56=0x038,
// ----------------------------------------------------------------------------------------
// merge(3) 0  32=0x20,  64=0x040,            128=0x080, 160=0x0a0, 192=0x0c0,
//  snap(3)                         96=0x060,                                  224=0x0e0,
// ----------------------------------------------------------------------------------------
// merge(4) 0 128=0x80, 256=0x100,            512=0x200, 640=0x280, 768=0x300,
//  snap(4)                        384=0x180,                                  896=0x380,
// ----------------------------------------------------------------------------------------
// ...
// clang-format on
void
BucketLevel::prepare(Application& app, uint32_t currLedger,
                     uint32_t currLedgerProtocol, std::shared_ptr<Bucket> snap,
                     std::vector<std::shared_ptr<Bucket>> const& shadows,
                     bool countMergeEvents)
{
    ZoneScoped;
    // If more than one absorb is pending at the same time, we have a logic
    // error in our caller (and all hell will break loose).
    releaseAssert(!mNextCurr.isMerging());
    auto curr = BucketList::shouldMergeWithEmptyCurr(currLedger, mLevel)
                    ? std::make_shared<Bucket>()
                    : mCurr;

    auto shadowsBasedOnProtocol =
        protocolVersionStartsFrom(Bucket::getBucketVersion(snap),
                                  Bucket::FIRST_PROTOCOL_SHADOWS_REMOVED)
            ? std::vector<std::shared_ptr<Bucket>>()
            : shadows;
    mNextCurr = FutureBucket(app, curr, snap, shadowsBasedOnProtocol,
                             currLedgerProtocol, countMergeEvents, mLevel);
    releaseAssert(mNextCurr.isMerging());
}

std::shared_ptr<Bucket>
BucketLevel::snap()
{
    mSnap = mCurr;
    mCurr = std::make_shared<Bucket>();
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

// levelSize is the idealized size of a level for algorithmic-boundary purposes;
// in practice the oldest level at any moment has a different size. We list the
// values here for reference:
//
// levelSize(0)  =       4=0x000004
// levelSize(1)  =      16=0x000010
// levelSize(2)  =      64=0x000040
// levelSize(3)  =     256=0x000100
// levelSize(4)  =    1024=0x000400
// levelSize(5)  =    4096=0x001000
// levelSize(6)  =   16384=0x004000
// levelSize(7)  =   65536=0x010000
// levelSize(8)  =  262144=0x040000
// levelSize(9)  = 1048576=0x100000
// levelSize(10) = 4194304=0x400000
uint32_t
BucketList::levelSize(uint32_t level)
{
    releaseAssert(level < kNumLevels);
    return 1UL << (2 * (level + 1));
}

// levelHalf is the idealized size of a half-level for algorithmic-boundary
// purposes; in practice the oldest level at any moment has a different size.
// We list the values here for reference:
//
// levelHalf(0)  =       2=0x000002
// levelHalf(1)  =       8=0x000008
// levelHalf(2)  =      32=0x000020
// levelHalf(3)  =     128=0x000080
// levelHalf(4)  =     512=0x000200
// levelHalf(5)  =    2048=0x000800
// levelHalf(6)  =    8192=0x002000
// levelHalf(7)  =   32768=0x008000
// levelHalf(8)  =  131072=0x020000
// levelHalf(9)  =  524288=0x080000
// levelHalf(10) = 2097152=0x200000
uint32_t
BucketList::levelHalf(uint32_t level)
{
    return levelSize(level) >> 1;
}

uint32_t
BucketList::sizeOfCurr(uint32_t ledger, uint32_t level)
{
    releaseAssert(ledger != 0);
    releaseAssert(level < kNumLevels);
    if (level == 0)
    {
        return (ledger == 1) ? 1 : (1 + ledger % 2);
    }

    auto const size = levelSize(level);
    auto const half = levelHalf(level);
    if (level != BucketList::kNumLevels - 1 && roundDown(ledger, half) != 0)
    {
        uint32_t const sizeDelta = 1UL << (2 * level - 1);
        if (roundDown(ledger, half) == ledger ||
            roundDown(ledger, size) == ledger)
        {
            return sizeDelta;
        }

        auto const prevSize = levelSize(level - 1);
        auto const prevHalf = levelHalf(level - 1);
        uint32_t previousRelevantLedger = std::max(
            {roundDown(ledger - 1, prevHalf), roundDown(ledger - 1, prevSize),
             roundDown(ledger - 1, half), roundDown(ledger - 1, size)});
        if (roundDown(ledger, prevHalf) == ledger ||
            roundDown(ledger, prevSize) == ledger)
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
    releaseAssert(ledger != 0);
    releaseAssert(level < kNumLevels);
    if (level == BucketList::kNumLevels - 1)
    {
        return 0;
    }
    else if (roundDown(ledger, levelSize(level)) != 0)
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
    releaseAssert(ledger != 0);
    releaseAssert(level < kNumLevels);
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
    releaseAssert(ledger != 0);
    releaseAssert(level < kNumLevels);
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
    ZoneScoped;
    SHA256 hsh;
    for (auto const& lev : mLevels)
    {
        hsh.add(lev.getHash());
    }
    return hsh.finish();
}

// levelShouldSpill is the set of boundaries at which each level should
// spill, it's not-entirely obvious which numbers these are by inspection,
// so we list the first 3 values it's true on each level here for reference:
//
// clang-format off
//
// levelShouldSpill(_, 0): 0,       2=0x000002,       4=0x000004,       6=0x000006
// levelShouldSpill(_, 1): 0,       8=0x000008,      16=0x000010,      24=0x000018
// levelShouldSpill(_, 2): 0,      32=0x000020,      64=0x000040,      96=0x000060
// levelShouldSpill(_, 3): 0,     128=0x000080,     256=0x000100,     384=0x000180
// levelShouldSpill(_, 4): 0,     512=0x000200,    1024=0x000400,    1536=0x000600
// levelShouldSpill(_, 5): 0,    2048=0x000800,    4096=0x001000,    6144=0x001800
// levelShouldSpill(_, 6): 0,    8192=0x002000,   16384=0x004000,   24576=0x006000
// levelShouldSpill(_, 7): 0,   32768=0x008000,   65536=0x010000,   98304=0x018000
// levelShouldSpill(_, 8): 0,  131072=0x020000,  262144=0x040000,  393216=0x060000
// levelShouldSpill(_, 9): 0,  524288=0x080000, 1048576=0x100000, 1572864=0x180000
//
// clang-format on

bool
BucketList::levelShouldSpill(uint32_t ledger, uint32_t level)
{
    if (level == kNumLevels - 1)
    {
        // There's a max level, that never spills.
        return false;
    }

    return (ledger == roundDown(ledger, levelHalf(level)) ||
            ledger == roundDown(ledger, levelSize(level)));
}

// Update period for curr buckets is the incoming spill frequency of the level
// which the bucket is in. For snap buckets, the update period is the incoming
// spill frequency of the level below.
// incoming_spill_frequency(i) = 2^(2i - 1) for i > 0
// incoming_spill_frequency(0) = 1
uint32_t
BucketList::bucketUpdatePeriod(uint32_t level, bool isCurr)
{
    if (!isCurr)
    {
        // Snap bucket changeRate is the same as the change rate of the curr
        // bucket in the level below
        return bucketUpdatePeriod(level + 1, true);
    }

    if (level == 0)
    {
        return 1;
    }

    return 1u << (2 * level - 1);
}

bool
BucketList::keepDeadEntries(uint32_t level)
{
    return level < BucketList::kNumLevels - 1;
}

BucketLevel const&
BucketList::getLevel(uint32_t i) const
{
    return mLevels.at(i);
}

BucketLevel&
BucketList::getLevel(uint32_t i)
{
    return mLevels.at(i);
}

void
BucketList::resolveAnyReadyFutures()
{
    ZoneScoped;
    for (auto& level : mLevels)
    {
        if (level.getNext().isMerging() && level.getNext().mergeComplete())
        {
            level.getNext().resolve();
        }
    }
}

bool
BucketList::futuresAllResolved(uint32_t maxLevel) const
{
    ZoneScoped;
    releaseAssert(maxLevel < mLevels.size());

    for (uint32_t i = 0; i <= maxLevel; i++)
    {
        if (mLevels[i].getNext().isMerging())
        {
            return false;
        }
    }
    return true;
}

uint32_t
BucketList::getMaxMergeLevel(uint32_t currLedger) const
{
    uint32_t i = 0;
    for (; i < static_cast<uint32_t>(mLevels.size()) - 1; ++i)
    {
        if (!levelShouldSpill(currLedger, i))
        {
            break;
        }
    }
    return i;
}

uint64_t
BucketList::getSize() const
{
    uint64_t sum = 0;
    for (auto const& lev : mLevels)
    {
        std::array<std::shared_ptr<Bucket>, 2> buckets = {lev.getCurr(),
                                                          lev.getSnap()};
        for (auto const& b : buckets)
        {
            if (b)
            {
                sum += b->getSize();
            }
        }
    }

    return sum;
}

void
BucketList::addBatch(Application& app, uint32_t currLedger,
                     uint32_t currLedgerProtocol,
                     std::vector<LedgerEntry> const& initEntries,
                     std::vector<LedgerEntry> const& liveEntries,
                     std::vector<LedgerKey> const& deadEntries)
{
    ZoneScoped;
    releaseAssert(currLedger > 0);

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

    releaseAssert(shadows.size() >= 2);
    shadows.pop_back();
    shadows.pop_back();

    for (uint32_t i = static_cast<uint32>(mLevels.size()) - 1; i != 0; --i)
    {
        releaseAssert(shadows.size() >= 2);
        shadows.pop_back();
        shadows.pop_back();

        /*
        CLOG_DEBUG(Bucket, "curr={}, half(i-1)={}, size(i-1)={},
        roundDown(curr,half)={}, roundDown(curr,size)={}", currLedger,
        levelHalf(i-1), levelSize(i-1), roundDown(currLedger, levelHalf(i-1)),
        roundDown(currLedger, levelSize(i-1)));
        */
        if (levelShouldSpill(currLedger, i - 1))
        {
            /**
             * At every ledger, level[0] prepares the new batch and commits
             * it.
             *
             * At ledger multiples of 2, level[0] snaps, level[1] commits
             * existing (promotes next to curr) and "prepares" by starting a
             * merge of that new level[1] curr with the new level[0] snap. This
             * is "level 0 spilling".
             *
             * At ledger multiples of 8, level[1] snaps, level[2] commits
             * existing (promotes next to curr) and "prepares" by starting a
             * merge of that new level[2] curr with the new level[1] snap. This
             * is "level 1 spilling".
             *
             * At ledger multiples of 32, level[2] snaps, level[3] commits
             * existing (promotes next to curr) and "prepares" by starting a
             * merge of that new level[3] curr with the new level[2] snap. This
             * is "level 2 spilling".
             *
             * All these have to be done in _reverse_ order (counting down
             * levels) because we want a 'curr' to be pulled out of the way into
             * a 'snap' the moment it's half-a-level full, not have anything
             * else spilled/added to it.
             */

            auto snap = mLevels[i - 1].snap();
            mLevels[i].commit();
            mLevels[i].prepare(app, currLedger, currLedgerProtocol, snap,
                               shadows, /*countMergeEvents=*/true);
        }
    }

    // In some testing scenarios, we want to inhibit counting level 0 merges
    // because they are not repeated when restarting merges on app startup,
    // and we are checking for an expected number of merge events on restart.
    bool countMergeEvents =
        !app.getConfig().ARTIFICIALLY_REDUCE_MERGE_COUNTS_FOR_TESTING;
    bool doFsync = !app.getConfig().DISABLE_XDR_FSYNC;
    releaseAssert(shadows.size() == 0);
    mLevels[0].prepare(app, currLedger, currLedgerProtocol,
                       Bucket::fresh(app.getBucketManager(), currLedgerProtocol,
                                     initEntries, liveEntries, deadEntries,
                                     countMergeEvents,
                                     app.getClock().getIOContext(), doFsync),
                       shadows, countMergeEvents);
    mLevels[0].commit();

    // We almost always want to try to resolve completed merges to single
    // buckets, as it makes restarts less fragile: fewer saved/restored shadows,
    // fewer buckets for the user to accidentally delete from their buckets
    // dir. Also makes publication less likely to redo a merge that was already
    // complete (but not resolved) when the snapshot gets taken.
    //
    // But we support the option of not-doing so, only for the sake of
    // testing. Note: this is nonblocking in any case.
    if (!app.getConfig().ARTIFICIALLY_PESSIMIZE_MERGES_FOR_TESTING)
    {
        resolveAnyReadyFutures();
    }
}

void
BucketList::updateStartingEvictionIterator(EvictionIterator& iter,
                                           uint32_t firstScanLevel,
                                           uint32_t ledgerSeq)
{
    // Check if an upgrade has changed the starting scan level to below the
    // current iterator level
    if (iter.bucketListLevel < firstScanLevel)
    {
        // Reset iterator to the new minimum level
        iter.bucketFileOffset = 0;
        iter.isCurrBucket = true;
        iter.bucketListLevel = firstScanLevel;
    }

    // Whenever a Bucket changes (spills or receives an incoming spill), the
    // iterator offset in that bucket is invalidated. After scanning, we
    // must write the iterator to the BucketList then close the ledger.
    // Bucket spills occur on ledger close after we've already written the
    // iterator, so the iterator may be invalidated. Because of this, we
    // must check if the Bucket the iterator currently points to changed on
    // the previous ledger, indicating the current iterator is invalid.
    if (iter.isCurrBucket)
    {
        // Check if bucket received an incoming spill
        releaseAssert(iter.bucketListLevel != 0);
        if (BucketList::levelShouldSpill(ledgerSeq - 1,
                                         iter.bucketListLevel - 1))
        {
            // If Bucket changed, reset to start of bucket
            iter.bucketFileOffset = 0;
        }
    }
    else
    {
        if (BucketList::levelShouldSpill(ledgerSeq - 1, iter.bucketListLevel))
        {
            // If Bucket changed, reset to start of bucket
            iter.bucketFileOffset = 0;
        }
    }
}

bool
BucketList::updateEvictionIterAndRecordStats(
    EvictionIterator& iter, EvictionIterator startIter,
    uint32_t configFirstScanLevel, uint32_t ledgerSeq,
    std::shared_ptr<EvictionStatistics> stats, EvictionCounters& counters)
{
    releaseAssert(stats);

    // If we reached eof in curr bucket, start scanning snap.
    // Last level has no snap so cycle back to the initial level.
    if (iter.isCurrBucket && iter.bucketListLevel != kNumLevels - 1)
    {
        iter.isCurrBucket = false;
        iter.bucketFileOffset = 0;
    }
    else
    {
        // If we reached eof in snap, move to next level
        ++iter.bucketListLevel;
        iter.isCurrBucket = true;
        iter.bucketFileOffset = 0;

        // If we have scanned the last level, cycle back to initial
        // level
        if (iter.bucketListLevel == kNumLevels)
        {
            iter.bucketListLevel = configFirstScanLevel;

            // Record then reset metrics at beginning of new eviction cycle
            stats->submitMetricsAndRestartCycle(ledgerSeq, counters);
        }
    }

    // If we are back to the bucket we started at, break
    if (iter.bucketListLevel == startIter.bucketListLevel &&
        iter.isCurrBucket == startIter.isCurrBucket)
    {
        return true;
    }

    return false;
}

void
BucketList::checkIfEvictionScanIsStuck(EvictionIterator const& evictionIter,
                                       uint32_t scanSize,
                                       std::shared_ptr<Bucket const> b,
                                       EvictionCounters& counters)
{
    // Check to see if we can finish scanning the new bucket before it
    // receives an update
    uint64_t period = bucketUpdatePeriod(evictionIter.bucketListLevel,
                                         evictionIter.isCurrBucket);
    if (period * scanSize < b->getSize())
    {
        CLOG_WARNING(Bucket,
                     "Bucket too large for current eviction scan size.");
        counters.incompleteBucketScan.inc();
    }
}

// To avoid noisy data, only count metrics that encompass a complete
// eviction cycle. If a node joins the network mid cycle, metrics will be
// nullopt and be initialized at the start of the next cycle.
void
BucketList::scanForEvictionLegacy(Application& app, AbstractLedgerTxn& ltx,
                                  uint32_t ledgerSeq,
                                  EvictionCounters& counters,
                                  std::shared_ptr<EvictionStatistics> stats)
{
    releaseAssert(stats);

    auto getBucketFromIter = [&levels = mLevels](EvictionIterator const& iter) {
        auto& level = levels.at(iter.bucketListLevel);
        return iter.isCurrBucket ? level.getCurr() : level.getSnap();
    };

    auto const& networkConfig =
        app.getLedgerManager().getSorobanNetworkConfig();
    auto const firstScanLevel =
        networkConfig.stateArchivalSettings().startingEvictionScanLevel;
    auto evictionIter = networkConfig.evictionIterator();
    auto scanSize = networkConfig.stateArchivalSettings().evictionScanSize;
    auto maxEntriesToEvict =
        networkConfig.stateArchivalSettings().maxEntriesToArchive;

    updateStartingEvictionIterator(evictionIter, firstScanLevel, ledgerSeq);

    auto startIter = evictionIter;
    auto b = getBucketFromIter(evictionIter);

    while (!b->scanForEvictionLegacy(
        ltx, evictionIter, scanSize, maxEntriesToEvict, ledgerSeq,
        counters.entriesEvicted, counters.bytesScannedForEviction, stats))
    {

        if (updateEvictionIterAndRecordStats(evictionIter, startIter,
                                             firstScanLevel, ledgerSeq, stats,
                                             counters))
        {
            break;
        }

        b = getBucketFromIter(evictionIter);
        checkIfEvictionScanIsStuck(
            evictionIter,
            networkConfig.stateArchivalSettings().evictionScanSize, b,
            counters);
    }

    networkConfig.updateEvictionIterator(ltx, evictionIter);
}

void
BucketList::restartMerges(Application& app, uint32_t maxProtocolVersion,
                          uint32_t ledger)
{
    ZoneScoped;
    for (uint32_t i = 0; i < static_cast<uint32>(mLevels.size()); i++)
    {
        auto& level = mLevels[i];
        auto& next = level.getNext();
        if (next.hasHashes() && !next.isLive())
        {
            next.makeLive(app, maxProtocolVersion, i);
            if (next.isMerging())
            {
                CLOG_INFO(Bucket, "Restarted merge on BucketList level {}", i);
            }
        }
        // The next block assumes we are re-starting a
        // FIRST_PROTOCOL_SHADOWS_REMOVED or later merge, which has no shadows
        // _and_ no stored inputs/outputs, and ensures that we are only
        // re-starting buckets of correct version.
        else if (next.isClear() && i > 0)
        {
            // Recover merge by iterating through bucketlist levels and
            // using snaps and currs. The only time we don't use level's
            // curr is when a level is about to snap; in that case, when the
            // merge is needed, level's curr will snap, and merge will be
            // promoted into curr. Therefore, when starting merge, we use an
            // empty curr.
            // Additionally, it is safe to recover a merge at any point
            // before the merge is needed (meaning it should be promoted
            // into level's curr after ledger close). This is due to the
            // fact that future bucket inputs _do not change_ until level
            // spill, and after such spills, new merges are started with new
            // inputs.
            auto snap = mLevels[i - 1].getSnap();

            // Exit early if a level has not been initialized yet;
            // There are two possibilities for empty buckets: it is either truly
            // untouched (meaning not enough ledgers were produced to populate
            // given level) or it's a protocol 10-or-earlier bucket (since it
            // does not contain a metadata entry). If we are dealing with
            // 10-or-earlier bucket, it must have had an output published, and
            // would be handled in the previous `if` block. Therefore, we must
            // be dealing with an untouched level.
            if (snap->isEmpty())
            {
                return;
            }

            auto version = Bucket::getBucketVersion(snap);
            if (protocolVersionIsBefore(version,
                                        Bucket::FIRST_PROTOCOL_SHADOWS_REMOVED))
            {
                auto msg = fmt::format(
                    FMT_STRING("Invalid state: bucketlist level {:d} has clear "
                               "future bucket but version {:d} snap"),
                    i, version);
                throw std::runtime_error(msg);
            }

            // Round down the current ledger to when the merge was started, and
            // re-start the merge via prepare, mimicking the logic in `addBatch`
            auto mergeStartLedger =
                roundDown(ledger, BucketList::levelHalf(i - 1));
            level.prepare(
                app, mergeStartLedger, version, snap, /* shadows= */ {},
                !app.getConfig().ARTIFICIALLY_REDUCE_MERGE_COUNTS_FOR_TESTING);
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
