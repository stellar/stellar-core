#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/FutureBucket.h"
#include "overlay/StellarXDR.h"
#include "xdrpp/message.h"
#include <future>

namespace stellar
{
// This is the "bucket list", a set sets-of-hashed-objects, organized into
// temporal "levels", with older levels being larger and changing less
// frequently. The purpose of this data structure is twofold:
//
//  1. to _effectively_ provide a "single summary hash" of a rapidly-changing
//     database without actually having to rehash the database on each change.
//
//  2. to provide a relatively cheap (in terms of bytes transferred) "catch-up"
//     operation, when a node has been offline for a period of time and wants to
//     synchronize its (database) ledger with the network's current ledger.
//
// In a sense it plays a role analogous to the "SHAMap" Merkle radix trie in
// Ripple, the difference being that the SHAMap has to do quite a lot of random
// seeking and rewriting objects as new values arrive: they are distributed at
// random in the trie's leaves, and so in general if K objects have to change
// among a set of N leaves, K trie leaves (each 16x the size of a single entry)
// need to be rewritten/rehashed, as well as K * log_16(N) interior nodes,
// meaning on (say) a million-object database, 6*K random seeks and 16*6*K bytes
// hashed and, more seriously, stored _permanently_ in the Ripple NodeStore, for
// revisiting that ledger. The goal of the "bucket list" here, relative to the
// SHAMap in Ripple, is to avoid the random I/O and reduce write amplification,
// while maintaining a degree of "not rehashing stuff too often if it didn't
// change".
//
// It is also somewhat decoupled from transaction processing; modified objects
// are written to it in serial form (for hashing and history storage) but the
// deserialized, "live" state of each object, against-which transactions are
// applied, is stored in the RDBMS.
//
// The approach we take is, rather than key-prefix / trie style partitioning,
// _temporal_ leveling, taking a cue from the design of log-structured merge
// trees (LSM-trees) such as those used in LevelDB. Within each temporal level,
// any time a subset of [key:hash:object] tuples are modified, the whole level
// is rehashed. But we arrange for three conditions that make this tolerable:
//
//    1. [key:hash:object] tuples are added in _batches_, each batch 1/4 the
//       size of the level. Rehashing only happens after a _batch_ is added.
//
//    2. The batches are propagated at frequencies that slow down in proportion
//       to the size of the level / size of the batches.
//
//    3. The batching is actually done in "half-levels", each of which is
//       _stable_ for period of time between batch-adds. That is, there's enough
//       time to merge in each batch, _and_ the batch being added is itself a
//       fixed snapshot, so no locking is required. Just a spare thread to do
//       the work.
//
// The "overall" hash of the bucket list is formed by hashing the concatenation
// of the hashes making up each level. The tricky part is the batch propagation.
//
//
// Intuitively:
// ------------
//
// Each level i is split in 2 halves, curr(i) and snap(i). At any moment,
// snap(i) is either empty or contains exactly size(i)/2 ledgers worth of
// objects.
//
// curr(i) has some number between 0 and half(i)=size(i)/2 worth of ledgers in
// it. When it gets to size(i)/2, it is snapshotted as snap(i), and the existing
// snap(i) is "instantaneously" merged into curr(i+1).
//
// In reality each snap(i) is evicted once every half(i) ledgers, which will be
// "a while"; 4x as long on each level. And snap(i) only has to
// "instantaneously" update curr(i+1) when it's evicted. So the moment snap(i)
// is initially _formed_, it forks a background thread to start merging its
// contents with curr(i+1). So long as it completes that work before it's
// officially evicted, it can "instantaneously" swap-in the merged value in
// place of curr(i+1) when its eviction occurs.
//
// This intuition is, directly, how the code is written, and having read it you
// can edit the code and accept on faith that it "happens to work". The
// remainder of this comment is just analysis and arithmetic of which values are
// flowing where, at what rate and cost, to help convince you (and the author)
// that it does what it's supposed to and can keep up with the performance
// goals and memory budget.
//
//
// Formally:
// ---------
//
// Define mask(v,m) = (v & ~(m-1))
// Define size(i) = 1 << (2*(i+1))
// Define half(i) = size(i) >> 1
// Define prev(i) = size(i-1)
//
// Then if ledger number is k,
//
// Define levels(k) = ceil(log_4(k))
//
// In the original design of the bucket list, we intended that each level would
// hold objects changed _in some range of ledgers_.
//
// for i in range(0, levels(k)):
//   curr(i) covers range (mask(k,half(i)), mask(k,prev(i))]
//   snap(i) covers range (mask(k,size(i)), mask(k,half(i))]
//
// In practice, the final implementation we settled on wound up having a sort of
// off-by-one error in the initial population of each level (see "initial level
// skews" below), so those ranges fail to hold. But they're _close_, just off by
// an unfortunate error term.
//
// Every time k increases, it must create a snapshot (promoting curr(i) to
// snap(i) and evicting snap(i)) at any level i where the size of the actual
// ledger range represented in curr(i) exceeds half(i).
//
//
// Example:
// --------
//
// Suppose we're on ledger 0x11_f9ab (decimal 1,178,027)
//
// This will occupy 10 levels of bucket list. Here they are (this is taken from
// measuring actual buckets in the current code, and is accurate -- previous
// calculations were off by a bit):
//
// level           curr                             snap
//            size=[lo_ledger, hi_ledger]      size=[lo_ledger, hi_ledger]
// =========================================================================
//   0         0x2=[0x11_f9aa, 0x11_f9ab]       0x2=[0x11_f9a8, 0x11_f9a9]
//   1         0x4=[0x11_f9a4, 0x11_f9a7]       0x8=[0x11_f99c, 0x11_f9a3]
//   2        0x10=[0x11_f98c, 0x11_f99b]      0x20=[0x11_f96c, 0x11_f98b]
//   3        0x40=[0x11_f92c, 0x11_f96b]      0x80=[0x11_f8ac, 0x11_f92b]
//   4       0x200=[0x11_f6ac, 0x11_f8ab]     0x200=[0x11_f4ac, 0x11_f6ab]
//   5       0x200=[0x11_f2ac, 0x11_f4ab]     0x800=[0x11_eaac, 0x11_f2ab]
//   6      0x2000=[0x11_caac, 0x11_eaab]    0x2000=[0x11_aaac, 0x11_caab]
//   7      0x8000=[0x11_2aac, 0x11_aaab]    0x8000=[0x10_aaac, 0x11_2aab]
//   8    0x2_0000=[ 0xe_aaac, 0x10_aaab]   0x20000=[ 0xc_aaac,  0xe_aaab]
//   9    0x2_0000=[ 0xa_aaac,  0xc_aaab]   0x80000=[ 0x2_aaac,  0xa_aaab]
//  10    0x2_aaab=[      0x1,  0x2_aaab]   ---------- empty -----------
//
// The sizes of the "snap" buckets _for levels that are full_ correspond exactly
// to the values of the levelHalf() function: (4^(level+1))/2. I.e. 0x2, 0x8,
// 0x20, 0x80, 0x200, etc.
//
// The sizes of the "curr" buckets _for levels that are full_ cycle over time
// between 1 and 4 times the size of levelHalf() of the _previous_ level (where
// 4x the previous levelHalf is the _current_ level's levelHalf).
//
// For example, the curr bucket on level 5 grows in units of 0x200 ledgers, as
// snap buckets of size 0x200 spill in from level 4. In the picture above it is
// currently holding 0x200 ledgers-worth, and it will cycle through 0x400, 0x600
// and 0x800 ledgers before returning to 0x200. When it holds 0x800 it is
// "full", and the next spill from level 4 will cause level 5 curr to snapshot,
// to become level 5 snap, and _only then_ will level 5 curr be replaced by the
// newly arrived 0x200-ledger spill from level 4.
//
//
// Initial ledger skews (a.k.a. "why do they all end in ...aaaab?"):
// -----------------------------------------------------------------
//
// For levels that _are not full_ (which only happens at the level holding the
// oldest bucket in the bucket list, as it grows, eg. level 10's curr in the
// table above) unfortunately the picture is more complex: each curr is
// essentially the sum of the "last 4 snaps" from its previous level, and those
// start out _empty_ on each level, only filling in as snaps from earlier-still
// levels (that also started empty) spill into them, fill up their curr, and get
// snap'ed, etc. So each level's buckets take exponentially-longer to "ramp up"
// to being full:
//
//    - level 0 is spilling full (2-ledger) snaps only after ledger 4,
//    - level 1 is spilling full (8-ledger) snaps only after ledger 16,
//    - level 2 is spilling full (32-ledger) snaps only after ledger 64,
//
// and so forth. So during the filling-up period, each curr and snap is at some
// degenerate size less than the expected size, and spanning a range that's
// offset from the expected range by the sum of all the size-degeneracies that
// occurred up to the present. The exact recurrence defining the sizes and range
// offsets of these "filling-up buckets" is a bit complex (see sizeOfCurr and
// sizeOfSnap) and beside the point. But for example, in the full bucketlist
// example table above, the level 10 curr at ledger 0x11f9ab only spans 0x2aaab
// ledgers, rather than a multiple-of-0x80000 as one would expect.
//
// Unfortunately even after the bucketlist fills up in its entirety, and most
// buckets have their expected _sizes_, every bucket's _ledger range_ will
// retain this weird offset from the elegant powers-of-4 boundaries one would
// want, as residue from its "filling up" period. Moreover the oldest bucket
// will forever have a "weird size" that includes the residue from the
// filling-up period. This state of affairs is unfortunately wired-in to the
// design now; an unforeseen consequence of choices made when translating the
// on-paper design to code that actually deals with initial conditions, and an
// oversight to have not tested-for specifically. Apologies for the resulting
// "ugly" ledger spans.
//
//
// Time intuitions:
// ----------------
//
// Assuming a ledger closes every 5 seconds, here are the timespans covered by
// each level:
//
// L0:   20 seconds          (4 ledgers)
// L1:   80 seconds         (16 ledgers)
// L2:    5 minutes         (64 ledgers)
// L3:   21 minutes        (256 ledgers)
// L4:   85 minutes      (1,024 ledgers)
// L5:    5 hours        (4,096 ledgers)
// L6:   22 hours       (16,384 ledgers)
// L7:    3 days        (65,536 ledgers)
// L8:   15 days       (262,144 ledgers)
// L9:   60 days     (1,048,576 ledgers)
// L10: 242 days     (4,194,304 ledgers)
// L11:   2 years   (16,777,216 ledgers)
// L12:  10 years   (67,108,864 ledgers)
// L13:  42 years  (268,435,456 ledgers)
//
//
// Performance:
// ------------
//
// Assume we're receiving 1,000 transactions / second, and each is changing 2
// objects, and ledgers take 5s to close, i.e. we're adding 10,000 objects /
// ledger to this data structure. Assuming the 10GB allocated to in-memory
// buckets, if each object is on average 256 bytes then we can store 4 objects
// per kb or 40 million objects in memory, or 4,000 (say: 4,096) ledgers in
// memory. So levels 0 to 5 can be in memory, the remainder must go on disk.
//
// How fast do we need to rewrite/rehash level 6? We rewrite it every time
// snap(5) is evicted. This happens every 2048 ledgers. That's 10240 seconds, or
// 2.8 hours. So we have >2 hours to do a sequential read + merge + hash of a
// half of level 6 (8,192 ledgers' worth of objects), or 20GB of data (at 1,000
// tx/s). Amazon EBS disks sustain sequential writes at 30MB/s, so it should
// take ~11 minutes, giving us a safety margin of ~15x.
//
// It _does_ mean that at this scale, we will be _doing_ an 20GB sequential
// read/merge/hash/write every 2.8 hours. That might cause a noticeable I/O
// spike and/or some degree of I/O wear. But the hardware should keep up and
// this is, after all, the 1000 tx/s "great success" situation where we're doing
// some substantial tuning, having scaled through three orders of magnitude.
//
// It also means we're doing a ~78GB sequential write once every ~11 hours, as
// we evict a snap from level 6 to level 7. But again, any storage device that's
// actually comfortable slinging around 78GB files (and we will have that much
// data to hash, period) should be able to process it in much less than 11
// hours; at 30MB/s it should take less than 45 minutes (safety margin ~15x).
//
// It is possible to increase the safety margin by several possible means:
// either buying physical hardware (commodity SSDs sustain writes in the 400MB/s
// range), by using instance storage rather than EBS (measured around 100MB/s
// write on disk, 250MB/s on SSD) or by using striping across EBS or instance
// volumes.
//
//
// Degeneracy and limits:
// ----------------------
//
// Beyond level 10, a certain degeneracy takes over. "Objects changed over the
// course of 4 million ledgers" starts to sound like "the entire database": if
// we're getting 10,000 objects changed per ledger, 4 million ledgers means
// touching 40 billion objects. It's unlikely that we're going to have _much_
// more objects than that over the long term; in particular we're unlikely to
// bump up against 160 billion objects anytime soon, which is where the next
// level would be. There are only so many humans on earth. Having more levels
// just produces "dead weight" levels that contain copies of the old database
// and are completely shadowed by levels above them.
//
// Moreover, even if we are dealing with (say) IoT smart toasters rather than
// humans -- thus we have trillions of accounts -- we want the data structure to
// "garbage collect" transient undesirable state at some reasonable frequency;
// if there was a bug, a temporary bulge in the object count, or similar
// transient condition, we'd like there to be a "last" level beyond which
// history is cut off, collapsed into a single "... and so on" snapshot of all
// history before: the state of the _full_ database at a certain point in the
// past, with no implied dependence on deeper history beneath it.
//
// We therefore cut off at level 10. Level 11 doesn't exist: it's "the entire
// database", which we update with a half-level-10 snapshot every 2-million
// ledgers. Which is "every 4 months" (at 5s per ledger).
//
// Cutting off at a fixed level carries a minor design risk: that the database
// might grow very large, relative to the transaction volume, and that we might
// therefore burden certain peers (those that have been offline longer than a
// month) with having to download a (large) full-database snapshot when, had we
// added an additional level, they might have got away with "only downloading a
// level", as a delta. We consider this risk acceptable, in light of the
// multiple tradeoffs competing in this design. Most peers with transient
// connectivity problems will be offline for less than a month if they are
// participating in the system, and we expect transaction volume and database
// size to scale in a quasi-linear fashion. In any case it only causes such a
// peer to artificially degenerate to "new peer syncing for the first time"
// behavior, which ought to be tolerably fast anyways.

class Application;
class Bucket;

namespace testutil
{
class BucketListDepthModifier;
}

class BucketLevel
{
    uint32_t mLevel;
    FutureBucket mNextCurr;
    std::shared_ptr<Bucket> mCurr;
    std::shared_ptr<Bucket> mSnap;

