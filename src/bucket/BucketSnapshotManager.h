#pragma once

// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/NonCopyable.h"
#include "util/UnorderedMap.h"
#include "util/types.h"

#include <memory>
#include <mutex>

namespace medida
{
class Meter;
class MetricsRegistry;
class Timer;
}

namespace stellar
{

class Application;
class BucketList;
class SearchableBucketListSnapshot;

// This class serves as the boundary between non-threadsafe singleton classes
// (BucketManager, BucketList, Metrics, etc) and threadsafe, parallel BucketList
// snapshots.
class BucketSnapshotManager : NonMovableOrCopyable
{
  private:
    medida::MetricsRegistry& mMetrics;
    BucketList const& mBucketList;
    std::recursive_mutex& mBucketListMutex;

    mutable UnorderedMap<LedgerEntryType, medida::Timer&> mPointTimers{};
    mutable UnorderedMap<std::string, medida::Timer&> mBulkTimers{};

    medida::Meter& mBulkLoadMeter;
    medida::Meter& mBloomMisses;
    medida::Meter& mBloomLookups;

  public:
    BucketSnapshotManager(medida::MetricsRegistry& metrics,
                          BucketList const& bl,
                          std::recursive_mutex& bucketListMutex);

    std::unique_ptr<SearchableBucketListSnapshot>
    getSearchableBucketListSnapshot() const;

    // Checks if snapshot is behind the current ledgerSeq and updates the
    // snapshot as necessary
    void maybeUpdateSnapshot(SearchableBucketListSnapshot& snapshot) const;

    medida::Timer& recordBulkLoadMetrics(std::string const& label,
                                         size_t numEntries) const;

    medida::Timer& getPointLoadTimer(LedgerEntryType t) const;
};
}