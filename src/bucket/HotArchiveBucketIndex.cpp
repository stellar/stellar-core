// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/HotArchiveBucketIndex.h"
#include "bucket/BucketIndexUtils.h"
#include "bucket/BucketManager.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "xdr/Stellar-ledger-entries.h"
#include <cereal/archives/binary.hpp>

namespace stellar
{

HotArchiveBucketIndex::HotArchiveBucketIndex(
    BucketManager& bm, std::filesystem::path const& filename, Hash const& hash,
    asio::io_context& ctx)
    : mDiskIndex(bm, filename, getPageSize(bm.getConfig(), 0), hash, ctx)
{
    ZoneScoped;
    releaseAssert(!filename.empty());

    CLOG_DEBUG(Bucket,
               "HotArchiveBucketIndex::createIndex() indexing key range with "
               "page size {} in bucket {}",
               mDiskIndex.getPageSize(), filename);
}

std::streamoff
HotArchiveBucketIndex::getPageSize(Config const& cfg, size_t bucketSize)
{
    auto ret = getPageSizeFromConfig(cfg);
    if (ret == 0)
    {
        // HotArchive doesn't support individual indexes, use default 16 KB
        // value even if config is set to 0
        return 16'384;
    }
    return ret;
}

template <class Archive>
HotArchiveBucketIndex::HotArchiveBucketIndex(BucketManager const& bm,
                                             Archive& ar,
                                             std::streamoff pageSize)
    : mDiskIndex(ar, bm, pageSize)
{
    // HotArchive only supports disk indexes
    releaseAssertOrThrow(pageSize != 0);
}

std::pair<IndexReturnT, HotArchiveBucketIndex::IterT>
HotArchiveBucketIndex::scan(IterT start, LedgerKey const& k) const
{
    ZoneScoped;
    return mDiskIndex.scan(start, k);
}

#ifdef BUILD_TESTS
bool
HotArchiveBucketIndex::operator==(HotArchiveBucketIndex const& in) const
{
    if (!(mDiskIndex == in.mDiskIndex))
    {
        return false;
    }

    return true;
}
#endif

template HotArchiveBucketIndex::HotArchiveBucketIndex(
    BucketManager const& bm, cereal::BinaryInputArchive& ar,
    std::streamoff pageSize);
}