  public:
    BucketLevel(uint32_t i);
    uint256 getHash() const;
    FutureBucket const& getNext() const;
    FutureBucket& getNext();
    std::shared_ptr<Bucket> getCurr() const;
    std::shared_ptr<Bucket> getSnap() const;
    void setNext(FutureBucket const& fb);
    void setCurr(std::shared_ptr<Bucket>);
    void setSnap(std::shared_ptr<Bucket>);
    void commit();
    void prepare(Application& app, uint32_t currLedger,
                 uint32_t currLedgerProtocol, std::shared_ptr<Bucket> snap,
                 std::vector<std::shared_ptr<Bucket>> const& shadows,
                 bool countMergeEvents);
    std::shared_ptr<Bucket> snap();
};

// NOTE: The access specifications for this class have been carefully chosen to
//       make it so BucketList::kNumLevels can only be modified from
//       BucketListDepthModifier -- not even BucketList can modify it. Please
//       use care when modifying this class.
class BucketListDepth
{
    uint32_t mNumLevels;

    BucketListDepth& operator=(uint32_t numLevels);

  public:
    BucketListDepth(uint32_t numLevels);

    operator uint32_t() const;

    friend class testutil::BucketListDepthModifier;
};

class BucketList
{
    // Helper for calculating `levelShouldSpill`
    static uint32_t mask(uint32_t v, uint32_t m);
    std::vector<BucketLevel> mLevels;

