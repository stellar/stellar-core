#pragma once

// copyright 2015 stellar development foundation and contributors. licensed
// under the apache license, version 2.0. see the copying file at the root
// of this distribution or at http://www.apache.org/licenses/license-2.0

#include "bucket/BucketUtils.h"
#include "util/NonCopyable.h"
#include "util/ProtocolVersion.h"
#include "xdr/Stellar-types.h"
#include <filesystem>
#include <optional>
#include <string>

namespace asio
{
class io_context;
}

namespace stellar
{

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
 * container and must be extened for each specific BucketList type. In
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

template <class BucketT, class IndexT>
class BucketBase : public NonMovableOrCopyable
{
    BUCKET_TYPE_ASSERT(BucketT);

    // Because of the CRTP design with derived Bucket classes, this base class
    // does not have direct access to BucketT::IndexT, so we take two templates
    // and make this assert.
    static_assert(
        std::is_same_v<
            IndexT,
            std::conditional_t<
                std::is_same_v<BucketT, LiveBucket>, LiveBucketIndex,
                std::conditional_t<std::is_same_v<BucketT, HotArchiveBucket>,
                                   HotArchiveBucketIndex, void>>>,
        "IndexT must match BucketT::IndexT");

  protected:
    std::filesystem::path const mFilename;
    Hash const mHash;
    size_t mSize{0};

    std::unique_ptr<IndexT const> mIndex{};

    // Returns index, throws if index not yet initialized
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
               std::unique_ptr<IndexT const>&& index);

    Hash const& getHash() const;
    std::filesystem::path const& getFilename() const;
    size_t getSize() const;

    bool isEmpty() const;

    // Delete index and close file stream
    void freeIndex();

    // Returns true if bucket is indexed, false otherwise
    bool isIndexed() const;

    // Sets index, throws if index is already set
    void setIndex(std::unique_ptr<IndexT const>&& index);

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

    static std::string randomBucketName(std::string const& tmpDir);
    static std::string randomBucketIndexName(std::string const& tmpDir);

#ifdef BUILD_TESTS
    IndexT const&
    getIndexForTesting() const
    {
        return getIndex();
    }

#endif // BUILD_TESTS

    template <class T> friend class BucketSnapshotBase;
};
}
