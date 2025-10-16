// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/P23HotArchiveBug.h"

#include <array>
#include <string>

#include "bucket/BucketUtils.h"
#include "bucket/SearchableBucketList.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnImpl.h"
#include "ledger/LedgerTypeUtils.h"
#include "main/Application.h"
#include <xdrpp/printer.h>

namespace stellar
{
namespace p23_hot_archive_bug
{
using namespace internal;

std::vector<LedgerKey>
getP23CorruptedHotArchiveKeys()
{
    std::vector<LedgerKey> result;
    for (size_t i = 0; i < P23_CORRUPTED_HOT_ARCHIVE_ENTRIES_COUNT; ++i)
    {
        LedgerEntry corruptedEntry;
        fromOpaqueBase64(corruptedEntry, P23_CORRUPTED_HOT_ARCHIVE_ENTRIES[i]);
        result.push_back(LedgerEntryKey(corruptedEntry));
    }
    return result;
}

void
addHotArchiveBatchWithP23HotArchiveFix(
    AbstractLedgerTxn& ltx, Application& app, LedgerHeader header,
    std::vector<LedgerEntry> const& archivedEntries,
    std::vector<LedgerKey> const& restoredEntries)
{
    LedgerKeySet currentBatchKeys;
    for (auto const& le : archivedEntries)
    {
        currentBatchKeys.insert(LedgerEntryKey(le));
    }

    auto updatedArchivedEntries = archivedEntries;
    updatedArchivedEntries.reserve(updatedArchivedEntries.size() +
                                   P23_CORRUPTED_HOT_ARCHIVE_ENTRIES_COUNT);
    auto const& hotArchiveSnapshot =
        app.getAppConnector().copySearchableHotArchiveBucketListSnapshot();
    for (size_t i = 0; i < P23_CORRUPTED_HOT_ARCHIVE_ENTRIES_COUNT; ++i)
    {
        LedgerEntry corruptedEntry;
        fromOpaqueBase64(corruptedEntry, P23_CORRUPTED_HOT_ARCHIVE_ENTRIES[i]);
        LedgerKey corruptedEntryKey = LedgerEntryKey(corruptedEntry);

        LedgerEntry fixedEntry;
        fromOpaqueBase64(fixedEntry,
                         P23_CORRUPTED_HOT_ARCHIVE_ENTRY_CORRECT_STATE[i]);
        LedgerKey fixedEntryKey = LedgerEntryKey(fixedEntry);
        // That would be a bug in the hardcoded data, so this is safe as long
        // as build is not broken.
        releaseAssert(corruptedEntryKey == fixedEntryKey);

        // Perform several checks to ensure that we only fix the entries in
        // Hot Archive that match our expectations for the corrupted entries.
        // All these checks are highly likely to pass in practice, but since
        // restoration of the corrupted entries is not prohibited at the
        // protocol level, we have to account for all the possibilities just in
        // case.

        // Ensure that the entry exists in Hot Archive.
        auto hotArchiveEntry = hotArchiveSnapshot->load(corruptedEntryKey);
        if (!hotArchiveEntry)
        {
            CLOG_WARNING(
                Ledger,
                "Skipping fix of the entry with key '{}' as it does not exist "
                "in the Hot Archive",
                xdr::xdr_to_string(corruptedEntryKey));
            continue;
        }

        // Ensure that the entry state matches our expectations for the
        // corrupted entry.
        if (hotArchiveEntry->archivedEntry() != corruptedEntry)
        {
            CLOG_WARNING(
                Ledger,
                "Skipping fix of the entry with key '{}' as it does not match "
                "the expected corrupted value: '{}' vs '{}'",
                xdr::xdr_to_string(corruptedEntryKey),
                xdr::xdr_to_string(corruptedEntry),
                xdr::xdr_to_string(hotArchiveEntry->archivedEntry()));
            continue;
        }

        // Ensure that the entry does not exist in the live state.
        if (ltx.loadWithoutRecord(corruptedEntryKey))
        {
            CLOG_WARNING(Ledger,
                         "Skipping fix of the entry with key '{}' as it exists "
                         "in the live state",
                         xdr::xdr_to_string(corruptedEntryKey));
            continue;
        }
        // Make sure we're not trying to overwrite any entries that are being
        // archived in this batch, as these aren't in the live state anymore,
        // but also don't have the right state in the hot archive.
        if (currentBatchKeys.find(corruptedEntryKey) != currentBatchKeys.end())
        {
            CLOG_WARNING(
                Ledger,
                "Skipping fix of the entry with key '{}' as it's present in "
                "the hot archive batch",
                xdr::xdr_to_string(corruptedEntryKey));
            continue;
        }
        CLOG_INFO(Ledger, "Applied fix to the Hot Archive entry {}: '{}'->'{}'",
                  i, xdr::xdr_to_string(corruptedEntry),
                  xdr::xdr_to_string(fixedEntry));
        updatedArchivedEntries.emplace_back(std::move(fixedEntry));
    }
    app.getBucketManager().addHotArchiveBatch(
        app, header, updatedArchivedEntries, restoredEntries);
}
} // namespace p23_hot_archive_bug
} // namespace stellar
