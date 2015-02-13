#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <future>
#include "generated/StellarXDR.h"
#include "xdrpp/message.h"

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
//    1. [key:hash:object] tuples are added in _batches_, each batch 1/16 the
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
// "a while"; 16x as long on each level. And snap(i) only has to
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
// Define size(i) = 1 << (4*(i+1))
// Define half(i) = size(i) >> 1
// Define prev(i) = size(i-1)
//
// Then if ledger number is k,
//
// Define levels(k) = ceil(log_16(k))
//
// Each level holds objects changed _in some range of ledgers_.
//
// for i in range(0, levels(k)):
//   curr(i) covers range (mask(k,half(i)), mask(k,prev(i))]
//   snap(i) covers range (mask(k,size(i)), mask(k,half(i))]
//
// Every time k increases, it must create a snapshot (promoting curr(i) to
// snap(i) and evicting snap(i)) at any level i where the size of the actual
// ledger range represented in curr(i) exceeds half(i). This should be
// equivalent to saying "snapshot+evict any level at which the ith hex digit of
// k changed from f to 0 or from 7 to 8".
//
//
// Example:
// --------
//
// Suppose we're on ledger 0x56789ab (decimal 90,671,531)
//
// We immediately know that we could represent the bucket list for this in 7
// levels, because the ledger number has 7 hex digits (alternatively:
// ceil(log_16(ledger)) == 7). It turns out (see below re: "degeneracy")
// that we won't use all 7, but for now let's imagine we did:
//
// The levels would then hold objects changed in the following ranges:
//
// level[0]  curr=(0x56789a8, 0x56789ab], snap=(0x56789a0, 0x56789a8]
// level[1]  curr=(0x5678980, 0x56789a0], snap=(0x5678900, 0x5678980]
// level[2]  curr=(0x5678800, 0x5678900], snap=(0x5678000, 0x5678800]
// level[3]  curr= ------ empty ------  , snap=(0x5670000, 0x5678000]
// level[4]  curr=(0x5600000, 0x5670000], snap= ------ empty ------
// level[5]  curr=(0x5000000, 0x5600000], snap= ------ empty ------
// level[6]  curr=(0x0, 0x5000000],       snap= ------ empty ------
//
// Assuming a ledger closes every 5 seconds, here are the timespans
// covered by each level:
//
// L0: 80 seconds  (16 ledgers)
// L1: 21.3 min    (256 ledgers)
// L2: 5.7 hours   (4,096 ledgers)
// L3: 3.8 days    (65,536 ledgers)
// L4: 60.7 days   (1,048,576 ledgers)
// L5: 2.66 years  (16,777,216 ledgers)
// L6: 42.5 years  (268,435,456 ledgers)
// L7: 681 years   (4,294,967,296 ledgers)
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
// memory. So levels 0, 1, 2. Levels 3 and 4 must go on disk.
//
// How fast do we need to rewrite/rehash level 3? We rewrite it every time
// snap(2) is evicted. This happens every 2048 ledgers. That's 10240 seconds, or
// 2.8 hours. So we have >2 hours to do a sequential read + merge + hash of a
// half of level 3 (32,768 ledgers' worth of objects), or 83GB of data (at 1,000
// tx/s). Amazon EBS disks sustain sequential writes at 30MB/s, so it should
// take ~46 minutes, giving us a safety margin of ~3x.
//
// It _does_ mean that at this scale, we will be _doing_ an 83GB sequential
// read/merge/hash/write every 2.8 hours. That might cause a noticeable I/O spike
// and/or some degree of I/O wear. But the hardware should keep up and this is,
// after all, the 1000 tx/s "great success" situation where we're doing some
// substantial tuning, having scaled through three orders of magnitude.
//
// It also means we're doing a ~1.3TB sequential write once every ~45 hours, as
// we evict a snap from level 3 to level 4. But again, any storage device that's
// actually comfortable slinging around 1.4TB files (and we will have that much
// data to hash, period) should be able to process it in much less than 45
// hours; at 30MB/s it should take less than 13h (safety margin ~3x).
//
// It is possible to increase the safety margin by several possible means:
// either buying physical hardware (commodity SSDs sustain writes in the 400MB/s
// range), by using instance storage rather than EBS (measured around 100MB/s
// write on disk, 250MB/s on SSD) or by using striping across EBS or instance
// volumes. If none of these are acceptable, the data structure can also be
// modified to split each bucket into (say) 8 sub-buckets, Merkle style, and
// combine the hashes of each; essentially performing the striping "in the data
// structure". Though this would effect the hash values so should be done
// earlier in the design process, if at all.
//
//
// Degeneracy and limits:
// ----------------------
//
// Beyond level 4, a certain degeneracy takes over. "Objects changed over the
// course of a million ledgers" starts to sound like "the entire database": if
// we're getting 10,000 objects changed per ledger, a million ledgers means
// touching 10 billion objects. It's unlikely that we're going to have _much_
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
// We therefore cut off at level 4. Level 5 doesn't exist: it's "the entire
// database", which we update with a half-level-4 snapshot every half-million
// ledgers. Which is "once a month" (at 5s per ledger).
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