  public:
    // Number of bucket levels in the bucketlist. Every bucketlist in the system
    // will have this many levels and it effectively gets wired-in to the
    // protocol. Careful about changing it.
    static BucketListDepth kNumLevels;

    static bool shouldMergeWithEmptyCurr(uint32_t ledger, uint32_t level);

    // Returns size of a given level, in ledgers.
    static uint32_t levelSize(uint32_t level);

    // Returns half the size of a given level, in ledgers.
    static uint32_t levelHalf(uint32_t level);

    // Returns the size of curr on a given level and ledger, in ledgers,
    // assuming that every ledger is present.
    static uint32_t sizeOfCurr(uint32_t ledger, uint32_t level);

    // Returns the size of snap on a given level and ledger, in ledgers,
    // assuming that every ledger is present.
    static uint32_t sizeOfSnap(uint32_t ledger, uint32_t level);

    // Returns the oldest ledger in curr on a given level and ledger,
    // assuming that every ledger is present.
    static uint32_t oldestLedgerInCurr(uint32_t ledger, uint32_t level);

    // Returns the oldest ledger in snap on a given level and ledger,
    // assuming that every ledger is present.
    static uint32_t oldestLedgerInSnap(uint32_t ledger, uint32_t level);

    // Returns true if, at a given point-in-time (`ledger`), a given `level`
    // should spill curr->snap and start merging snap into its next level.
    static bool levelShouldSpill(uint32_t ledger, uint32_t level);

