#pragma once

// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketIndex.h"
#include "medida/meter.h"

class bloom_filter;

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
        std::unique_ptr<bloom_filter> filter{};

        template <class Archive>
        void
        save(Archive& ar) const
        {
            auto version = BUCKET_INDEX_VERSION;
            ar(version, pageSize, keysToOffset, filter);
        }

        // Note: version and pageSize must be loaded before this function is
        // called. pageSize determines template type, so pageSize should be
        // loaded, checked, and then call this function with the appropriate
        // template type
        template <class Archive>
        void
        load(Archive& ar)
        {
            ar(keysToOffset, filter);
        }
    } mData;

    medida::Meter& mBloomMissMeter;
    medida::Meter& mBloomLookupMeter;

    BucketIndexImpl(BucketManager& bm, std::filesystem::path const& filename,
                    std::streamoff pageSize, Hash const& hash);

    template <class Archive>
    BucketIndexImpl(BucketManager const& bm, Archive& ar,
                    std::streamoff pageSize);

    // Saves index to disk, overwriting any preexisting file for this index
    void saveToDisk(BucketManager& bm, Hash const& hash) const;

    friend BucketIndex;

  public:
    virtual std::optional<std::streamoff>
    lookup(LedgerKey const& k) const override;

    virtual std::pair<std::optional<std::streamoff>, Iterator>
    scan(Iterator start, LedgerKey const& k) const override;

    virtual std::optional<std::pair<std::streamoff, std::streamoff>>
    getPoolshareTrustlineRange(AccountID const& accountID) const override;

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

#ifdef BUILD_TESTS
    virtual bool operator==(BucketIndex const& inRaw) const override;
#endif
};
}