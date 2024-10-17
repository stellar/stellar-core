#pragma once

// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/Bucket.h"
#include "bucket/BucketIndex.h"
#include "medida/meter.h"
#include "util/BinaryFuseFilter.h"
#include "xdr/Stellar-types.h"

#include <cereal/types/map.hpp>
#include <map>
#include <memory>

namespace stellar
{
// Index maps either individual keys or a key range of BucketEntry's to the
// associated offset within the bucket file. Index stored as vector of pairs:
// First: LedgerKey/Key ranges sorted in the same scheme as LedgerEntryCmp
// Second: offset into the bucket file for a given key/ key range.
// pageSize determines how large, in bytes, each range should be. pageSize == 0
// indicates individual keys used instead of ranges.
template <class IndexT> class BucketIndexImpl : public BucketIndex
{
    // Cereal doesn't like templated classes that derive from pure-virtual
    // interfaces, so we serialize this inner struct that is not polymorphic
    // instead of the actual BucketIndexImpl class
    struct SerializableBucketIndex
    {
        IndexT keysToOffset{};
        std::streamoff pageSize{};
        std::unique_ptr<BinaryFuseFilter16> filter{};
        std::map<Asset, std::vector<PoolID>> assetToPoolID{};
        BucketEntryCounters counters{};

        template <class Archive>
        void
        save(Archive& ar) const
        {
            auto version = BUCKET_INDEX_VERSION;
            ar(version, pageSize, assetToPoolID, keysToOffset, filter,
               counters);
        }

        // Note: version and pageSize must be loaded before this function is
        // called. pageSize determines template type, so pageSize should be
        // loaded, checked, and then call this function with the appropriate
        // template type
        template <class Archive>
        void
        load(Archive& ar)
        {
            ar(assetToPoolID, keysToOffset, filter, counters);
        }
    } mData;

    medida::Meter& mBloomMissMeter;
    medida::Meter& mBloomLookupMeter;

    // Templated constructors are valid C++, but since this is a templated class
    // already, there's no way for the compiler to deduce the type without a
    // templated parameter, hence the tag
    template <class BucketEntryT>
    BucketIndexImpl(BucketManager& bm, std::filesystem::path const& filename,
                    std::streamoff pageSize, Hash const& hash,
                    BucketEntryT const& typeTag);

    template <class Archive>
    BucketIndexImpl(BucketManager const& bm, Archive& ar,
                    std::streamoff pageSize);

    // Saves index to disk, overwriting any preexisting file for this index
    void saveToDisk(BucketManager& bm, Hash const& hash) const;

    // Returns [lowFileOffset, highFileOffset) that contain the key ranges
    // [lowerBound, upperBound]. If no file offsets exist, returns [0, 0]
    std::optional<std::pair<std::streamoff, std::streamoff>>
    getOffsetBounds(LedgerKey const& lowerBound,
                    LedgerKey const& upperBound) const;

    friend BucketIndex;

  public:
    virtual std::optional<std::streamoff>
    lookup(LedgerKey const& k) const override;

    virtual std::pair<std::optional<std::streamoff>, Iterator>
    scan(Iterator start, LedgerKey const& k) const override;

    virtual std::optional<std::pair<std::streamoff, std::streamoff>>
    getPoolshareTrustlineRange(AccountID const& accountID) const override;

    virtual std::vector<PoolID> const&
    getPoolIDsByAsset(Asset const& asset) const override;

    virtual std::optional<std::pair<std::streamoff, std::streamoff>>
    getOfferRange() const override;

    virtual std::streamoff
    getPageSize() const override
    {
        return mData.pageSize;
    }

    virtual Iterator
    begin() const override
    {
        return mData.keysToOffset.begin();
    }

    virtual Iterator
    end() const override
    {
        return mData.keysToOffset.end();
    }

    virtual void markBloomMiss() const override;
    virtual void markBloomLookup() const override;
    virtual BucketEntryCounters const& getBucketEntryCounters() const override;

#ifdef BUILD_TESTS
    virtual bool operator==(BucketIndex const& inRaw) const override;
#endif
};
}