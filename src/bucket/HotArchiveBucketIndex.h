#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketUtils.h"
#include "bucket/DiskIndex.h"
#include "bucket/HotArchiveBucket.h"
#include "bucket/LedgerCmp.h"
#include "util/NonCopyable.h"
#include "util/XDROperators.h" // IWYU pragma: keep
#include "xdr/Stellar-ledger-entries.h"
#include <filesystem>
#include <optional>

#include <cereal/archives/binary.hpp>

namespace asio
{
class io_context;
}

namespace medida
{
class Meter;
}

namespace stellar
{

/**
 * BucketIndex is an in-memory mapping of LedgerKey's to the file offset
 * of the associated LedgerEntry in a given Bucket file. Because the set of
 * LedgerKeys is too large to keep in memory, BucketIndex can either index
 * individual keys or key ranges.
 *
 * For small buckets, an individual index is used for faster index lookup. For
 * larger buckets, the range index is used. The range index cannot give an exact
 * position for a given LedgerEntry or tell if it exists in the bucket, but can
 * give an offset range for where the entry would be if it exists. Config flags
 * determine the size of the range index, as well as what bucket size should use
 * the individual index vs range index.
 */

class BucketManager;

class HotArchiveBucketIndex : public NonMovableOrCopyable
{
  private:
    DiskIndex<HotArchiveBucket> const mDiskIndex;
    medida::Meter& mBloomMissMeter;
    medida::Meter& mBloomLookupMeter;

    // TODO: Pass as function pointers
    // void markBloomMiss() const;
    // void markBloomLookup() const;

  public:
    inline static const uint32_t BUCKET_INDEX_VERSION = 0;

    HotArchiveBucketIndex(BucketManager& bm,
                          std::filesystem::path const& filename,
                          std::streamoff pageSize, Hash const& hash,
                          asio::io_context& ctx);

    template <class Archive>
    HotArchiveBucketIndex(BucketManager const& bm, Archive& ar,
                          std::streamoff pageSize);

    std::optional<std::streamoff>
    lookup(LedgerKey const& k) const
    {
        return mDiskIndex.scan(mDiskIndex.begin(), k).first;
    }

    std::pair<std::optional<std::streamoff>, RangeIndex::const_iterator>
    scan(RangeIndex::const_iterator start, LedgerKey const& k) const;

    BucketEntryCounters const&
    getBucketEntryCounters() const
    {
        return mDiskIndex.getBucketEntryCounters();
    }

    uint32_t
    getPageSize() const
    {
        return mDiskIndex.getPageSize();
    }

    RangeIndex::const_iterator
    begin() const
    {
        return mDiskIndex.begin();
    }

    RangeIndex::const_iterator
    end() const
    {
        return mDiskIndex.end();
    }
#ifdef BUILD_TESTS
    bool operator==(HotArchiveBucketIndex const& in) const;
#endif
};
}