/**
 * Bucket is an immutable container for a sorted set of "Entries" (object ID,
 * hash, xdr-message tuples) which is designed to be held in a shared_ptr<>
 * which is referenced between threads, to minimize copying. It is therefore
 * imperative that it be _really_ immutable, not just faking it.
 *
 * Two buckets can be merged together efficiently (in a single pass): elements
 * from the newer bucket overwrite elements from the older bucket, the rest are
 * merged in sorted order, and all elements are hashed while being added.
 */

class Bucket : public std::enable_shared_from_this<Bucket>
{

  private:

    // Assuming an object is ~256 bytes and we might have as many as 100 buckets
    // in memory at a time (due to history operations, hashing, and the
    // naturally-occurring 10 buckets we get from 5 levels): Allocating a 1GB
    // memory cap gives us 10MB per bucket. Rounding down a touch to 2^23 bytes
    // (8MB) gives us 2^15 objects before we spill to disk.
    static size_t const kMaxMemoryObjectsPerBucket = 1 << 15;

    // If mSpilledToFile, then filename is empty, mEntries contains entries;
    // else filename is non-empty, it names an XDR file with entries and
    // mEntries is empty. In both cases mHash is valid.
    bool const mSpilledToFile;
    std::vector<CLFEntry> const mEntries;
    uint256 const mHash;
    std::string mFilename;

  public:

    class InputIterator;
    class OutputIterator;

    Bucket();
    ~Bucket();
    Bucket(std::string const& tmpDir,
           std::vector<CLFEntry> const& entries,
           uint256 const& hash);
    Bucket(std::string const& filename, uint256 const& hash);
    std::vector<CLFEntry> const& getEntries() const;
    uint256 const& getHash() const;
    std::string const& getFilename() const;
    bool isSpilledToFile() const;
    bool containsCLFIdentity(CLFEntry const& id) const;

    static std::shared_ptr<Bucket>
    fresh(std::string const& tmpDir,
          std::vector<LedgerEntry> const& liveEntries,
          std::vector<LedgerKey> const& deadEntries);

    static std::shared_ptr<Bucket>
    merge(std::string const& tmpDir,
          std::shared_ptr<Bucket> const& oldBucket,
          std::shared_ptr<Bucket> const& newBucket,
          std::vector<std::shared_ptr<Bucket>> const& shadows =
          std::vector<std::shared_ptr<Bucket>>());
};

class BucketLevel
{
    size_t mLevel;
    std::future<std::shared_ptr<Bucket>> mNextCurr;
    std::shared_ptr<Bucket> mCurr;
    std::shared_ptr<Bucket> mSnap;

  public:
    BucketLevel(size_t i);
    uint256 getHash() const;
    std::shared_ptr<Bucket> getCurr() const;
    std::shared_ptr<Bucket> getSnap() const;
    void commit();
    void prepare(Application& app, uint64_t currLedger,
                 std::shared_ptr<Bucket> snap,
                 std::vector<std::shared_ptr<Bucket>> const& shadows);
    std::shared_ptr<Bucket> snap();
};

class BucketList
{
    std::vector<BucketLevel> mLevels;

  public:
    static uint64_t levelSize(size_t level);
    static uint64_t levelHalf(size_t level);
    static bool levelShouldSpill(uint64_t ledger, size_t level);
    static uint64_t mask(uint64_t v, uint64_t m);
    static size_t numLevels(uint64_t ledger);

    // BucketList _just_ stores a set of entries; anything else the CLF
    // wants to support should happen in another class. These operations form a
    // minimal, testable interface to BucketList.
    size_t numLevels() const;
    BucketLevel const& getLevel(size_t i) const;
    uint256 getHash() const;
    void addBatch(Application& app, uint64_t currLedger,
                  std::vector<LedgerEntry> const& liveEntries,
                  std::vector<LedgerKey> const& deadEntries);
};

}

