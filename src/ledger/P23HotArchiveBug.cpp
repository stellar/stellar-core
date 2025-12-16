// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/P23HotArchiveBug.h"

#include <array>
#include <string>

#include "bucket/BucketListSnapshot.h"
#include "bucket/BucketManager.h"
#include "bucket/BucketUtils.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnImpl.h"
#include "ledger/LedgerTypeUtils.h"
#include "main/AppConnector.h"
#include "main/Application.h"
#include <xdrpp/printer.h>

namespace stellar
{
namespace p23_hot_archive_bug
{
namespace
{
LedgerEntry
decodeLedgerEntry(std::string const& encodedBase64)
{
    LedgerEntry entry;
    fromOpaqueBase64(entry, encodedBase64);
    return entry;
}
} // namespace

using namespace internal;

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
        LedgerEntry corruptedEntry =
            decodeLedgerEntry(P23_CORRUPTED_HOT_ARCHIVE_ENTRIES[i]);
        LedgerKey corruptedEntryKey = LedgerEntryKey(corruptedEntry);

        LedgerEntry fixedEntry =
            decodeLedgerEntry(P23_CORRUPTED_HOT_ARCHIVE_ENTRY_CORRECT_STATE[i]);
        LedgerKey fixedEntryKey = LedgerEntryKey(fixedEntry);
        // That would be a bug in the hardcoded data, so this is safe as long
        // as build is not broken.
        releaseAssert(corruptedEntryKey == fixedEntryKey);

        // Perform several checks to ensure that we only fix the entries in
        // Hot Archive that match our expectations for the corrupted entries.

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
        // corrupted entry. This technically could be violated before p24 vote,
        // but we know that it hasn't been the case.
        releaseAssert(hotArchiveEntry->archivedEntry() == corruptedEntry);

        // Ensure that the entry does not exist in the live state - at this
        // point we've ensured it's in the Hot Archive.
        releaseAssert(!ltx.loadWithoutRecord(corruptedEntryKey));
        // Make sure we're not trying to overwrite any entries that are being
        // archived in this batch, as these aren't in the live state anymore,
        // but also don't have the right state in the hot archive.
        releaseAssert(currentBatchKeys.find(corruptedEntryKey) ==
                      currentBatchKeys.end());
        CLOG_INFO(Ledger, "Applied fix to the Hot Archive entry {}: '{}'->'{}'",
                  i, xdr::xdr_to_string(corruptedEntry),
                  xdr::xdr_to_string(fixedEntry));
        updatedArchivedEntries.emplace_back(std::move(fixedEntry));
    }

    if (app.getProtocol23CorruptionDataVerifier())
    {
        app.getProtocol23CorruptionDataVerifier()->verifyEntryFixesOnP24Upgrade(
            updatedArchivedEntries);
    }

    app.getBucketManager().addHotArchiveBatch(
        app, header, updatedArchivedEntries, restoredEntries);
}

bool
Protocol23CorruptionDataVerifier::loadFromFile(std::string const& path)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        CLOG_ERROR(Ledger, "Failed to open Protocol 23 corruption file: {}",
                   path);
        return false;
    }

    std::string line;
    size_t lineNumber = 0;

    // Read and skip header line
    if (!std::getline(file, line))
    {
        CLOG_ERROR(Ledger, "Protocol 23 corruption file is empty: {}", path);
        return false;
    }
    lineNumber++;

    // Process each data line
    while (std::getline(file, line))
    {
        lineNumber++;
        if (line.empty())
        {
            continue;
        }

        if (!parseLine(line, lineNumber))
        {
            CLOG_ERROR(Ledger,
                       "Failed to parse line {} in Protocol 23 corruption file",
                       lineNumber);
            return false;
        }
    }

    CLOG_INFO(Ledger,
              "Loaded Protocol 23 corruption data: {} keys, {} evicted "
              "sequences, {} restored, {} not restored",
              mKeyToEntries.size(), mEvictedSeqToKeys.size(),
              mKeyToRestoredSeq.size(), mNotRestoredKeys.size());
    // Verify the hardcoded data as soon as loading is complete.
    // Technically, before catchup has been performed we can't tell if the
    // corrupted data file is valid (i.e. it might be invalid, and hardcoded
    // data is valid), but we operate under the assumption that the hardcoded
    // data is in fact correct, i.e. first we prove that hardcoded data matches
    // the CSV, and then we prove that CSV itself is valid during catchup (and
    // thus also hardcoded data is valid as well).
    verifyHardcodedData();
    return true;
}

