// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketIndexUtils.h"
#include "bucket/BucketManager.h"
#include "bucket/DiskIndex.h"
#include "bucket/HotArchiveBucket.h"
#include "bucket/HotArchiveBucketIndex.h"
#include "bucket/LiveBucket.h"
#include "bucket/LiveBucketIndex.h"
#include "main/Config.h"
#include "util/Fs.h"
#include <fmt/format.h>

namespace stellar
{

std::streamoff
getPageSizeFromConfig(Config const& cfg)
{
    if (cfg.BUCKETLIST_DB_INDEX_PAGE_SIZE_EXPONENT == 0)
    {
        return 0;
    }

    return 1UL << cfg.BUCKETLIST_DB_INDEX_PAGE_SIZE_EXPONENT;
}

template <class BucketT>
std::unique_ptr<typename BucketT::IndexT const>
createIndex(BucketManager& bm, std::filesystem::path const& filename,
            Hash const& hash, asio::io_context& ctx)
{
    BUCKET_TYPE_ASSERT(BucketT);

    ZoneScoped;
    releaseAssertOrThrow(!filename.empty());

    try
    {
        return std::unique_ptr<typename BucketT::IndexT const>(
            new typename BucketT::IndexT(bm, filename, hash, ctx));
    }
    // BucketIndex throws if BucketManager shuts down before index finishes,
    // so return empty index instead of partial index
    catch (std::runtime_error&)
    {
        return {};
    }
}

template <class BucketT>
std::unique_ptr<typename BucketT::IndexT const>
loadIndex(BucketManager const& bm, std::filesystem::path const& filename,
          std::size_t fileSize)
{
    std::ifstream in(filename, std::ios::binary);
    if (!in)
    {
        throw std::runtime_error(fmt::format(
            FMT_STRING("Error opening file {}"), filename.string()));
    }

    std::streamoff pageSize;
    uint32_t version;
    cereal::BinaryInputArchive ar(in);
    DiskIndex<BucketT>::preLoad(ar, version, pageSize);

    // Page size based on current settings. These may have changed since the
    // on-disk index was serialized.
    auto expectedPageSize =
        BucketT::IndexT::getPageSize(bm.getConfig(), fileSize);

    // Make sure on-disk index was built with correct version and config
    // parameters before deserializing whole file
    if (version != BucketT::IndexT::BUCKET_INDEX_VERSION ||
        pageSize != expectedPageSize)
    {
        return {};
    }

    return std::unique_ptr<typename BucketT::IndexT const>(
        new typename BucketT::IndexT(bm, ar, pageSize));
}

template std::unique_ptr<typename LiveBucket::IndexT const>
createIndex<LiveBucket>(BucketManager& bm,
                        std::filesystem::path const& filename, Hash const& hash,
                        asio::io_context& ctx);
template std::unique_ptr<typename HotArchiveBucket::IndexT const>
createIndex<HotArchiveBucket>(BucketManager& bm,
                              std::filesystem::path const& filename,
                              Hash const& hash, asio::io_context& ctx);

template std::unique_ptr<typename LiveBucket::IndexT const>
loadIndex<LiveBucket>(BucketManager const& bm,
                      std::filesystem::path const& filename,
                      std::size_t fileSize);
template std::unique_ptr<typename HotArchiveBucket::IndexT const>
loadIndex<HotArchiveBucket>(BucketManager const& bm,
                            std::filesystem::path const& filename,
                            std::size_t fileSize);
}
