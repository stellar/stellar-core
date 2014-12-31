#ifndef __BUCKETLIST__
#define __BUCKETLIST__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <future>
#include "generated/StellarXDR.h"
#include "CanonicalLedgerForm.h"

namespace stellar
{
// This is the "bucket list", a set sets-of-hashes, organized into temporal
// "levels", with older levels being larger and changing less frequently. The
// purpose of this data structure is to _effectively_ provide a "single summary
// hash" of a rapidly-changing database without actually having to rehash the
// database on each change.
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
// It is also completely decoupled from the storage of actual objects; this
// structure just calculates hashes-of-hashes. Object storage is in an RDBMS.
//
// The approach we take is, rather than key-prefix / trie style partitioning,
// _temporal_ leveling, taking a cue from the design of log-structured merge
// trees (LSM-trees) such as those used in LevelDB. Within each temporal level,
// any time a subset of [key:hash] pairs are modified, the whole level is
// rehashed. But we arrange for three conditions that make this tolerable:
//
//    1. key:hash pairs are added in _batches_, each batch 1/16 the size of
//       the level. Rehashing only happens after a _batch_ is added.
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
// Each level holds hashes of objects changed _in some range of ledgers_.
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
// We immediately know that we have 7 levels, because the ledger number has 7
// hex digits (alternatively: ceil(log_16(ledger)) == 7)
//
// The levels hold hashes of objects changed in the following ranges:
//
// level[0]  curr=(0x56789a8, 0x56789ab], snap=(0x56789a0, 0x56789a8]
// level[1]  curr=(0x5678980, 0x56789a0], snap=(0x5678900, 0x5678980]
// level[2]  curr=(0x5678800, 0x5678900], snap=(0x5678000, 0x5678800]
// level[3]  curr= ------ empty ------  , snap=(0x5670000, 0x5678000]
// level[4]  curr=(0x5600000, 0x5670000], snap= ------ empty ------
// level[5]  curr=(0x5000000, 0x5600000], snap= ------ empty ------
// level[6]  curr=(0x0, 0x5000000],       snap= ------ empty ------
//
//
// L0: 80 seconds  (16 ledgers @ 5sec/ledger)
// L1: 21.3 min    (256 ledgers)
// L2: 5.7 hours   (4,096 ledgers)
// L3: 3.8 days    (65,536 ledgers)
// L4: 60.7 days   (1,048,576 ledgers)
// L5: 2.66 years  (16,777,216 ledgers)
// L6: 42.5 years  (268,435,456 ledgers)
// L7: 681 years   (4,294,967,296 ledgers)

// Performance:
// ------------
//
// Assume we're receiving 1000 transactions / second, and each is changing 2
// objects, and ledgers take 5s to close, i.e. we're adding 10000 hashes /
// ledger to this data structure. Assuming the 4GB allocated to in-memory
// buckets, below, we can keep 82 million hashes in memory, which is 8200
// ledgers, which means we're keeping levels 0-2 (sizes: 16, 256 and 4096
// ledgers, respectively) in RAM and levels 3 and up on disk.
//
// How fast do we need to rewrite level 3? We rewrite it every time snap(2) is
// evicted. This happens every 2048 ledgers. That's 10240 seconds, or 2.8
// hours. So we have >2 hours to do a sequential read + merge + hash of level
// 3. Level 3 contains 65,536 ledgers' worth of hashes, or 15GB of
// data. Commodity storage now can do sequential reads at 200MB/s (disk) or
// 500MB/s (SSD), so it should take (at worst) ~75 seconds, giving us a safety
// margin of ~100x.
//
// It _does_ mean that at this scale, we will be _doing_ a 15GB sequential
// read/merge/hash/write every 2.8 hours. That might cause a noticable I/O spike
// and/or some degree of I/O wear. But the hardware should keep up and this is,
// after all, the 1000 tx/s "great success" situation. People running validators
// can probably afford a few SSDs at that point.
//
// It also means we're doing a ~256GB sequential read once every ~45 hours, as
// we evict a snap from level 3 to level 4. But again, any storage device that's
// actually comfortable slinging around 256GB files (and we will have that much
// data to hash, period) should be able to serve it to you in much less than 45
// hours.
//
//
// Degeneracy:
// -----------
//
// As you get into bigger and bigger levels, you will start to merge more and
// more updates-to-the-same-objects into the level, and the level will not take
// up its theoretical maximum size in terms of ledger-churn-rate so much as
// it'll start to approximate the set of "all the objects in the system". For
// the sake of implementation simplicity, we don't try to track this
// phenomenon. It's just a natural size-limiting condition the levels will run
// into.
//
// For example, at level 7 you'll have room for (potentially) the set of objects
// changed in the previous 200 million ledgers; besides the fact that that's 42
// years of ledger-time, it's also likely that your system will have peaked at
// fewer than 200 billion objects by then -- assuming there are fewer than 10
// billion users on earth -- i.e. the level will not be using its "theoretical
// maximum". But the implementation has fewer complications if we just define it
// in terms of "objects changed per N ledgers", and let the levels
// asymptotically approach "the whole database" at whatever rate it happens.
//
// In the worst case, the levels become "the whole database" relatively early
// (say, at level 4) in the lifetime of the system, and levels 5, 6, even 7 are
// effectively redundant / fully-shadowed copies of the same set of objects, as
// the system ages and the ledger number continues to climb. This is the "not
// too many users, but long-lived system" scenario: 40 years of activity on
// (say) 1m accounts. The amount of "redundant" hashing/storage in such a
// degenerate case is _much cheaper_ than the amount of work those levels would
// be if they were "fully populated" (i.e. they're only as big as level 4,
// rather than their theoretical maximums, each 16x bigger than the last) so it
// should be relatively harmless if this happens.

class Application;

class Hasher
{
    // FIXME : use a real CHF, this is a stand-in while doing design.
    uint256 mState;

