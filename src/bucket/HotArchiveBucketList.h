#pragma once

// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketListBase.h"
#include "bucket/HotArchiveBucket.h"

namespace stellar
{
// The HotArchiveBucketList stores recently evicted entries. It contains Buckets
// of type HotArchiveBucket, which store individual entries of type
// HotArchiveBucketEntry.
class HotArchiveBucketList : public BucketListBase<HotArchiveBucket>
{
  public:
    void addBatch(Application& app, uint32_t currLedger,
                  uint32_t currLedgerProtocol,
                  std::vector<LedgerEntry> const& archiveEntries,
                  std::vector<LedgerKey> const& restoredEntries,
                  std::vector<LedgerKey> const& deletedEntries);
};
}