bool
Protocol23CorruptionDataVerifier::parseLine(std::string const& line,
                                            size_t lineNumber)
{
    // Parse CSV line: ledgerKey,correctLedgerEntry,corruptedLedgerEntry,
    // evictedLedgerSeq,restoredLedgerSeq
    std::vector<std::string> fields;
    std::stringstream ss(line);
    std::string field;

    while (std::getline(ss, field, ','))
    {
        fields.push_back(field);
    }

    if (fields.size() != 5)
    {
        CLOG_ERROR(Ledger,
                   "Line {}: Expected 5 fields, got {}. Line content: {}",
                   lineNumber, fields.size(), line);
        return false;
    }

    // Strip trailing carriage return from last field if present (CRLF line
    // endings)
    if (!fields[4].empty() && fields[4].back() == '\r')
    {
        fields[4].pop_back();
    }

    try
    {
        // Decode ledger key
        LedgerKey ledgerKey;
        fromOpaqueBase64(ledgerKey, fields[0]);
        // Decode correct ledger entry
        LedgerEntry correctEntry = decodeLedgerEntry(fields[1]);

        // Decode corrupted ledger entry
        LedgerEntry corruptedEntry = decodeLedgerEntry(fields[2]);

        // Parse evicted ledger sequence
        uint32_t evictedSeq = std::stoul(fields[3]);

        // Parse restored ledger sequence (may be "not-restored")
        bool isRestored = (fields[4] != "not-restored");
        std::optional<uint32_t> restoredSeq;
        if (isRestored)
        {
            restoredSeq = std::stoul(fields[4]);
        }

        // Store in data structures
        mKeyToEntries.emplace(ledgerKey,
                              std::make_pair(correctEntry, corruptedEntry));

        // Add to evicted sequence map
        auto& keysAtSeq = mEvictedSeqToKeys[evictedSeq];
        keysAtSeq.insert(ledgerKey);

        // Add to restored or not-restored sets
        if (restoredSeq)
        {
            mKeyToRestoredSeq.emplace(ledgerKey, *restoredSeq);
        }
        else
        {
            mNotRestoredKeys.insert(ledgerKey);
        }

        return true;
    }
    catch (std::exception const& e)
    {
        CLOG_ERROR(Ledger, "Line {}: Exception during parsing: {}", lineNumber,
                   e.what());
        return false;
    }
}

void
Protocol23CorruptionDataVerifier::verifyHardcodedData() const
{
    CLOG_INFO(Ledger,
              "Verifying hardcoded Protocol 23 corruption data against CSV...");

    // Build maps from hardcoded arrays. Both include the set of all corrupted
    // keys.
    UnorderedMap<LedgerKey, LedgerEntry> corruptedMap;
    UnorderedMap<LedgerKey, LedgerEntry> correctMap;

    for (size_t i = 0; i < P23_CORRUPTED_HOT_ARCHIVE_ENTRIES_COUNT; ++i)
    {
        LedgerEntry corruptedEntry =
            decodeLedgerEntry(P23_CORRUPTED_HOT_ARCHIVE_ENTRIES[i]);
        corruptedMap.emplace(LedgerEntryKey(corruptedEntry), corruptedEntry);

        LedgerEntry correctEntry =
            decodeLedgerEntry(P23_CORRUPTED_HOT_ARCHIVE_ENTRY_CORRECT_STATE[i]);
        correctMap.emplace(LedgerEntryKey(correctEntry), correctEntry);
    }

    // Verify that the keys in corruptedMap and correctMap match
    releaseAssert(corruptedMap.size() == correctMap.size());
    for (auto const& [key, _] : corruptedMap)
    {
        releaseAssert(correctMap.find(key) != correctMap.end());
    }

    // Verify that all keys in the hard coded maps match the CSV data.
    // mKeyToEntries is a mapping of ledger key -> <correct entry, corrupted
    // entry> pair, derived from the CSV data.
    for (auto const& [key, entryPair] : mKeyToEntries)
    {
        auto const& [csvCorrectEntry, csvCorruptedEntry] = entryPair;

        auto corruptedIt = corruptedMap.find(key);
        releaseAssert(corruptedIt != corruptedMap.end());
        releaseAssert(csvCorruptedEntry == corruptedIt->second);

        auto correctIt = correctMap.find(key);
        releaseAssert(correctIt != correctMap.end());
        releaseAssert(csvCorrectEntry == correctIt->second);
    }

    CLOG_INFO(Ledger,
              "Successfully verified {} hardcoded entries against CSV data",
              corruptedMap.size());
}

