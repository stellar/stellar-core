// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/HotArchiveBucketList.h"
#include "bucket/BucketListBase.h"

namespace stellar
{

void
HotArchiveBucketList::addBatch(Application& app, uint32_t currLedger,
                               uint32_t currLedgerProtocol,
                               std::vector<LedgerEntry> const& archiveEntries,
                               std::vector<LedgerKey> const& restoredEntries,
                               std::vector<LedgerKey> const& deletedEntries)
{
    ZoneScoped;
    releaseAssertOrThrow(protocolVersionStartsFrom(
        currLedgerProtocol,
        HotArchiveBucket::FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION));
    addBatchInternal(app, currLedger, currLedgerProtocol, archiveEntries,
                     restoredEntries, deletedEntries);
}
}