    // Returns true if at given `level` dead entries should be kept.
    static bool keepDeadEntries(uint32_t level);

    // Create a new BucketList with every `kNumLevels` levels, each with
    // an empty bucket in `curr` and `snap`.
    BucketList();

    // Return level `i` of the BucketList.
    BucketLevel const& getLevel(uint32_t i) const;

    // Return level `i` of the BucketList.
    BucketLevel& getLevel(uint32_t i);

    // Return a cumulative hash of the entire bucketlist; this is the hash of
    // the concatenation of each level's hash, each of which in turn is the hash
    // of the concatenation of the hashes of the `curr` and `snap` buckets.
    Hash getHash() const;

    // Restart any merges that might be running on background worker threads,
    // merging buckets between levels. This needs to be called after forcing a
    // BucketList to adopt a new state, either at application restart or when
    // catching up from buckets loaded over the network.

    // There are two ways to re-start a merge:
    // 1. Use non-LIVE inputs/outputs from HAS file. This function will make
    // these FutureBuckets LIVE (either FB_INPUT_LIVE or FB_OUTPUT_LIVE)
    // using hashes of inputs and outputs from the HAS
    // 2. Introduced with FIRST_PROTOCOL_SHADOWS_REMOVED: skip using
    // input/output hashes, and restart live merges from currs and snaps of the
    // bucketlist at that ledger.
    void restartMerges(Application& app, uint32_t maxProtocolVersion,
                       uint32_t ledger);

