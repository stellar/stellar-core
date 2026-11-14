#pragma once

// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/LiveBucket.h"
#include "crypto/LtHash.h"
#include "ledger/LedgerEntryRefs.h"
#include "util/BatchExecutor.h"
#include "util/NonCopyable.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-ledger.h"
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace medida
{
class Timer;
}

namespace stellar
{

class Application;
class BucketManager;
struct HistoryArchiveState;

// Level 0 of the live bucket list, written as shards
// (EXPERIMENTAL_SHARDED_TOP_BUCKET).
//
// What a level-0 bucket IS does not change: the merge of the batches of the
// one or two ledgers it holds, sorted by key, behind one metadata entry. What
// changes is its hash and how it gets to disk.
//
// * The hash. Not the SHA-256 of the bucket's file but the LtHash16 of its
//   records (the XDR of every bucket entry of the file, the metadata entry
//   among them), condensed with SHA-256 where a 32-byte hash is expected
//   (`bucketHash`). It does not depend on the order of the records or on how
//   they are split into parts, so the parts can be hashed apart and added
//   up. Levels 1 and deeper keep SHA-256.
//
// * Shards. A shard is a set of keys that is the same in every ledger: the
//   shard of an entry is a function of its key (`shardOf`). There are three
//   groups: contract data and code, TTL entries, everything else (classic
//   entries); a group has N shards, by a hash of the key. How many shards a
//   node uses changes no hash.
//
// * A ledger's batch. The entries are cut into the shards, and every shard,
//   on its own thread, sorts its entries, writes them to a file of its own
//   and syncs it (that is what makes the ledger durable), merges them with
//   what the shard holds of the previous ledger (with the merge code of every
//   bucket merge) and hashes the result. The shards' hashes are added up, and
//   the new level-0 bucket exists: its hash, and its index for lookups.
//
// * The bucket's file. It is written afterwards by a thread of its own, the
//   shards' entries interleaved by key: byte for byte the file that level 0
//   has without the setting. It is what spills into level 1 and what is
//   published to history archives. Whoever needs it waits for it
//   (`BucketBase::getFilename`).
//
// The bucket list is touched by its owning thread only, as without the
// setting; this class is not thread safe, and `addBatch` is called by that
// thread.
class ShardedTopBucket : public NonMovableOrCopyable
{
  public:
    // One shard's part of the bucket.
    struct Shard
    {
        // Sorted by key.
        std::vector<BucketEntry> entries;
        // The entries' XDR, one after the other, as the bucket file holds
        // them (without the 4 bytes in front of each).
        std::string records;
        // Where entry i's XDR begins in `records`; one more at the end.
        std::vector<uint32_t> offsets{0};
        // Of the records.
        LtHash16 hash;

        void append(BucketEntry&& entry);
    };

    ShardedTopBucket(Application& app, uint32_t shardsPerGroup);
    ~ShardedTopBucket();

    uint32_t
    shardsPerGroup() const
    {
        return mShardsPerGroup;
    }

    // Adds the entries of ledger `ledgerSeq` to `curr`, level 0's current
    // bucket (empty after a spill), and returns the new current bucket. Its
    // file may not be on disk yet.
    std::shared_ptr<LiveBucket>
    addBatch(Application& app, uint32_t ledgerSeq, uint32_t protocolVersion,
             std::shared_ptr<LiveBucket> const& curr,
             LedgerEntryRefs initEntries, LedgerEntryRefs liveEntries,
             LedgerKeyRefs deadEntries, bool countMergeEvents, bool doFsync);

    // Blocks until the file of the bucket `addBatch` returned last is on
    // disk.
    void waitForFile();

    // The shard of a key with `shardsPerGroup` shards per group: the group's
    // number (0 contract data and code, 1 TTL, 2 the rest) times
    // `shardsPerGroup`, plus a hash of the key modulo `shardsPerGroup`.
    static size_t shardOf(LedgerKey const& key, uint32_t shardsPerGroup);

    // The hash of a level-0 bucket whose records have the LtHash `h`: the
    // zero hash for no records (the empty bucket), the digest otherwise.
    static Hash bucketHash(LtHash16 const& h);

    // The LtHash of the records of a bucket file. Throws unless the file is
    // the canonical file of its records (the metadata entry first, entries
    // in the strict order of their keys, canonical XDR, nothing else).
    static LtHash16 hashOfFile(std::string const& filename);

    // At startup, before the bucket files of the last closed ledger are
    // checked: makes sure that level 0's current bucket of `has` has its
    // file. If the node stopped after the ledger's commit and before the
    // file was written, the file is rebuilt from the ledger's shard files
    // and the bucket they were added to. Removes all shard files left.
    static void prepareForRestart(Application& app,
                                  HistoryArchiveState const& has,
                                  uint32_t protocolVersion);

    // The metadata entry of a live bucket of that protocol version, and
    // whether such a bucket has one.
    static BucketEntry metaEntry(uint32_t protocolVersion);
    static bool hasMetaEntry(uint32_t protocolVersion);

#ifdef BUILD_TESTS
    std::vector<std::shared_ptr<Shard const>> const&
    getShardsForTesting() const
    {
        return mShards;
    }
    // While set, in every application of the process: a bucket's file is not
    // written and the ledger's shard files stay, as on a node that stops
    // before the file's writer is done.
    static void loseFileForTesting(bool lose);
#endif

  private:
    uint32_t const mShardsPerGroup;
    size_t const mNumShards;

    // The bucket the shards describe.
    Hash mCurrHash{};
    // Never changed once made: a ledger's tasks read the previous ledger's
    // shards while the file's writer may still be reading them too.
    std::vector<std::shared_ptr<Shard const>> mShards;

    // The writer of that bucket's file, and what tells that it is done.
    std::future<std::chrono::nanoseconds> mFileWriter;
    std::shared_future<void> mFileReady;

    // Where a ledger's shard files go, and what tells this object's files
    // from another's in the same directory.
    std::string mShardDir;
    std::string mInstance;
    BatchExecutor mExecutor;

    medida::Timer& mCutTimer;
    medida::Timer& mShardsTimer;
    medida::Timer& mFinishTimer;
    medida::Timer& mFileTimer;
    medida::Timer& mFileWaitTimer;

    static void removeShardFiles(std::string const& shardDir);

    // Takes the shards from `curr`, a bucket this object did not make (after
    // a restart, or a bucket list assumed from a history archive state).
    void rebuildFrom(std::shared_ptr<LiveBucket> const& curr);
};
}
