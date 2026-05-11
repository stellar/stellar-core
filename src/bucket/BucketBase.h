// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "bucket/BucketInputIterator.h"
#include "bucket/BucketUtils.h"
#include "util/NonCopyable.h"
#include "util/ProtocolVersion.h"
#include "xdr/Stellar-types.h"
#include <filesystem>
#include <string>

namespace asio
{
class io_context;
}

namespace stellar
{

template <IsBucketType BucketT> class SearchableBucketListSnapshot;

/**
 * Bucket is an immutable container for a sorted set of "Entries" (object ID,
 * hash, xdr-message tuples) which is designed to be held in a shared_ptr<>
 * which is referenced between threads, to minimize copying. It is therefore
 * imperative that it be _really_ immutable, not just faking it.
 *
 * Two buckets can be merged together efficiently (in a single pass): elements
 * from the newer bucket overwrite elements from the older bucket, the rest are
 * merged in sorted order, and all elements are hashed while being added.
 *
 * Different types of BucketList vary on the type of entries they contain and by
 * extension the merge logic of those entries. Additionally, some types of
 * BucketList may have special operations only relevant to that specific type.
 * This pure virtual base class provides the core functionality of a BucketList
 * container and must be extended for each specific BucketList type. In
 * particular, the fresh and merge functions must be defined for the specific
 * type, while other functionality can be shared.
 */

class BucketManager;

enum class Loop
{
    COMPLETE,
    INCOMPLETE
};

class HotArchiveBucket;
class LiveBucket;
class LiveBucketIndex;
class HotArchiveBucketIndex;

template <IsBucketType BucketT> class BucketBase : public NonMovableOrCopyable
{
  public:
    // Because of the CRTP design with derived Bucket classes, this base class
    // does not have direct access to BucketT::IndexT, so we hardcode the
    // index types
    using IndexT = std::conditional_t<std::is_same_v<BucketT, LiveBucket>,
                                      LiveBucketIndex, HotArchiveBucketIndex>;

  protected:
    std::filesystem::path const mFilename;
    Hash const mHash;
    size_t mSize{0};

    std::shared_ptr<IndexT const> mIndex{};

    // Unconditionally resets mIndex. Must only be called via the static
    // freeIndex(shared_ptr) which asserts the use_count invariant.
    void freeIndex();

    // Returns index, throws if index not yet initialized. Note: Does not use
    // atomic load (see comments on index functions below).
    IndexT const& getIndex() const;

    static std::string randomFileName(std::string const& tmpDir,
                                      std::string ext);

  public:
    static constexpr ProtocolVersion
        FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION = ProtocolVersion::V_23;

    // Create an empty bucket. The empty bucket has hash '000000...' and its
    // filename is the empty string.
    BucketBase();

    // Construct a bucket with a given filename and hash. Asserts that the file
    // exists, but does not check that the hash is the bucket's hash. Caller
    // needs to ensure that.
    BucketBase(std::string const& filename, Hash const& hash,
               std::shared_ptr<IndexT const>&& index);

    Hash const& getHash() const;
    std::filesystem::path const& getFilename() const;
    size_t getSize() const;

    bool isEmpty() const;

    // The following index functions (freeIndex, isIndexed, setIndex,
    // maybeGetIndexForMerge) should only be used in the bucket creation/garbage
    // collection path when no BucketList readers hold a pointer to this bucket.
    // These accesses must be guarded by std::atomic_load, since bucket
    // creation/GC can occur concurrently (Index worker thread, background
    // merge, main thread GC).
    //
    // Note that getIndex is not protected. This is intentional, since the
    // BucketList should allow lockless concurrent reads. This is safe because:
    //    - If the index is not already set (race with setIndex), we are already
    //      in a critically bad state, getIndex should throw.
    //    - If the index is set (race with freeIndex), we assert that only 1
    //      reference exists, such that there can be no concurrent race.

    // Frees the index on a bucket that has no external references
    // (use_count == 1, i.e. only the BucketManager's map holds it).
    // This guarantees no concurrent reader can race on mIndex.
    static void
    freeIndex(std::shared_ptr<BucketT> const& bucket)
    {
        releaseAssert(bucket);
        releaseAssert(bucket.use_count() == 1);
        bucket->freeIndex();
    }

    // Returns true if bucket is indexed, false otherwise
    bool isIndexed() const;

    // Sets index, throws if index is already set
    void setIndex(std::shared_ptr<IndexT const> index);

    // Returns the bucket's index if it exists, otherwise
    // nullptr. This is used by background merges to grab a shared_ptr to an
    // existing index, preventing a race where GC could free the index between
    // checking the index and adopting the finished merge result.
    static std::shared_ptr<IndexT const>
    maybeGetIndexForMerge(std::shared_ptr<BucketT> const& bucket)
    {
        releaseAssert(bucket);
        return std::atomic_load(&bucket->mIndex);
    }

    // Merge two buckets together, producing a fresh one. Entries in `oldBucket`
    // are overridden in the fresh bucket by keywise-equal entries in
    // `newBucket`. Entries are inhibited from the fresh bucket by keywise-equal
    // entries in any of the buckets in the provided `shadows` vector.
    //
    // Each bucket is self-describing in terms of the ledger protocol version it
    // was constructed under, and the merge algorithm adjusts to the maximum of
    // the versions attached to each input or shadow bucket. The provided
    // `maxProtocolVersion` bounds this (for error checking) and should usually
    // be the protocol of the ledger header at which the merge is starting. An
    // exception will be thrown if any provided bucket versions exceed it.
    static std::shared_ptr<BucketT>
    merge(BucketManager& bucketManager, uint32_t maxProtocolVersion,
          std::shared_ptr<BucketT> const& oldBucket,
          std::shared_ptr<BucketT> const& newBucket,
          std::vector<std::shared_ptr<BucketT>> const& shadows,
          bool keepTombstoneEntries, bool countMergeEvents,
          asio::io_context& ctx, bool doFsync);

    // Helper function that implements the core merge algorithm logic for both
    // iterator based and in-memory merges.
    // PutFunc will be called to write entries that are the result of the merge.
    // Parameter pack is empty for HotArchiveBucket, since they do not support
    // shadows.
    // For Livebucket, parameter pack is
    // std::vector<BucketInputIterator<BucketT>>& shadowIterators,
    //    bool keepShadowedLifecycleEntries
    template <typename InputSource, typename PutFuncT, typename... ShadowParams>
    static void mergeInternal(BucketManager& bucketManager,
                              InputSource& inputSource, PutFuncT putFunc,
                              uint32_t protocolVersion, MergeCounters& mc,
                              ShadowParams&&... shadowParams);

    static std::string randomBucketName(std::string const& tmpDir);
    static std::string randomBucketIndexName(std::string const& tmpDir);

#ifdef BUILD_TESTS
    IndexT const&
    getIndexForTesting() const
    {
        return getIndex();
    }

    void
    freeIndexForTesting()
    {
        freeIndex();
    }

#endif // BUILD_TESTS

    template <IsBucketType T> friend class SearchableBucketListSnapshot;
};
}
