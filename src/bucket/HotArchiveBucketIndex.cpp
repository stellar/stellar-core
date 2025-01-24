// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/HotArchiveBucketIndex.h"
#include "bucket/BucketManager.h"
#include "util/GlobalChecks.h"
#include "xdr/Stellar-ledger-entries.h"
#include <cereal/archives/binary.hpp>
#include <optional>

namespace stellar
{

HotArchiveBucketIndex::HotArchiveBucketIndex(
    BucketManager& bm, std::filesystem::path const& filename,
    std::streamoff pageSize, Hash const& hash, asio::io_context& ctx)
    : mDiskIndex(bm, filename, pageSize, hash, ctx)
{
    ZoneScoped;
    releaseAssert(!filename.empty());

    // HotArchive only supports disk indexes
    releaseAssertOrThrow(pageSize != 0);
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

std::pair<std::optional<std::streamoff>, RangeIndex::const_iterator>
HotArchiveBucketIndex::scan(RangeIndex::const_iterator start,
                            LedgerKey const& k) const
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