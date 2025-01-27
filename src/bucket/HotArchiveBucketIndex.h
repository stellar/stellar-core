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

  public:
    inline static const uint32_t BUCKET_INDEX_VERSION = 0;

    using IterT = DiskIndex<HotArchiveBucket>::IterT;

    HotArchiveBucketIndex(BucketManager& bm,
                          std::filesystem::path const& filename,
                          Hash const& hash, asio::io_context& ctx);

    template <class Archive>
    HotArchiveBucketIndex(BucketManager const& bm, Archive& ar,
                          std::streamoff pageSize);

    // Returns pagesize for given index based on config parameters. BucketSize
    // is ignored, but we keep the parameter for consistency with
    // LiveBucketIndex.
    static std::streamoff getPageSize(Config const& cfg, size_t bucketSize);

    IndexReturnT
    lookup(LedgerKey const& k) const
    {
        return mDiskIndex.scan(mDiskIndex.begin(), k).first;
    }

    std::pair<IndexReturnT, IterT> scan(IterT start, LedgerKey const& k) const;

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

    IterT
    begin() const
    {
        return mDiskIndex.begin();
    }

    IterT
    end() const
    {
        return mDiskIndex.end();
    }

    void
    markBloomMiss() const
    {
        mDiskIndex.markBloomMiss();
    }
#ifdef BUILD_TESTS
    bool operator==(HotArchiveBucketIndex const& in) const;
#endif
};
}