void
Protocol23CorruptionDataVerifier::verifyRestorationOfCorruptedEntry(
    LedgerKey const& restoredKey, LedgerEntry const& restoredEntry,
    uint32_t ledgerSeq, uint32_t protocolVersion)
{
    if (!protocolVersionEquals(protocolVersion, ProtocolVersion::V_23))
    {
        return;
    }
    // Modify state using mutex just in case, even though in
    // p23 we haven't increased the number of threads.
    std::lock_guard<std::mutex> lock(mMutex);

    // Check if this key is in the corrupted keys map
    auto restoredIter = mKeyToRestoredSeq.find(restoredKey);
    if (restoredIter != mKeyToRestoredSeq.end())
    {
        // Key is in the corrupted restores map. Assert that the
        // actual restoration ledger seq matches the ledger seq
        // in the CSV.
        releaseAssert(restoredIter->second == ledgerSeq);

        // Validate that the actual restored value is the
        // corrupt value from the CSV.
        auto entryIter = mKeyToEntries.find(restoredKey);
        releaseAssert(entryIter != mKeyToEntries.end());
        auto const& [correctEntry, corruptedEntry] = entryIter->second;
        releaseAssert(restoredEntry == corruptedEntry);

        // Make sure this key has not already been restored during the
        // verification period.
        releaseAssert(mKeysRestoredDuringCatchup.find(restoredKey) ==
                      mKeysRestoredDuringCatchup.end());
        mKeysRestoredDuringCatchup.insert(restoredKey);
    }

    // Assert that the key is not a key that the CSV file said
    // was not restored.
    releaseAssert(mNotRestoredKeys.find(restoredKey) == mNotRestoredKeys.end());
}

void
Protocol23CorruptionDataVerifier::verifyArchivalOfCorruptedEntry(
    EvictedStateVectors const& evictedState, Application& app,
    uint32_t ledgerSeq, uint32_t protocolVersion)
{
    if (!protocolVersionEquals(protocolVersion, ProtocolVersion::V_23))
    {
        return;
    }
    // Modify state using mutex just in case, even though in
    // p23 we haven't increased the number of threads.
    std::lock_guard<std::mutex> lock(mMutex);

    // This database can load the actual, correct version of a
    // given ledger key. This tells us the value that should
    // have been evicted.
    auto liveDatabase = app.getBucketManager()
                            .getBucketSnapshotManager()
                            .copySearchableLiveBucketListSnapshot();

    // This is the set of all keys incorrectly evicted for this
    // ledger
    auto evictedKeysIter = mEvictedSeqToKeys.find(ledgerSeq);
    UnorderedSet<LedgerKey> incorrectlyEvictedKeys =
        evictedKeysIter == mEvictedSeqToKeys.end() ? UnorderedSet<LedgerKey>()
                                                   : evictedKeysIter->second;

    // Iterate through all evicted keys and check if they are
    // corrupt.
    for (auto const& evictedEntry : evictedState.archivedEntries)
    {
        // Load the correct value from the live database.
        auto evictedLedgerKey = LedgerEntryKey(evictedEntry);
        auto databaseEntry = liveDatabase->load(evictedLedgerKey);
        releaseAssert(databaseEntry != nullptr);

        // If there was a corruption
        if (*databaseEntry != evictedEntry)
        {
            auto iter = incorrectlyEvictedKeys.find(evictedLedgerKey);

            // Require that if a key was incorrectly evicted, it
            // was in the CSV input file.
            releaseAssert(iter != incorrectlyEvictedKeys.end());

            // Assert that both the corrupt value and correct
            // value from the CSV file match the actual evicted
            // value and the actual good value from the
            // database.
            auto const& [correctCSVEntry, corruptedCSVEntry] =
                mKeyToEntries.at(evictedLedgerKey);

            releaseAssert(*databaseEntry == correctCSVEntry);
            releaseAssert(evictedEntry == corruptedCSVEntry);

            // Mark that we have seen a given key
            incorrectlyEvictedKeys.erase(iter);

            // Make sure this key has not already been evicted during the
            // verification period.
            releaseAssert(mKeysEvictedDuringCatchup.find(evictedLedgerKey) ==
                          mKeysEvictedDuringCatchup.end());
            mKeysEvictedDuringCatchup.insert(evictedLedgerKey);
        }
    }

    // Make sure that we saw and checked all keys that we
    // incorrectly evicted for this ledger.
    releaseAssert(incorrectlyEvictedKeys.empty());
}