  public:
    void update(uint8_t const* data, size_t len);
    template <typename arr>
    void
    update(arr const& v)
    {
        update(v.data(), v.size());
    }
    uint256 finish();
};

/**
 * Bucket is an immutable container for a sorted set of k/v pairs (object ID /
 * hash) which is designed to be held in a shared_ptr<> which is referenced
 * between threads, to minimize copying. It is therefore imperative that it be
 * _really_ immutable, not just faking it.
 *
 * Two buckets can be merged together efficiently (in a single pass): elements
 * from the newer bucket overwrite elements from the older bucket, the rest are
 * merged in sorted order, and all elements are hashed while being added.
 *
 * This is the basic version of a Bucket; a more sophisticated one (which we'll
 * add soon) will store itself on disk if it's any bigger than a specified
 * limit. For now we assume we can fit all of them in memory.
 */
class Bucket
{

  public:
    // Each k/v entry is 20+32=52 bytes long.
    //
    // Suppose you reserve (say) 2**32=4GB RAM storing Buckets, then we can keep
    // about 82 million object/hash pairs in memory, before hitting disk.
    using KVPair = std::tuple<uint256, uint256>;

  private:
    std::vector<KVPair> const mEntries;
    uint256 const mHash;

  public:
    Bucket();
    Bucket(std::vector<KVPair>&& entries, uint256&& hash);
    std::vector<KVPair> const& getEntries() const;
    uint256 const& getHash() const;

    static std::shared_ptr<Bucket> fresh(std::vector<KVPair>&& entries);

    static std::shared_ptr<Bucket>
    merge(std::shared_ptr<Bucket> const& oldBucket,
          std::shared_ptr<Bucket> const& newBucket);
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
    Bucket const& getCurr() const;
    Bucket const& getSnap() const;
    void commit();
    void prepare(Application& app, uint64_t currLedger,
                 std::shared_ptr<Bucket> snap);
    std::shared_ptr<Bucket> snap();
};

class BucketList : public CLFGateway
{
    std::vector<BucketLevel> mLevels;

  public:
    static uint64_t levelSize(size_t level);
    static uint64_t levelHalf(size_t level);
    static bool levelShouldSpill(uint64_t ledger, size_t level);
    static uint64_t mask(uint64_t v, uint64_t m);
    static size_t numLevels(uint64_t ledger);

    // These two are from CLFGateway, don't exactly map on to concepts in
    // BucketList, but we implement them for now to keep it compiling.
    virtual LedgerHeaderPtr
    getCurrentHeader()
    {
        return nullptr;
    }
    virtual void recvDelta(CLFDeltaPtr delta){};

    // BucketList _just_ stores a set of key/hash pairs; anything else the CLF
    // wants to support should happen in another class. These operations form a
    // minimal, testable interface to BucketList.
    size_t numLevels() const;
    BucketLevel const& getLevel(size_t i) const;
    uint256 getHash() const;
    void addBatch(Application& app, uint64_t currLedger,
                  std::vector<Bucket::KVPair>&& batch);
};
}
#endif
