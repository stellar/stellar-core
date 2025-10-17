// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/ArchivedStateConsistency.h"
#include "bucket/BucketManager.h"
#include "bucket/BucketSnapshot.h"
#include "bucket/BucketSnapshotManager.h"
#include "bucket/LedgerCmp.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTypeUtils.h"
#include "main/Application.h"
#include "transactions/TransactionUtils.h"
#include "util/GlobalChecks.h"
#include "util/XDRCereal.h"
#include "util/types.h"
#include <fmt/format.h>

namespace stellar
{
ArchivedStateConsistency::ArchivedStateConsistency() : Invariant(true)
{
}

std::string
ArchivedStateConsistency::start(Application& app)
{
    auto protocolVersion =
        app.getLedgerManager().getLastClosedLedgerHeader().header.ledgerVersion;
    if (protocolVersionIsBefore(
            protocolVersion,
            LiveBucket::FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION))
    {
        CLOG_INFO(Invariant,
                  "Skipping ArchivedStateConsistency invariant for "
                  "protocol version {}",
                  protocolVersion);
        return std::string{};
    }

    CLOG_INFO(Invariant, "Starting ArchivedStateConsistency invariant");
    auto has = app.getLedgerManager().getLastClosedLedgerHAS();

    std::map<LedgerKey, LedgerEntry> archived =
        app.getBucketManager().loadCompleteHotArchiveState(has);
    std::map<LedgerKey, LedgerEntry> live =
        app.getBucketManager().loadCompleteLedgerState(has);
    auto archivedIt = archived.begin();
    auto liveIt = live.begin();

    while (archivedIt != archived.end() && liveIt != live.end())
    {
        if (archivedIt->first < liveIt->first)
        {
            archivedIt++;
            continue;
        }
        else if (liveIt->first < archivedIt->first)
        {
            liveIt++;
            continue;
        }
        else
        {
            return fmt::format(
                FMT_STRING(
                    "ArchivedStateConsistency:: Entry with the same key is "
                    "present in both live and archived state. Key: {}"),
                xdrToCerealString(archivedIt->first, "entry_key"));
        }
    }

    CLOG_INFO(Invariant, "ArchivedStateConsistency invariant passed");
    return std::string{};
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
    SearchableSnapshotConstPtr lclLiveState,
    SearchableHotArchiveSnapshotConstPtr lclHotArchiveState,
    std::vector<LedgerEntry> const& evictedFromLive,
    std::vector<LedgerKey> const& deletedKeysFromLive,
    UnorderedMap<LedgerKey, LedgerEntry> const& restoredFromArchive,
    UnorderedMap<LedgerKey, LedgerEntry> const& restoredFromLiveState)
{
    if (protocolVersionIsBefore(
            lclLiveState->getLedgerHeader().ledgerVersion,
            LiveBucket::FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION))
    {
        CLOG_INFO(Invariant,
                  "Skipping ArchivedStateConsistency invariant for "
                  "protocol version {}",
                  lclLiveState->getLedgerHeader().ledgerVersion);
        return std::string{};
    }
    auto ledgerSeq = lclLiveState->getLedgerSeq() + 1;
    auto ledgerVers = lclLiveState->getLedgerHeader().ledgerVersion;
    auto evictionRes = checkEvictionInvariants(
        lclLiveState, lclHotArchiveState, deletedKeysFromLive, evictedFromLive,
        ledgerSeq, ledgerVers);

    auto restoreRes = checkRestoreInvariants(
        lclLiveState, lclHotArchiveState, restoredFromArchive,
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
    SearchableSnapshotConstPtr liveState,
    SearchableHotArchiveSnapshotConstPtr archivedState,
    std::vector<LedgerKey> const& deletedTempAndTTLKeys,
    std::vector<LedgerEntry> const& archivedEntries, uint32_t ledgerSeq,
    uint32_t ledgerVers)
{
    ZoneScoped;

    if (deletedTempAndTTLKeys.empty() && archivedEntries.empty())
    {
        return std::string{};
    }

    // Snapshots should be based on the lcl ledger
    releaseAssertOrThrow(liveState->getLedgerSeq() == ledgerSeq - 1);
    releaseAssertOrThrow(archivedState->getLedgerSeq() == ledgerSeq - 1);

    // Load the newest version of all evicted entries and their TTLs from
    // the BucketList.
    LedgerKeySet archivedKeys;
    for (auto const& archivedEntry : archivedEntries)
    {
        releaseAssertOrThrow(isPersistentEntry(archivedEntry.data));
        archivedKeys.insert(LedgerEntryKey(archivedEntry));
    }

    LedgerKeySet tempAndTTLKeysToLoad;
    for (auto const& deletedKey : deletedTempAndTTLKeys)
    {
        releaseAssertOrThrow(isTemporaryEntry(deletedKey) ||
                             deletedKey.type() == TTL);
        tempAndTTLKeysToLoad.insert(deletedKey);
    }

    auto persistentLoadResult = populateLoadedEntries(
        archivedKeys,
        liveState->loadKeys(archivedKeys, "ArchivedStateConsistency"));

    auto tempAndTTLLoadResult = populateLoadedEntries(
        tempAndTTLKeysToLoad,
        liveState->loadKeys(tempAndTTLKeysToLoad, "ArchivedStateConsistency"));

    // Persistent entry invariants
    for (auto const& archivedEntry : archivedEntries)
    {
        releaseAssertOrThrow(isPersistentEntry(archivedEntry.data));
        auto lk = LedgerEntryKey(archivedEntry);
        auto entryIter = persistentLoadResult.find(lk);
        releaseAssertOrThrow(entryIter != persistentLoadResult.end());

        // Check that the archived entry is not already present in the archive
        if (auto preexistingEntry = archivedState->load(lk);
            preexistingEntry != nullptr)
        {
            return fmt::format(
                FMT_STRING("ArchivedStateConsistency invariant failed: "
                           "Archived entry already present in archive: {}"),
                xdrToCerealString(preexistingEntry->archivedEntry(), "entry"));
        }

        // Check that entry exists
        if (entryIter->second == nullptr)
        {
            return fmt::format(
                FMT_STRING("ArchivedStateConsistency invariant failed: "
                           "Evicted entry does not exist in live state: {}"),
                xdrToCerealString(lk, "entry_key"));
        }

        // Check that TTL was also evicted and exists
        auto ttlKey = getTTLKey(archivedEntry);
        auto ttlIter = tempAndTTLLoadResult.find(ttlKey);
        if (ttlIter == tempAndTTLLoadResult.end())
        {
            return fmt::format(
                FMT_STRING("ArchivedStateConsistency invariant failed: "
                           "TTL for persistent entry not evicted. "
                           "Entry key: {}, TTL key: {}"),
                xdrToCerealString(lk, "entry_key"),
                xdrToCerealString(ttlKey, "ttl_key"));
        }
        else if (ttlIter->second == nullptr)
        {
            return fmt::format(
                FMT_STRING("ArchivedStateConsistency invariant failed: "
                           "TTL does not exist. Entry Key: {}, TTL key: {}"),
                xdrToCerealString(lk, "entry_key"),
                xdrToCerealString(ttlKey, "ttl_key"));
        }

        // Check that entry is actually expired
        else if (isLive(*ttlIter->second, ledgerSeq))
        {
            return fmt::format(
                FMT_STRING("ArchivedStateConsistency invariant failed: "
                           "Evicted TTL is still live. "
                           "Entry key: {}, TTL entry: {}"),
                xdrToCerealString(lk, "entry_key"),
                xdrToCerealString(*ttlIter->second, "ttl_entry"));
        }

        // Check that we're evicting the most up to date version. Only check
        // starting at protocol 24, since this bug exists in p23.
        if (protocolVersionStartsFrom(ledgerVers, ProtocolVersion::V_24) &&
            archivedEntry != *entryIter->second)
        {
            std::string errorMsg = fmt::format(
                FMT_STRING("ArchivedStateConsistency invariant failed: "
                           "Outdated entry evicted. Key: {}"),
                xdrToCerealString(lk, "entry_key"));
            errorMsg += fmt::format(
                FMT_STRING("\nEvicted entry: {}\nCorrect value: {}"),
                xdrToCerealString(archivedEntry, "evicted"),
                xdrToCerealString(*entryIter->second, "correct"));
            return errorMsg;
        }
    }

    // Count the number of TTLs and temp entries evicted so we can see if we
    // have an "orphaned" TTL value without an associated data entry
    size_t numTTLsEvicted = 0;
    size_t numTempEntriesEvicted = 0;
    for (auto const& deletedKey : deletedTempAndTTLKeys)
    {
        // We'll just count the TTL keys we come across, the validity check
        // will happen via the data entry.
        if (isTemporaryEntry(deletedKey))
        {
            ++numTempEntriesEvicted;

            auto lk = deletedKey;
            auto entryIter = tempAndTTLLoadResult.find(lk);
            releaseAssertOrThrow(entryIter != tempAndTTLLoadResult.end());
            if (entryIter->second == nullptr)
            {
                return fmt::format(
                    FMT_STRING(
                        "ArchivedStateConsistency invariant failed: "
                        "Evicted temp key does not exist in live state: {}"),
                    xdrToCerealString(lk, "key"));
            }

            auto ttlLk = getTTLKey(deletedKey);
            auto ttlIter = tempAndTTLLoadResult.find(ttlLk);
            if (ttlIter == tempAndTTLLoadResult.end())
            {
                return fmt::format(
                    FMT_STRING("ArchivedStateConsistency invariant failed: "
                               "TTL for temp entry not evicted. Entry key: {}, "
                               "TTL key: {}"),
                    xdrToCerealString(lk, "entry_key"),
                    xdrToCerealString(ttlLk, "ttl_key"));
            }

            else if (ttlIter->second == nullptr)
            {
                return fmt::format(
                    FMT_STRING("ArchivedStateConsistency invariant failed: "
                               "TTL for temp entry does not exist in live "
                               "state. Entry key: {}, TTL key: {}"),
                    xdrToCerealString(lk, "entry_key"),
                    xdrToCerealString(ttlLk, "ttl_key"));
            }
            else if (isLive(*ttlIter->second, ledgerSeq))
            {
                return fmt::format(
                    FMT_STRING("ArchivedStateConsistency invariant failed: "
                               "Evicted TTL for temp entry is still live. "
                               "Entry key: {}, "
                               "TTL entry: {}"),
                    xdrToCerealString(lk, "entry_key"),
                    xdrToCerealString(*ttlIter->second, "ttl_entry"));
            }
        }
        else
        {
            ++numTTLsEvicted;
        }
    }

    if (numTempEntriesEvicted + archivedEntries.size() != numTTLsEvicted)
    {
        return fmt::format(
            FMT_STRING(
                "ArchivedStateConsistency invariant failed: "
                "Number of TTLs evicted does not match number of "
                "data/code entries evicted. "
                "Evicted {} TTLs, {} temp entries, {} archived entries."),
            numTTLsEvicted, numTempEntriesEvicted, archivedEntries.size());
    }

    return std::string{};
}

std::string
ArchivedStateConsistency::checkRestoreInvariants(
    SearchableSnapshotConstPtr lclLiveState,
    SearchableHotArchiveSnapshotConstPtr lclHotArchiveState,
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
        if (lclLiveState->load(key) != nullptr)
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

        auto hotArchiveEntry = lclHotArchiveState->load(key);
        if (hotArchiveEntry == nullptr)
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
        else if (!(hotArchiveEntry->archivedEntry().data == entry.data) ||
                 !(hotArchiveEntry->archivedEntry().ext == entry.ext) &&
                     protocolVersionStartsFrom(ledgerVer,
                                               ProtocolVersion::V_24))
        {
            return fmt::format(
                FMT_STRING(
                    "ArchivedStateConsistency invariant failed: "
                    "Restored entry from archive has incorrect value: Entry to "
                    "Restore: {}, Hot Archive Entry: {}"),
                xdrToCerealString(entry, "entry_to_restore"),
                xdrToCerealString(hotArchiveEntry->archivedEntry(),
                                  "hot_archive_entry"));
        }
    }

    // For live state restores, check that the entry we're restoring is the
    // correct value on the live BucketList, is actually expired, and is not
    // in the hot archive.
    for (auto const& [key, entry] : restoredFromLiveState)
    {
        if (auto hotArchiveEntry = lclHotArchiveState->load(key);
            hotArchiveEntry != nullptr)
        {
            return fmt::format(
                FMT_STRING("ArchivedStateConsistency invariant failed: "
                           "Restored entry from live BucketList exists in hot "
                           "archive: Live Entry: {}, Hot Archive Entry: {}"),
                xdrToCerealString(entry, "live_entry"),
                xdrToCerealString(hotArchiveEntry->archivedEntry(),
                                  "hot_archive_entry"));
        }

        auto liveEntry = lclLiveState->load(key);
        if (liveEntry == nullptr)
        {
            return fmt::format(
                FMT_STRING("ArchivedStateConsistency invariant failed: "
                           "Restored entry from live BucketList does not exist "
                           "in live state: {}"),
                xdrToCerealString(key, "key"));
        }
        else if (*liveEntry != entry)
        {
            return fmt::format(
                FMT_STRING("ArchivedStateConsistency invariant failed: "
                           "Restored entry from live BucketList has incorrect "
                           "value: Live Entry: {}, Entry to Restore: {}"),
                xdrToCerealString(liveEntry->data, "live_entry"),
                xdrToCerealString(entry, "entry_to_restore"));
        }

        if (key.type() == TTL && isLive(entry, ledgerSeq))
        {
            return fmt::format(
                FMT_STRING("ArchivedStateConsistency invariant failed: "
                           "Restored entry from live BucketList is not "
                           "expired: Entry: {}, TTL Entry: {}"),
                xdrToCerealString(entry, "entry"),
                xdrToCerealString(entry, "ttl_entry"));
        }
    }

    return std::string{};
}
};