void
Protocol23CorruptionDataVerifier::verifyEntryFixesOnP24Upgrade(
    std::vector<LedgerEntry> const& entryBatch) const
{
    // General note: this check is much more strict than the set of checks
    // during the upgrade. That's intentional as after the upgrade we'll be 100%
    // sure these pass (or we will adjust this to match unexpected state
    // changes that have happened before the upgrade).

    // Verify that the batch only contains the entry fixes, which should be the
    // case since eviction is disabled.
    releaseAssert(entryBatch.size() == mNotRestoredKeys.size());

    for (auto const& entry : entryBatch)
    {
        LedgerKey entryKey = LedgerEntryKey(entry);
        // The key must be in the not-restored set.
        releaseAssert(mNotRestoredKeys.find(entryKey) !=
                      mNotRestoredKeys.end());

        auto iter = mKeyToEntries.find(entryKey);
        releaseAssert(iter != mKeyToEntries.end());
        auto const& [correctEntry, corruptedEntry] = iter->second;
        // Verify that the entry in the batch matches the correct
        // value from the CSV file.
        releaseAssert(entry == correctEntry);
    }

    if (mKeysRestoredDuringCatchup.size() != mKeyToRestoredSeq.size())
    {
        CLOG_FATAL(
            Ledger,
            "Not all corrupted restored entries were seen during catchup: saw "
            "{}, "
            "expected {}. Make sure catchup run covers whole protocol 23.",
            mKeysRestoredDuringCatchup.size(), mKeyToRestoredSeq.size());
    }
    if (mKeysEvictedDuringCatchup.size() != mKeyToEntries.size())
    {
        CLOG_FATAL(
            Ledger,
            "Not all corrupted evicted entries were seen during catchup: saw "
            "{}, expected {}. Make sure catchup run covers whole protocol 23.",
            mKeysEvictedDuringCatchup.size(), mKeyToEntries.size());
    }
}

Protocol23CorruptionEventReconciler::Protocol23CorruptionEventReconciler(
    Hash const& networkID)
{
    for (size_t i = 0; i < P23_CORRUPTED_AFFECTED_ASSETS_COUNT; ++i)
    {
        Asset asset;
        fromOpaqueBase64(asset, P23_CORRUPTED_AFFECTED_ASSETS[i]);

        SCAddress address;
        address.type(SC_ADDRESS_TYPE_CONTRACT);
        address.contractId() = getAssetContractID(networkID, asset);

        auto [_i, inserted] = mSACAssetMap.emplace(address, asset);
        releaseAssert(inserted);
    }

    for (size_t i = 0; i < P23_CORRUPTED_HOT_ARCHIVE_ENTRIES_COUNT; ++i)
    {
        LedgerEntry corruptedEntry =
            decodeLedgerEntry(P23_CORRUPTED_HOT_ARCHIVE_ENTRIES[i]);

        LedgerEntry correctEntry =
            decodeLedgerEntry(P23_CORRUPTED_HOT_ARCHIVE_ENTRY_CORRECT_STATE[i]);

        mKeyToEntries.emplace(LedgerEntryKey(corruptedEntry),
                              std::make_pair(correctEntry, corruptedEntry));
    }
}

