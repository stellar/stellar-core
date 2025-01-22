// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/LiveBucketIndex.h"
#include "bucket/BucketManager.h"
#include "bucket/DiskIndex.h"
#include "util/GlobalChecks.h"
#include "xdr/Stellar-ledger-entries.h"
#include <ios>
#include <vector>

namespace stellar
{

bool
LiveBucketIndex::typeNotSupported(LedgerEntryType t)
{
    return t == OFFER;
}

LiveBucketIndex::LiveBucketIndex(BucketManager& bm,
                                 std::filesystem::path const& filename,
                                 std::streamoff pageSize, Hash const& hash,
                                 asio::io_context& ctx)
    : mBloomMissMeter(bm.getBloomMissMeter())
    , mBloomLookupMeter(bm.getBloomLookupMeter())
{
    ZoneScoped;
    releaseAssert(!filename.empty());

    if (pageSize == 0)
    {
        // TODO: Individual index
        releaseAssert(false);
    }
    else
    {
        mDiskIndex = std::make_unique<DiskIndex<LiveBucket>>(
            bm, filename, pageSize, hash, ctx);
    }
}

template <class Archive>
LiveBucketIndex::LiveBucketIndex(BucketManager const& bm, Archive& ar,
                                 std::streamoff pageSize)

    : mDiskIndex(std::make_unique<DiskIndex<LiveBucket>>(ar, pageSize))
    , mBloomMissMeter(bm.getBloomMissMeter())
    , mBloomLookupMeter(bm.getBloomLookupMeter())
{
    // Only disk indexes are serialized
    releaseAssertOrThrow(pageSize != 0);
}

RangeIndex::const_iterator
LiveBucketIndex::begin() const
{
    releaseAssertOrThrow(mDiskIndex);
    return mDiskIndex->begin();
}

RangeIndex::const_iterator
LiveBucketIndex::end() const
{
    releaseAssertOrThrow(mDiskIndex);
    return mDiskIndex->end();
}

std::optional<std::streamoff>
LiveBucketIndex::lookup(LedgerKey const& k) const
{
    releaseAssertOrThrow(mDiskIndex);
    return mDiskIndex->scan(mDiskIndex->begin(), k).first;
}

std::pair<std::optional<std::streamoff>, RangeIndex::const_iterator>
LiveBucketIndex::scan(RangeIndex::const_iterator start,
                      LedgerKey const& k) const
{
    releaseAssertOrThrow(mDiskIndex);
    return mDiskIndex->scan(start, k);
}

std::vector<PoolID> const&
LiveBucketIndex::getPoolIDsByAsset(Asset const& asset) const
{
    static const std::vector<PoolID> emptyVec = {};

    // TODO: Individual index
    releaseAssertOrThrow(mDiskIndex);

    auto iter = mDiskIndex->getAssetPoolIDMap().find(asset);
    if (iter == mDiskIndex->getAssetPoolIDMap().end())
    {
        return emptyVec;
    }

    return iter->second;
}

std::optional<std::pair<std::streamoff, std::streamoff>>
LiveBucketIndex::getOfferRange() const
{
    // TODO: Individual index
    releaseAssert(mDiskIndex);

    // Get the smallest and largest possible offer keys
    LedgerKey upperBound(OFFER);
    upperBound.offer().sellerID.ed25519().fill(
        std::numeric_limits<uint8_t>::max());
    upperBound.offer().offerID = std::numeric_limits<int64_t>::max();

    LedgerKey lowerBound(OFFER);
    lowerBound.offer().sellerID.ed25519().fill(
        std::numeric_limits<uint8_t>::min());
    lowerBound.offer().offerID = std::numeric_limits<int64_t>::min();

    return mDiskIndex->getOffsetBounds(lowerBound, upperBound);
}

uint32_t
LiveBucketIndex::getPageSize() const
{
    releaseAssertOrThrow(mDiskIndex);
    return mDiskIndex->getPageSize();
}

BucketEntryCounters const&
LiveBucketIndex::getBucketEntryCounters() const
{
    releaseAssertOrThrow(mDiskIndex);
    return mDiskIndex->getBucketEntryCounters();
}

#ifdef BUILD_TESTS
bool
LiveBucketIndex::operator==(LiveBucketIndex const& in) const
{
    if (mDiskIndex)
    {
        if (!in.mDiskIndex)
        {
            return false;
        }

        if (!(*mDiskIndex == *in.mDiskIndex))
        {
            return false;
        }
    }
    else
    {
        if (in.mDiskIndex)
        {
            return false;
        }
    }

    return true;
}
#endif

template LiveBucketIndex::LiveBucketIndex(BucketManager const& bm,
                                          cereal::BinaryInputArchive& ar,
                                          std::streamoff pageSize);
}