// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/InMemorySorobanStateMatchesBucketList.h"
#include "bucket/BucketUtils.h"
#include "invariant/InvariantManager.h"
#include "ledger/InMemorySorobanState.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "util/XDRCereal.h"
#include <fmt/format.h>

namespace stellar
{

namespace
{
std::string
checkEntry(LedgerEntry const& bucketEntry,
           std::shared_ptr<LedgerEntry const> const& inMemoryEntry)
{
    if (!inMemoryEntry)
    {
        auto keyStr =
            xdrToCerealString(LedgerEntryKey(bucketEntry), "key", true);
        return fmt::format(FMT_STRING("entry exists in bucket list but missing "
                                      "from in-memory cache for key: {}"),
                           keyStr);
    }

    if (bucketEntry != *inMemoryEntry)
    {
        auto bucketStr = xdrToCerealString(bucketEntry, "bucket", true);
        auto memoryStr = xdrToCerealString(*inMemoryEntry, "memory", true);
        auto keyStr =
            xdrToCerealString(LedgerEntryKey(bucketEntry), "key", true);
        return fmt::format(
            FMT_STRING("bucket list entry differs from in-memory entry for "
                       "key {}; bucket={}, memory={}"),
            keyStr, bucketStr, memoryStr);
    }

    return {};
}
}

std::shared_ptr<Invariant>
InMemorySorobanStateMatchesBucketList::registerInvariant(Application& app)
{
    return app.getInvariantManager()
        .registerInvariant<InMemorySorobanStateMatchesBucketList>();
}

InMemorySorobanStateMatchesBucketList::InMemorySorobanStateMatchesBucketList()
    : Invariant(true)
{
}

std::string
InMemorySorobanStateMatchesBucketList::getName() const
{
    return "InMemorySorobanStateMatchesBucketList";
}

// Check that all entries modified by this ledger commit have been correctly
// updated in the in-memory soroban state. We don't want to load from the
// lcl snapshot here, since this would require many disk reads on ledger
// close, and the snapshot is out of date. If we just closed ledger N,
// we want to check the changes committed at the end of N, while the snapshot
// is based off of N - 1. We have the vectors of entries just committed to the
// BucketList, so we compare against those directly.
std::string
InMemorySorobanStateMatchesBucketList::checkOnLedgerCommit(
    SearchableSnapshotConstPtr lclLiveState,
    SearchableHotArchiveSnapshotConstPtr lclHotArchiveState,
    LedgerCommitState const& commitState,
    InMemorySorobanState const& inMemorySorobanState)
{
    auto checkEntries =
        [&inMemorySorobanState](std::vector<LedgerEntry> const& entries) {
            for (auto const& bucketEntry : entries)
            {
                auto key = LedgerEntryKey(bucketEntry);
                if (InMemorySorobanState::isInMemoryType(key))
                {
                    auto inMemoryEntry = inMemorySorobanState.get(key);
                    auto result = checkEntry(bucketEntry, inMemoryEntry);
                    if (!result.empty())
                    {
                        return result;
                    }
                }
            }
            return std::string{};
        };

    auto result = checkEntries(commitState.bucketListEntries.initEntries);
    if (!result.empty())
    {
        return result;
    }

    result = checkEntries(commitState.bucketListEntries.liveEntries);
    if (!result.empty())
    {
        return result;
    }

    // Check deleted state from dead entries
    for (auto const& key : commitState.bucketListEntries.deadEntries)
    {
        if (InMemorySorobanState::isInMemoryType(key))
        {
            auto inMemoryEntry = inMemorySorobanState.get(key);
            if (inMemoryEntry)
            {
                auto keyStr = xdrToCerealString(key, "key", true);
                return fmt::format(
                    FMT_STRING(
                        "deleted entry still exists in in-memory state: {}"),
                    keyStr);
            }
        }
    }

    return {};
}
}
