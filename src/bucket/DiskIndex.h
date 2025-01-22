#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketIndexUtils.h"
#include "bucket/BucketUtils.h"
#include "util/BinaryFuseFilter.h"
#include "util/GlobalChecks.h"
#include "util/XDROperators.h" // IWYU pragma: keep

#include <cereal/archives/binary.hpp>
#include <cereal/types/map.hpp>
#include <cereal/types/memory.hpp>
#include <cereal/types/vector.hpp>

#include <filesystem>
#include <memory>

namespace stellar
{
class BucketManager;

// For large Buckets, we cannot cache all contents in memory. Instead, we use a
// random eviction cache for partial cacheing, and a range based index + binary
// fuse filter for disk lookups. Creating this index is expensive, so we persist
// it to disk. We do not persist the random eviction cache.
template <class BucketT> class DiskIndex : public NonMovableOrCopyable
{
    BUCKET_TYPE_ASSERT(BucketT);

    // Fields from here are persisted on disk. Cereal doesn't like templates so
    // we define an inner struct to hold all serializable fields.
    struct Data
    {
        std::streamoff pageSize{};
        RangeIndex keysToOffset;
        std::unique_ptr<BinaryFuseFilter16> filter{};

        // Note: mAssetToPoolID is null for HotArchive Bucket types
        std::unique_ptr<AssetPoolIDMap> assetToPoolID{};
        BucketEntryCounters counters{};

        template <class Archive>
        void
        save(Archive& ar) const
        {
            auto version = BucketT::IndexT::BUCKET_INDEX_VERSION;
            ar(version, pageSize, keysToOffset, filter, assetToPoolID,
               counters);
        }

        // Note: version and pageSize must be loaded before this
        // function is called. Caller is responsible for deserializing these
        // fields first via the preload function. If any of these values does
        // not match the expected values, it indicates an upgrade has occurred
        // since serialization. The on-disk version is outdated and must be
        // replaced by a newly created index.
        template <class Archive>
        void
        load(Archive& ar)
        {
            ar(keysToOffset, filter, assetToPoolID, counters);
        }

    } mData;

    // Saves index to disk, overwriting any preexisting file for this index
    void saveToDisk(BucketManager& bm, Hash const& hash,
                    asio::io_context& ctx) const;

  public:
    // Constructor for creating a fresh index.
    DiskIndex(BucketManager& bm, std::filesystem::path const& filename,
              std::streamoff pageSize, Hash const& hash, asio::io_context& ctx);

    // Constructor for loading pre-existing index from disk. Must call preLoad
    // before calling this constructor to properly deserialize index.
    template <class Archive> DiskIndex(Archive& ar, std::streamoff pageSize);

    // Begins searching for LegerKey k from start.
    // Returns pair of:
    // file offset in the bucket file for k, or std::nullopt if not found
    // iterator that points to the first index entry not less than k, or
    // BucketIndex::end()
    std::pair<std::optional<std::streamoff>, RangeIndex::const_iterator>
    scan(RangeIndex::const_iterator start, LedgerKey const& k) const;

    // Returns [lowFileOffset, highFileOffset) that contain the key ranges
    // [lowerBound, upperBound]. If no file offsets exist, returns [0, 0]
    std::optional<std::pair<std::streamoff, std::streamoff>>
    getOffsetBounds(LedgerKey const& lowerBound,
                    LedgerKey const& upperBound) const;

    // Returns page size for index
    std::streamoff
    getPageSize() const
    {
        return mData.pageSize;
    }

    BucketEntryCounters const&
    getBucketEntryCounters() const
    {
        return mData.counters;
    }

    RangeIndex::const_iterator
    begin() const
    {
        return mData.keysToOffset.begin();
    }

    RangeIndex::const_iterator
    end() const
    {
        return mData.keysToOffset.end();
    }

    // Loads index version and pageSize from serialized index archive. Must be
    // called before DiskIndex(Archive& ar, std::streamoff pageSize) to properly
    // deserialize index.
    template <class Archive>
    static void
    preLoad(Archive& ar, uint32_t& version, std::streamoff& pageSize)
    {
        ar(version, pageSize);
    }

    // This messy template makes is such that this function is only defined
    // when BucketT == LiveBucket
    template <int..., typename T = BucketT,
              std::enable_if_t<std::is_same_v<T, LiveBucket>, bool> = true>
    AssetPoolIDMap const&
    getAssetPoolIDMap() const
    {
        static_assert(std::is_same_v<T, LiveBucket>);
        releaseAssert(mData.assetToPoolID);
        return *mData.assetToPoolID;
    }

#ifdef BUILD_TESTS
    bool operator==(DiskIndex<BucketT> const& inRaw) const;
#endif
};

// Builds index for given bucketfile. This is expensive (> 20 seconds
// for the largest buckets) and should only be called once. If pageSize
// == 0 or if file size is less than the cutoff, individual key index is
// used. Otherwise range index is used, with the range defined by
// pageSize.
template <class BucketT>
std::unique_ptr<typename BucketT::IndexT const>
createIndex(BucketManager& bm, std::filesystem::path const& filename,
            Hash const& hash, asio::io_context& ctx);

// Loads index from given file. If file does not exist or if saved
// index does not have same parameters as current config, return null
template <class BucketT>
std::unique_ptr<typename BucketT::IndexT const>
loadIndex(BucketManager const& bm, std::filesystem::path const& filename,
          size_t bucketFileSize);
}