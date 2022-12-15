#pragma once

// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/LedgerCmp.h"
#include "util/GlobalChecks.h"
#include "util/NonCopyable.h"
#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <variant>

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

// BucketIndex abstract interface
class BucketIndex : public NonMovableOrCopyable
{
  public:
    // maps smallest and largest LedgerKey on a given page inclusively
    // [lowerBound, upperbound]
    struct RangeEntry
    {
        LedgerKey lowerBound;
        LedgerKey upperBound;

        RangeEntry(LedgerKey low, LedgerKey high)
            : lowerBound(low), upperBound(high)
        {
            releaseAssert(low < high || low == high);
        }
    };

    using IndividualEntry = LedgerKey;
    using RangeIndex = std::vector<std::pair<RangeEntry, std::streamoff>>;
    using IndividualIndex =
        std::vector<std::pair<IndividualEntry, std::streamoff>>;
    using Iterator = std::variant<RangeIndex::const_iterator,
                                  IndividualIndex::const_iterator>;

    inline static const std::string DBBackendState = "bl";

    // Returns true if LedgerEntryType not supported by BucketListDB
    static bool typeNotSupported(LedgerEntryType t);

    // Builds index for given bucketfile. This is expensive (> 20 seconds for
    // the largest buckets) and should only be called once. If pageSize == 0 or
    // if file size is less than the cutoff, individual key index is used.
    // Otherwise range index is used, with the range defined by pageSize.
    static std::unique_ptr<BucketIndex const>
    createIndex(BucketManager const& bm, std::filesystem::path const& filename);

    virtual ~BucketIndex() = default;

    // Returns offset in the bucket file for the given key, or std::nullopt if
    // the key is not found
    virtual std::optional<std::streamoff> lookup(LedgerKey const& k) const = 0;

    // Begins searching for LegerKey k from start.
    // Returns pair of:
    // file offset in the bucket file for k, or std::nullopt if not found
    // iterator that points to the first index entry not less than k, or
    // BucketIndex::end()
    virtual std::pair<std::optional<std::streamoff>, Iterator>
    scan(Iterator start, LedgerKey const& k) const = 0;

    // Returns lower bound and upper bound for poolshare trustline entry
    // positions associated with the given accountID. If no trustlines found,
    // returns std::pair<0, 0>
    virtual std::pair<std::streamoff, std::streamoff>
    getPoolshareTrustlineRange(AccountID const& accountID) const = 0;

    // Returns page size for index. InidividualIndex returns 0 for page size
    virtual std::streamoff getPageSize() const = 0;

    virtual Iterator begin() const = 0;

    virtual Iterator end() const = 0;

    virtual void markBloomMiss() const = 0;
    virtual void markBloomLookup() const = 0;
};
}