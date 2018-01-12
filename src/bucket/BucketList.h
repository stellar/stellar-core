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
// Each level holds objects changed _in some range of ledgers_.
//
// for i in range(0, levels(k)):
//   curr(i) covers range (mask(k,half(i)), mask(k,prev(i))]
//   snap(i) covers range (mask(k,size(i)), mask(k,half(i))]
//
// Every time k increases, it must create a snapshot (promoting curr(i) to
// snap(i) and evicting snap(i)) at any level i where the size of the actual
// ledger range represented in curr(i) exceeds half(i).
//
//
// Example:
// --------
//
// Suppose we're on ledger 0x56789ab (decimal 90,671,531)
//
// We immediately know that we could represent the bucket list for this in 14
// levels, because the ledger number has 7 hex digits (alternatively:
// ceil(log_4(ledger)) == 14). It turns out (see below re: "degeneracy")
// that we won't use all 14, but for now let's imagine we did:
//
// The levels would then hold objects changed in the following ranges:
//
// level[0]  curr=(0x56789aa, 0x56789ab],  snap=(0x56789a8, 0x56789aa]
// level[1]  curr= ------ empty ------- ,  snap=(0x56789a0, 0x56789a8]
// level[2]  curr= ------ empty ------- ,  snap=(0x5678980, 0x56789a0]
// level[3]  curr= ------ empty ------- ,  snap=(0x5678900, 0x5678980]
// level[4]  curr=(0x5678800, 0x5678900],  snap= ------ empty -------
// level[5]  curr= ------ empty ------- ,  snap=(0x5678000, 0x5678800]
// level[6]  curr= ------ empty ------- ,  snap= ------ empty -------
// level[7]  curr= ------ empty ------- ,  snap=(0x5670000, 0x5678000]
// level[8]  curr=(0x5660000, 0x5670000],  snap=(0x5640000, 0x5660000]
// level[9]  curr=(0x5600000, 0x5640000],  snap= ------ empty -------
// level[10] curr= ------ empty ------- ,  snap=(0x5400000, 0x5600000]
// level[11] curr=(0x5000000, 0x5400000],  snap= ------ empty -------
// level[12] curr=(0x4000000, 0x5000000],  snap= ------ empty -------
// level[13] curr=(0x0, 0x4000000],        snap= ------ empty -------
//
// Assuming a ledger closes every 5 seconds, here are the timespans
// covered by each level:
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
                 std::shared_ptr<Bucket> snap,
                 std::vector<std::shared_ptr<Bucket>> const& shadows);
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
    BucketLevel& getLevel(uint32_t i);

    // Return a cumulative hash of the entire bucketlist; this is the hash of
    // the concatenation of each level's hash, each of which in turn is the hash
    // of the concatenation of the hashes of the `curr` and `snap` buckets.
    Hash getHash() const;

    // Restart any merges that might be running on background worker threads,
    // merging buckets between levels. This needs to be called after forcing a
    // BucketList to adopt a new state, either at application restart or when
    // catching up from buckets loaded over the network.
    void restartMerges(Application& app);

    // Add a batch of live and dead entries to the bucketlist, representing the
    // entries effected by closing `currLedger`. The bucketlist will incorporate
    // these into the smallest (0th) level, as well as commit or prepare merges
    // for any levels that should have spilled due to passing through
    // `currLedger`.
    void addBatch(Application& app, uint32_t currLedger,
                  std::vector<LedgerEntry> const& liveEntries,
                  std::vector<LedgerKey> const& deadEntries);
};
}
