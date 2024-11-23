#pragma once

// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
#include "xdr/Stellar-types.h"
#include <iosfwd>
#include <vector>

namespace stellar
{
// Key type for cache of merges-in-progress. These only exist to enable
// re-attaching a deserialized FutureBucket to a std::shared_future, or (if the
// merge is finished and has been promoted to a live bucket) to identify which
// _output_ was produced from a given set of _inputs_ so we can recreate a
// pre-resolved std::shared_future containing that output.
struct MergeKey
{
    MergeKey(bool keepTombstoneEntries, Hash const& currHash,
             Hash const& snapHash, std::vector<Hash> const& shadowHashes);

    bool mKeepTombstoneEntries;
    Hash mInputCurrBucket;
    Hash mInputSnapBucket;
    std::vector<Hash> mInputShadowBuckets;
    bool operator==(MergeKey const& other) const;
};

std::ostream& operator<<(std::ostream& out, MergeKey const& b);

std::string format_as(MergeKey const& k);
}

namespace std
{
template <> struct hash<stellar::MergeKey>
{
    size_t operator()(stellar::MergeKey const& k) const noexcept;
};
}