    // Run through the levels and check for FutureBuckets that are done merging;
    // if so, call resolve() on them, changing state from FB_LIVE_INPUTS to
    // FB_LIVE_OUTPUT. This helps eagerly release no-longer-needed inputs and
    // avoid propagating partially-completed BucketList states to
    // HistoryArchiveStates, that can cause repeated merges when re-activated.
    void resolveAnyReadyFutures();

    // returns true if levels [0, maxLevel] are resolved
    bool futuresAllResolved(uint32_t maxLevel = kNumLevels - 1) const;

    // returns the largest level that this ledger will need to merge
    uint32_t getMaxMergeLevel(uint32_t currLedger) const;

    // Add a batch of initial (created), live (updated) and dead entries to the
    // bucketlist, representing the entries effected by closing
    // `currLedger`. The bucketlist will incorporate these into the smallest
    // (0th) level, as well as commit or prepare merges for any levels that
    // should have spilled due to passing through `currLedger`. The `currLedger`
    // and `currProtocolVersion` values should be taken from the ledger at which
    // this batch is being added.
    void addBatch(Application& app, uint32_t currLedger,
                  uint32_t currLedgerProtocol,
                  std::vector<LedgerEntry> const& initEntries,
                  std::vector<LedgerEntry> const& liveEntries,
                  std::vector<LedgerKey> const& deadEntries);
};
}
