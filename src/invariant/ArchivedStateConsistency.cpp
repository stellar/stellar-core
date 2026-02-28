// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/ArchivedStateConsistency.h"
#include "bucket/BucketListSnapshot.h"
#include "bucket/LedgerCmp.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerStateSnapshot.h"
#include "ledger/LedgerTypeUtils.h"
#include "main/Application.h"
#include "transactions/TransactionUtils.h"
#include "util/GlobalChecks.h"
#include "util/LogSlowExecution.h"
#include "util/XDRCereal.h"
#include "util/types.h"
#include <fmt/format.h>
#include <vector>

namespace stellar
{
ArchivedStateConsistency::ArchivedStateConsistency() : Invariant(false)
{
}

std::shared_ptr<Invariant>
ArchivedStateConsistency::registerInvariant(Application& app)
{
    return app.getInvariantManager()
        .registerInvariant<ArchivedStateConsistency>();
}

std::string
ArchivedStateConsistency::getName() const
{
    return "ArchivedStateConsistency";
}

std::string
ArchivedStateConsistency::checkOnLedgerCommit(
    ApplyLedgerStateSnapshot const& lclSnapshot,
    std::vector<LedgerEntry> const& persitentEvictedFromLive,
    std::vector<LedgerKey> const& tempAndTTLEvictedFromLive,
    UnorderedMap<LedgerKey, LedgerEntry> const& restoredFromArchive,
    UnorderedMap<LedgerKey, LedgerEntry> const& restoredFromLiveState)
{
    LogSlowExecution logSlow("ArchivedStateConsistency::checkOnLedgerCommit",
                             LogSlowExecution::Mode::AUTOMATIC_RAII, "took",
                             std::chrono::milliseconds(1));

    auto ledgerSeq = lclSnapshot.getLedgerSeq() + 1;
    auto ledgerVers = lclSnapshot.getLedgerHeader().ledgerVersion;

    if (protocolVersionIsBefore(
            ledgerVers,
            LiveBucket::FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION))
    {
        return std::string{};
    }

    // Collect all keys to preload
    LedgerKeySet allKeys;

    // Keys for evicted from live entries
    for (auto const& e : persitentEvictedFromLive)
    {
        auto key = LedgerEntryKey(e);
        releaseAssertOrThrow(isPersistentEntry(key));
        allKeys.insert(key);
    }

    // Keys for deleted from live (temp and TTLs)
    for (auto const& k : tempAndTTLEvictedFromLive)
    {
        releaseAssertOrThrow(!isPersistentEntry(k));
        allKeys.insert(k);
    }

    // Keys for restored entries
    for (auto const& [key, entry] : restoredFromArchive)
    {
        if (key.type() != TTL)
        {
            releaseAssertOrThrow(isPersistentEntry(key));
        }
        allKeys.insert(key);
    }
    for (auto const& [key, entry] : restoredFromLiveState)
    {
        allKeys.insert(key);
    }

    // Preload from both live and archived state
    UnorderedMap<LedgerKey, LedgerEntry> preloadedLiveEntries;
    auto preloadedLiveVector =
        lclSnapshot.loadLiveKeys(allKeys, "ArchivedStateConsistency");
    for (auto const& entry : preloadedLiveVector)
    {
        preloadedLiveEntries[LedgerEntryKey(entry)] = entry;
    }

    auto preloadedArchivedVector = lclSnapshot.loadArchiveKeys(allKeys);
    UnorderedMap<LedgerKey, HotArchiveBucketEntry> preloadedArchivedEntries;
    for (auto const& entry : preloadedArchivedVector)
    {
        if (entry.type() == HotArchiveBucketEntryType::HOT_ARCHIVE_ARCHIVED)
        {
            preloadedArchivedEntries[LedgerEntryKey(entry.archivedEntry())] =
                entry;
        }
    }

    UnorderedSet<LedgerKey> deletedKeys;
    for (auto const& k : tempAndTTLEvictedFromLive)
    {
        deletedKeys.insert(k);
    }

    auto evictionRes = checkEvictionInvariants(
        preloadedLiveEntries, preloadedArchivedEntries, deletedKeys,
        persitentEvictedFromLive, ledgerSeq, ledgerVers);

    auto restoreRes = checkRestoreInvariants(
        preloadedLiveEntries, preloadedArchivedEntries, restoredFromArchive,
        restoredFromLiveState, ledgerSeq, ledgerVers);

    if (evictionRes.empty() && restoreRes.empty())
    {
        return std::string{};
    }
    else
    {
        return evictionRes + "\n" + restoreRes;
    }
}

std::string
ArchivedStateConsistency::checkEvictionInvariants(
    UnorderedMap<LedgerKey, LedgerEntry> const& preloadedLiveEntries,
    UnorderedMap<LedgerKey, HotArchiveBucketEntry> const&
        preloadedArchivedEntries,
    UnorderedSet<LedgerKey> const& deletedKeys,
    std::vector<LedgerEntry> const& archivedEntries, uint32_t ledgerSeq,
    uint32_t ledgerVers)
{
    ZoneScoped;

    if (deletedKeys.empty() && archivedEntries.empty())
    {
        return std::string{};
    }

    for (auto const& archivedEntry : archivedEntries)
    {
        releaseAssertOrThrow(isPersistentEntry(archivedEntry.data));
        auto lk = LedgerEntryKey(archivedEntry);

        // Archived entry does not already exist in archive
        if (auto preexistingEntry = preloadedArchivedEntries.find(lk);
            preexistingEntry != preloadedArchivedEntries.end())
        {
            return fmt::format(
                FMT_STRING("ArchivedStateConsistency invariant failed: "
                           "Archived entry already present in archive: {}"),
                xdrToCerealString(preexistingEntry->second.archivedEntry(),
                                  "entry"));
        }

        // Archived entry exists in live state
        auto entryIter = preloadedLiveEntries.find(lk);
        if (entryIter == preloadedLiveEntries.end())
        {
            return fmt::format(
                FMT_STRING("ArchivedStateConsistency invariant failed: "
                           "Evicted entry does not exist in live state: {}"),
                xdrToCerealString(lk, "entry_key"));
        }

        // TTL for archived entry exists in live state and is appropriately
        // deleted
        auto ttlKey = getTTLKey(archivedEntry);
        auto ttlIter = preloadedLiveEntries.find(ttlKey);
        if (ttlIter == preloadedLiveEntries.end())
        {
            return fmt::format(
                FMT_STRING("ArchivedStateConsistency invariant failed: "
                           "TTL for persistent entry does not exist. "
                           "Entry key: {}, TTL key: {}"),
                xdrToCerealString(lk, "entry_key"),
                xdrToCerealString(ttlKey, "ttl_key"));
        }

        // Check that entry is actually expired
        else if (isLive(ttlIter->second, ledgerSeq))
        {
            return fmt::format(
                FMT_STRING("ArchivedStateConsistency invariant failed: "
                           "Evicted TTL is still live. "
                           "Entry key: {}, TTL entry: {}"),
                xdrToCerealString(lk, "entry_key"),
                xdrToCerealString(ttlIter->second, "ttl_entry"));
        }

        // Check that we're evicting the most up to date version. Only check
        // starting at protocol 24, since p23 had a bug where outdated entries
        // were evicted.
        if (protocolVersionStartsFrom(ledgerVers, ProtocolVersion::V_24) &&
            archivedEntry != entryIter->second)
        {
            std::string errorMsg = fmt::format(
                FMT_STRING("ArchivedStateConsistency invariant failed: "
                           "Outdated entry evicted. Key: {}"),
                xdrToCerealString(lk, "entry_key"));
            errorMsg += fmt::format(
                FMT_STRING("\nEvicted entry: {}\nCorrect value: {}"),
                xdrToCerealString(archivedEntry, "evicted"),
                xdrToCerealString(entryIter->second, "correct"));
            return errorMsg;
        }
    }

    // Count the number of TTLs and temp entries evicted so we can see if we
    // have an "orphaned" TTL value without an associated data entry
    size_t ttls = 0;
    size_t temps = 0;
    for (auto const& lk : deletedKeys)
    {
        // We'll just count the TTL keys we come across, the validity check
        // will happen via the data entry.
        if (isTemporaryEntry(lk))
        {
            ++temps;

            auto entryIter = preloadedLiveEntries.find(lk);
            if (entryIter == preloadedLiveEntries.end())
            {
                return fmt::format(
                    FMT_STRING(
                        "ArchivedStateConsistency invariant failed: "
                        "Evicted temp key does not exist in live state: {}"),
                    xdrToCerealString(lk, "key"));
            }

            auto ttlLk = getTTLKey(lk);
            auto ttlIter = preloadedLiveEntries.find(ttlLk);
            if (ttlIter == preloadedLiveEntries.end())
            {
                return fmt::format(
                    FMT_STRING("ArchivedStateConsistency invariant failed: "
                               "TTL for temp entry does not exist in live "
                               "state. Entry key: {}, "
                               "TTL key: {}"),
                    xdrToCerealString(lk, "entry_key"),
                    xdrToCerealString(ttlLk, "ttl_key"));
            }
            else if (isLive(ttlIter->second, ledgerSeq))
            {
                return fmt::format(
                    FMT_STRING("ArchivedStateConsistency invariant failed: "
                               "Evicted TTL for temp entry is still live. "
                               "Entry key: {}, "
                               "TTL entry: {}"),
                    xdrToCerealString(lk, "entry_key"),
                    xdrToCerealString(ttlIter->second, "ttl_entry"));
            }
        }
        else
        {
            ++ttls;
        }
    }

    if (temps + archivedEntries.size() != ttls)
    {
        return fmt::format(
            FMT_STRING(
                "ArchivedStateConsistency invariant failed: "
                "Number of TTLs evicted does not match number of "
                "data/code entries evicted. "
                "Evicted {} TTLs, {} temp entries, {} archived entries."),
            ttls, temps, archivedEntries.size());
    }

    return std::string{};
}

std::string
ArchivedStateConsistency::checkRestoreInvariants(
    UnorderedMap<LedgerKey, LedgerEntry> const& preloadedLiveEntries,
    UnorderedMap<LedgerKey, HotArchiveBucketEntry> const&
        preloadedArchivedEntries,
    UnorderedMap<LedgerKey, LedgerEntry> const& restoredFromArchive,
    UnorderedMap<LedgerKey, LedgerEntry> const& restoredFromLiveState,
    uint32_t ledgerSeq, uint32_t ledgerVer)
{
    ZoneScoped;

    for (auto const& [key, entry] : restoredFromLiveState)
    {
        // TTL keys are populated upstream during the restore process (they are
        // not actually in the archive)
        if (key.type() == TTL)
        {
            continue;
        }

        if (!isPersistentEntry(key))
        {
            return fmt::format(
                FMT_STRING("ArchivedStateConsistency invariant failed: "
                           "Restored entry from live state is not a persistent "
                           "entry: {}"),
                xdrToCerealString(key, "key"));
        }
        else if (restoredFromLiveState.find(getTTLKey(key)) ==
                 restoredFromLiveState.end())
        {
            return fmt::format(
                FMT_STRING(
                    "ArchivedStateConsistency invariant failed: "
                    "TTL for restored entry from live state is missing: {}"),
                xdrToCerealString(getTTLKey(key), "ttl_key"));
        }
    }

    for (auto const& [key, entry] : restoredFromArchive)
    {
        // TTL keys are populated upstream during the restore process (they are
        // not actually in the archive)
        if (key.type() == TTL)
        {
            continue;
        }

        if (!isPersistentEntry(key))
        {
            return fmt::format(
                FMT_STRING("ArchivedStateConsistency invariant failed: "
                           "Restored entry from archive is not a persistent "
                           "entry: {}"),
                xdrToCerealString(key, "key"));
        }
        else if (restoredFromArchive.find(getTTLKey(key)) ==
                 restoredFromArchive.end())
        {
            return fmt::format(
                FMT_STRING(
                    "ArchivedStateConsistency invariant failed: "
                    "TTL for restored entry from archive is missing: {}"),
                xdrToCerealString(getTTLKey(key), "ttl_key"));
        }
    }

    // For hot archive restores, just check that the entry is not in live
    // state and exists in the hot archive with the correct value.
    for (auto const& [key, entry] : restoredFromArchive)
    {
        if (preloadedLiveEntries.find(key) != preloadedLiveEntries.end())
        {
            return fmt::format(
                FMT_STRING(
                    "ArchivedStateConsistency invariant failed: "
                    "Restored entry from archive is still in live state: {}"),
                xdrToCerealString(key, "key"));
        }

        if (key.type() == TTL)
        {
            continue;
        }

        auto hotArchiveEntry = preloadedArchivedEntries.find(key);
        if (hotArchiveEntry == preloadedArchivedEntries.end())
        {
            return fmt::format(
                FMT_STRING("ArchivedStateConsistency invariant failed: "
                           "Restored entry from archive does not exist in hot "
                           "archive: {}"),
                xdrToCerealString(key, "key"));
        }
        // Skip this check prior to protocol 24, since there was a bug in 23
        // Don't check lastModifiedLedgerSeq, since it may have been updated by
        // the ltx
        else if (protocolVersionStartsFrom(ledgerVer, ProtocolVersion::V_24))
        {
            bool isValid =
                hotArchiveEntry->second.archivedEntry().data == entry.data &&
                hotArchiveEntry->second.archivedEntry().ext == entry.ext;
            if (!isValid)
            {
                return fmt::format(
                    FMT_STRING("ArchivedStateConsistency invariant failed: "
                               "Restored entry from archive has incorrect "
                               "value: Entry to "
                               "Restore: {}, Hot Archive Entry: {}"),
                    xdrToCerealString(entry, "entry_to_restore"),
                    xdrToCerealString(hotArchiveEntry->second.archivedEntry(),
                                      "hot_archive_entry"));
            }
        }
    }

    // For live state restores, check that the entry we're restoring is the
    // correct value on the live BucketList, is actually expired, and is not
    // in the hot archive.
    for (auto const& [key, entry] : restoredFromLiveState)
    {
        if (auto hotArchiveEntry = preloadedArchivedEntries.find(key);
            hotArchiveEntry != preloadedArchivedEntries.end())
        {
            return fmt::format(
                FMT_STRING("ArchivedStateConsistency invariant failed: "
                           "Restored entry from live BucketList exists in hot "
                           "archive: Live Entry: {}, Hot Archive Entry: {}"),
                xdrToCerealString(entry, "live_entry"),
                xdrToCerealString(hotArchiveEntry->second.archivedEntry(),
                                  "hot_archive_entry"));
        }

        auto liveEntry = preloadedLiveEntries.find(key);
        if (liveEntry == preloadedLiveEntries.end())
        {
            return fmt::format(
                FMT_STRING("ArchivedStateConsistency invariant failed: "
                           "Restored entry from live BucketList does not exist "
                           "in live state: {}"),
                xdrToCerealString(key, "key"));
        }
        // For non-TTL entries, `entry` (the restored value) should be
        // identical to `liveEntry->second` (the value on the live BucketList)
        // since data/code entries are not modified during a restore. TTL
        // entries are excluded from this check because restoration updates the
        // TTL's liveUntilLedgerSeq, so the restored value will differ from the
        // on-disk value.
        else if (key.type() != TTL && liveEntry->second != entry)
        {
            return fmt::format(
                FMT_STRING("ArchivedStateConsistency invariant failed: "
                           "Restored entry from live BucketList has incorrect "
                           "value: Live Entry: {}, Entry to Restore: {}"),
                xdrToCerealString(liveEntry->second.data, "live_entry"),
                xdrToCerealString(entry, "entry_to_restore"));
        }

        if (key.type() == TTL)
        {
            // `entry` is the TTL after restoration (with updated
            // liveUntilLedgerSeq). `liveEntry->second` is the original
            // on-disk TTL before restoration. We check that the restored
            // TTL is now live and the original was expired.
            if (!isLive(entry, ledgerSeq))
            {
                return fmt::format(
                    FMT_STRING("ArchivedStateConsistency invariant failed: "
                               "Restored entry's updated TTL is still "
                               "expired: TTL Entry: {}"),
                    xdrToCerealString(entry, "ttl_entry"));
            }
            if (isLive(liveEntry->second, ledgerSeq))
            {
                return fmt::format(
                    FMT_STRING("ArchivedStateConsistency invariant failed: "
                               "Restored entry from live BucketList is not "
                               "expired: TTL Entry: {}"),
                    xdrToCerealString(liveEntry->second, "ttl_entry"));
            }
        }
    }

    return std::string{};
}
};