// This returns an int64_t because we know that for the affected SACs
// the balances always fit in the [0, INT64_MAX] range.
// We know that the only SAC entries that were incorrectly restored were
// Balances, so we can be strict with our check here.
static std::pair<int64_t, SCAddress /*owner*/>
getSACBalance(LedgerEntry const& le)
{
    releaseAssert(le.data.type() == CONTRACT_DATA);
    auto const& cd = le.data.contractData();

    releaseAssert(cd.durability == ContractDataDurability::PERSISTENT &&
                  cd.key.type() == SCV_VEC && cd.key.vec() &&
                  cd.key.vec()->size() == 2);

    SCVal balanceSymbol(SCV_SYMBOL);
    balanceSymbol.sym() = "Balance";

    // The balanceSymbol should be the first entry in the SCVec
    releaseAssert(cd.key.vec()->at(0) == balanceSymbol);

    // Now that we know this is an SAC balance entry, assert on the rest of the
    // assumptions.
    auto const& balanceOwner = cd.key.vec()->at(1);
    releaseAssert(balanceOwner.type() == SCV_ADDRESS);
    releaseAssert(cd.val.type() == SCV_MAP && cd.val.map() &&
                  cd.val.map()->size() == 3);

    SCVal amountSymbol(SCV_SYMBOL);
    amountSymbol.sym() = "amount";

    auto const& amountEntry = cd.val.map()->at(0);
    releaseAssert(amountEntry.key == amountSymbol &&
                  amountEntry.val.type() == SCV_I128);

    auto lo = amountEntry.val.i128().lo;
    auto hi = amountEntry.val.i128().hi;
    // For the range in question, we know hi is always 0.
    releaseAssert(hi == 0);
    releaseAssert(lo <= std::numeric_limits<std::int64_t>::max());

    return std::make_pair(static_cast<int64_t>(lo), balanceOwner.address());
}

std::optional<Protocol23CorruptionEventReconciler::SACReconciliationInfo>
Protocol23CorruptionEventReconciler::getSACReconciliationEventAndTrackDiff(
    LedgerKey const& restoredKey, LedgerEntry const& restoredEntry,
    uint32_t ledgerSeq, uint32_t protocolVersion)
{
    if (!protocolVersionEquals(protocolVersion, ProtocolVersion::V_23))
    {
        return std::nullopt;
    }

    // Modify state using mutex just in case, even though in
    // p23 we haven't increased the number of threads.
    // Not optimal, but this function is only called infrequently during
    // p23 catchup.
    std::lock_guard<std::mutex> lock(mMutex);

    auto entryIter = mKeyToEntries.find(restoredKey);
    if (entryIter == mKeyToEntries.end())
    {
        return std::nullopt;
    }

    if (restoredEntry.data.type() != CONTRACT_DATA)
    {
        return std::nullopt;
    }

    auto const& contractId = restoredEntry.data.contractData().contract;
    auto it = mSACAssetMap.find(contractId);
    if (it == mSACAssetMap.end())
    {
        return std::nullopt;
    }

    auto restoredBalance = getSACBalance(restoredEntry);

    auto const& [correctEntry, corruptedEntry] = entryIter->second;
    releaseAssert(restoredEntry == corruptedEntry);

    auto correctBalance = getSACBalance(correctEntry);

    // The balance addresses must match.
    releaseAssert(correctBalance.second == restoredBalance.second);

    if (correctBalance.first == restoredBalance.first)
    {
        // No change in amount, no reconciliation event.
        return std::nullopt;
    }

    auto diff = restoredBalance.first - correctBalance.first;

    Protocol23CorruptionEventReconciler::SACReconciliationInfo info;
    info.asset = it->second;
    info.mintOrBurnAddress = correctBalance.second;
    info.amount = diff;

    mReconciliationAmounts[correctBalance.second][it->second].emplace_back(
        diff);

    return info;
}

bool
Protocol23CorruptionEventReconciler::hasReconciliationAmount(
    Asset const& asset, SCAddress const& address, CxxI128 const& amount) const
{
    auto addrIt = mReconciliationAmounts.find(address);
    if (addrIt == mReconciliationAmounts.end())
    {
        return false;
    }

    auto assetIt = addrIt->second.find(asset);
    if (assetIt == addrIt->second.end())
    {
        return false;
    }

    for (auto const& a : assetIt->second)
    {
        if (rust_bridge::i128_i64_eq(amount, a))
        {
            return true;
        }
    }
    return false;
}

} // namespace p23_hot_archive_bug
} // namespace stellar
