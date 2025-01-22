#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketUtils.h"
#include "util/GlobalChecks.h"
#include "util/RandomEvictionCache.h"
#include "util/XDROperators.h" // IWYU pragma: keep
#include <map>
#include <vector>

#include "util/XDRCereal.h"
#include <cereal/archives/binary.hpp>
#include <cereal/types/utility.hpp>
#include <cereal/types/vector.hpp>

namespace stellar
{
// maps smallest and largest LedgerKey on a given page inclusively
// [lowerBound, upperbound]
struct RangeEntry
{
    LedgerKey lowerBound;
    LedgerKey upperBound;

    RangeEntry() = default;
    RangeEntry(LedgerKey low, LedgerKey high)
        : lowerBound(low), upperBound(high)
    {
        releaseAssert(low < high || low == high);
    }

    inline bool
    operator==(RangeEntry const& in) const
    {
        return lowerBound == in.lowerBound && upperBound == in.upperBound;
    }

    template <class Archive>
    void
    serialize(Archive& ar)
    {
        ar(lowerBound, upperBound);
    }
};

using RangeIndex = std::vector<std::pair<RangeEntry, std::streamoff>>;
using BucketEvictionCache =
    RandomEvictionCache<LedgerKey, std::shared_ptr<BucketEntry const>>;

using AssetPoolIDMap = std::map<Asset, std::vector<PoolID>>;

// For small Buckets, we can cache all contents in memory. Because we cache all
// entries, the index is just as large as the Bucket itself, so we never persist
// this index type. It is always recreated on startup.
struct InMemoryIndex
{
    std::map<LedgerKey, std::shared_ptr<BucketEntry>> inMemoryMap;
    BucketEntryCounters counters{};
};
}