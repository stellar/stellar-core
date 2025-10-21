#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <vector>

#include "util/UnorderedMap.h"
#include "util/UnorderedSet.h"

#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-ledger.h"

namespace stellar
{
class Application;
class AbstractLedgerTxn;
class Config;
struct EvictedStateVectors;

namespace p23_hot_archive_bug
{
// 'Hide' these constants away as we don't really want to access them directly
// outside the tests.
namespace internal
{
constexpr size_t P23_CORRUPTED_HOT_ARCHIVE_ENTRIES_COUNT = 478;
extern const std::array<std::string, P23_CORRUPTED_HOT_ARCHIVE_ENTRIES_COUNT>
    P23_CORRUPTED_HOT_ARCHIVE_ENTRIES;
extern const std::array<std::string, P23_CORRUPTED_HOT_ARCHIVE_ENTRIES_COUNT>
    P23_CORRUPTED_HOT_ARCHIVE_ENTRY_CORRECT_STATE;
} // namespace internal

// Verifier for protocol 23 Hot Archive corruption data.
// This verifies that the externally provided corruption data file matches the
// actual corruptions that have occurred during protocol 23, and that the
// fixes to entries that have never been restored match the expected correct
// state.
// This should be used in combination with catchup that spans the whole of
// protocol 23, as that is when the corruptions and restorations occur.
//
// More specifically, this verifies the following:
// - That every data corruption that occurs during archival exists in the file
// in the expected state, and that there is no corruption not accounted for.
// - That every restoration of a corrupted entry is provided in the
// file with the correct restoration ledger and the expected corrupted state.
// - That every expected corruption from the file has happened before p24
// upgrade
// - That p24 upgrade puts the correct fixes to every entry that was never
// restored, according to the file
// - That `P23_CORRUPTED_HOT_ARCHIVE_ENTRIES` and
// `P23_CORRUPTED_HOT_ARCHIVE_ENTRY_CORRECT_STATE` match the provided file as
// an additional sanity check.
struct Protocol23CorruptionDataVerifier
{
  public:
    // Load corruption data from CSV file at given path
    // The CSV format is: ledgerKey,correctLedgerEntry,corruptedLedgerEntry,
    // evictedLedgerSeq,restoredLedgerSeq where:
    // - ledgerKey: base64 XDR encoded LedgerKey
    // - correctLedgerEntry: base64 XDR encoded LedgerEntry
    // - corruptedLedgerEntry: base64 XDR encoded LedgerEntry
    // - evictedLedgerSeq: ledger sequence number where entry was evicted
    // - restoredLedgerSeq: ledger sequence number where entry was restored
    //   (or "not-restored" if null)
    // Returns true on success, false on error
    bool loadFromFile(std::string const& path);

    // Verifies that the restoration of a corrupted entry matches the expected
    // data.
    // This should be called for every restoration that occurs during catchup,
    // non-corrupted restorations are ignored.
    // This is thread-safe.
    void verifyRestorationOfCorruptedEntry(LedgerKey const& restoredKey,
                                           LedgerEntry const& restoredEntry,
                                           uint32_t ledgerSeq,
                                           uint32_t protocolVersion);
    // Verifies that every corruption during archival is accounted for in the
    // expected data.
    // This should be called for every eviction that occurs during catchup,
    // non-corrupted evictions are ignored.
    // This is thread-safe.
    void verifyArchivalOfCorruptedEntry(EvictedStateVectors const& evictedState,
                                        Application& app, uint32_t ledgerSeq,
                                        uint32_t protocolVersion);
    // Verifies that the batch of Hot Archive fixes on protocol 24 upgrade
    // corresponds to the expected data (i.e. only entries that were never
    // restored have been fixed, and that the fix comes back to the correct
    // state).
    // Also verifies that all the data entries have been exhausted by
    // verification functions above, which requires the catchup to run for
    // the whole duration of protocol 23.
    // This is not thread-safe, which is fine as it shouldn't ever happen during
    // the transaction application.
    void verifyEntryFixesOnP24Upgrade(
        std::vector<LedgerEntry> const& entryBatch) const;

  private:
    void verifyHardcodedData() const;

    // Parse a single line from the CSV file
    // Returns true on success, false on error
    bool parseLine(std::string const& line, size_t lineNumber);

    // Map from evicted ledger sequence to set of ledger keys evicted at that
    // sequence
    UnorderedMap<uint32_t, UnorderedSet<LedgerKey>> mEvictedSeqToKeys;

    // Map from ledger key to pair of correct and corrupted ledger entries
    // pair.first = correct entry, pair.second = corrupted entry
    UnorderedMap<LedgerKey, std::pair<LedgerEntry, LedgerEntry>> mKeyToEntries;

    // Map from ledger key to restored ledger sequence (only for entries that
    // were restored)
    UnorderedMap<LedgerKey, uint32_t> mKeyToRestoredSeq;

    // Set of ledger keys that were not restored
    UnorderedSet<LedgerKey> mNotRestoredKeys;

    // Keys of the corrupted entries that have been restored during catchup.
    // This is populated in `verifyRestorationOfCorruptedEntry` calls.
    UnorderedSet<LedgerKey> mKeysRestoredDuringCatchup;
    // Keys of the entries that have been evicted during catchup in corrupted
    // state. This is populated in `verifyArchivalOfCorruptedEntry` calls.
    UnorderedSet<LedgerKey> mKeysEvictedDuringCatchup;

    std::mutex mMutex;
};

std::vector<LedgerKey> getP23CorruptedHotArchiveKeys();

void addHotArchiveBatchWithP23HotArchiveFix(
    AbstractLedgerTxn& ltx, Application& app, LedgerHeader header,
    std::vector<LedgerEntry> const& archivedEntries,
    std::vector<LedgerKey> const& restoredEntries);

} // namespace p23_hot_archive_bug
} // namespace stellar
