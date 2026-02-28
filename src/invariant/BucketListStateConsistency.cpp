// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/BucketListStateConsistency.h"
#include "bucket/LedgerCmp.h"
#include "bucket/LiveBucket.h"
#include "invariant/Invariant.h"
#include "invariant/InvariantManager.h"
#include "ledger/InMemorySorobanState.h"
#include "ledger/LedgerStateSnapshot.h"
#include "ledger/LedgerTypeUtils.h"
#include "ledger/NetworkConfig.h"
#include "main/Application.h"
#include "util/GlobalChecks.h"
#include "util/LogSlowExecution.h"
#include "util/XDRCereal.h"
#include <fmt/format.h>

namespace stellar
{
BucketListStateConsistency::BucketListStateConsistency() : Invariant(true)
{
}

// This checks for consistency between the in-memory snapshot and live
// BucketList, along with other important properties. We check these properties:
// 1. Every live entry in the BL is reflected in the in-memory cache
// 2. No entry exists in the cache, but not the BL
// 3. Each live soroban entry also has a live TTL entry associated with it (and
//    the TTL value in the cache matches the BL value)
// 4. No TTL entry exists without a corresponding soroban entry
// 5. No live entry in the live BL is also present in the hot archive BL
// 6. Only persistent CONTRACT_DATA and CONTRACT_CODE entries exist in the
//    hot archive (TEMPORARY entries are deleted, not archived)
// 7. The cached total entry sizes match the sum of actual entry sizes
std::string
BucketListStateConsistency::checkSnapshot(
    ApplyLedgerStateSnapshot const& snapshot,
    InMemorySorobanState const& inMemorySnapshot,
    std::function<bool()> isStopping)
{
    LogSlowExecution logSlow("BucketListStateConsistency::checkSnapshot",
                             LogSlowExecution::Mode::AUTOMATIC_RAII, "took",
                             std::chrono::minutes(2));
    auto const& header = snapshot.getLedgerHeader();

    if (protocolVersionIsBefore(header.ledgerVersion, SOROBAN_PROTOCOL_VERSION))
    {
        return std::string{};
    }

    // Check property 1 for non-TTL entries. We iterate through all contract
    // code and data entries, checking that the entry exists in the cache if the
    // entry is live. We also track all live keys we see for checking TTL
    // properties later. We will check property 1 for TTL entries later when we
    // iterate through TTL entries.
    // Additionally, if persistent eviction is supported, check property 5 by
    // verifying that each live entry is not in the hot archive.
    bool checkHotArchive = protocolVersionStartsFrom(
        header.ledgerVersion,
        LiveBucket::FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION);

    UnorderedSet<LedgerKey> seenLiveNonTTLKeys;
    UnorderedSet<LedgerKey> seenDeadKeys;
    std::string errorMsg;

    // Property 7: Track total entry sizes for validation
    LedgerSnapshot lsForConfig(snapshot);
    auto sorobanConfig = SorobanNetworkConfig::loadFromLedger(lsForConfig);
    uint64_t expectedSorobanSize = 0;

    auto checkLiveEntry = [&seenLiveNonTTLKeys, &seenDeadKeys, &errorMsg,
                           &inMemorySnapshot, &snapshot, checkHotArchive,
                           &isStopping, &expectedSorobanSize, &header,
                           &sorobanConfig](BucketEntry const& be) {
        if (isStopping())
        {
            return Loop::COMPLETE;
        }

        if (be.type() == LIVEENTRY || be.type() == INITENTRY)
        {
            auto lk = LedgerEntryKey(be.liveEntry());

            // Skip if we've already seen this key (shadowed by newer version)
            if (seenLiveNonTTLKeys.find(lk) != seenLiveNonTTLKeys.end() ||
                seenDeadKeys.find(lk) != seenDeadKeys.end())
            {
                return Loop::INCOMPLETE;
            }

            // This is the newest version of this entry, check that it exists in
            // the in-memory snapshot
            auto inMemoryEntry = inMemorySnapshot.get(lk);
            if (!inMemoryEntry)
            {
                errorMsg = fmt::format(
                    FMT_STRING("BucketListStateConsistency invariant failed: "
                               "Live entry not found in InMemorySorobanState: "
                               "{}"),
                    xdrToCerealString(lk, "entryKey"));
                return Loop::COMPLETE;
            }

            // Check that the entry values match
            if (*inMemoryEntry != be.liveEntry())
            {
                errorMsg = fmt::format(
                    FMT_STRING("BucketListStateConsistency invariant failed: "
                               "Entry mismatch for key {}. BucketList entry: "
                               "{}, InMemory entry: {}"),
                    xdrToCerealString(lk, "entryKey"),
                    xdrToCerealString(be.liveEntry(), "bucketEntry"),
                    xdrToCerealString(*inMemoryEntry, "inMemoryEntry"));
                return Loop::COMPLETE;
            }

            // Check property 5: live entry should not exist in hot archive
            if (checkHotArchive && snapshot.loadArchiveEntry(lk))
            {
                errorMsg = fmt::format(
                    FMT_STRING("BucketListStateConsistency invariant failed: "
                               "Live entry is present in both live and "
                               "archived BucketList: {}"),
                    xdrToCerealString(lk, "entryKey"));
                return Loop::COMPLETE;
            }

            // Property 7: Accumulate entry size for size validation
            if (lk.type() == CONTRACT_CODE)
            {
                uint32_t entryXdrSize = xdr::xdr_size(be.liveEntry());
                expectedSorobanSize +=
                    ledgerEntrySizeForRent(be.liveEntry(), entryXdrSize,
                                           header.ledgerVersion, sorobanConfig);
            }
            else
            {
                expectedSorobanSize += xdr::xdr_size(be.liveEntry());
            }

            seenLiveNonTTLKeys.emplace(lk);
        }
        else if (be.type() == DEADENTRY)
        {
            auto lk = be.deadEntry();

            // Skip if we've already seen this key
            if (seenLiveNonTTLKeys.find(lk) != seenLiveNonTTLKeys.end() ||
                seenDeadKeys.find(lk) != seenDeadKeys.end())
            {
                return Loop::INCOMPLETE;
            }

            seenDeadKeys.emplace(lk);
        }
        return Loop::INCOMPLETE;
    };

    // First check contract data entries.
    snapshot.scanLiveEntriesOfType(CONTRACT_DATA, checkLiveEntry);

    // Note: All BucketList scans will exit early if isStopping() is true or if
    // there is an error, so we need to call shouldAbortInvariantScan after each
    // scan to make sure we don't continue checks based on bad state if we
    // stopped early.
    if (shouldAbortInvariantScan(errorMsg, isStopping))
    {
        return errorMsg;
    }

    size_t contractDataBLCount = seenLiveNonTTLKeys.size();

    // Clear seen dead keys to save memory, but don't clear seen live non-TTL
    // keys since we will need them when checking TTL entries.
    seenDeadKeys.clear();

    snapshot.scanLiveEntriesOfType(CONTRACT_CODE, checkLiveEntry);
    if (shouldAbortInvariantScan(errorMsg, isStopping))
    {
        return errorMsg;
    }

    size_t contractCodeBLCount =
        seenLiveNonTTLKeys.size() - contractDataBLCount;
    seenDeadKeys.clear();

    // Check property 2 by comparing entry counts.
    size_t inMemoryDataCount = inMemorySnapshot.getContractDataEntryCount();
    size_t inMemoryCodeCount = inMemorySnapshot.getContractCodeEntryCount();

    if (contractDataBLCount != inMemoryDataCount)
    {
        errorMsg = fmt::format(
            FMT_STRING("BucketListStateConsistency invariant failed: "
                       "CONTRACT_DATA entry count mismatch. "
                       "BucketList has {} CONTRACT_DATA entries, "
                       "InMemorySorobanState has {} CONTRACT_DATA entries"),
            contractDataBLCount, inMemoryDataCount);
        return errorMsg;
    }

    if (contractCodeBLCount != inMemoryCodeCount)
    {
        errorMsg = fmt::format(
            FMT_STRING("BucketListStateConsistency invariant failed: "
                       "CONTRACT_CODE entry count mismatch. "
                       "BucketList has {} CONTRACT_CODE entries, "
                       "InMemorySorobanState has {} CONTRACT_CODE entries"),
            contractCodeBLCount, inMemoryCodeCount);
        return errorMsg;
    }

    // Check property 3 by iterating through all TTL entries.
    // For each live TTL, verify it corresponds to a valid soroban key by
    // checking the set of keys we constructed earlier.
    // Finally, check that the TTL value in the cache matches the BL value.
    UnorderedSet<uint256> expectedTTLKeyHashes;
    for (auto const& lk : seenLiveNonTTLKeys)
    {
        auto ttlKey = getTTLKey(lk);
        expectedTTLKeyHashes.emplace(ttlKey.ttl().keyHash);
    }
    seenLiveNonTTLKeys.clear();

    UnorderedSet<LedgerKey> seenLiveTTLKeys;

    auto checkTTLEntry = [&expectedTTLKeyHashes, &seenLiveTTLKeys,
                          &seenDeadKeys, &errorMsg, &inMemorySnapshot,
                          &isStopping](BucketEntry const& be) {
        if (isStopping())
        {
            return Loop::COMPLETE;
        }

        if (be.type() == LIVEENTRY || be.type() == INITENTRY)
        {
            auto ttlKey = LedgerEntryKey(be.liveEntry());
            auto const& ttlKeyHash = ttlKey.ttl().keyHash;

            // Skip if we've already seen this key (shadowed by newer version)
            if (seenLiveTTLKeys.find(ttlKey) != seenLiveTTLKeys.end() ||
                seenDeadKeys.find(ttlKey) != seenDeadKeys.end())
            {
                return Loop::INCOMPLETE;
            }

            // Check that this TTL corresponds to a known CONTRACT_DATA or
            // CONTRACT_CODE entry
            if (expectedTTLKeyHashes.find(ttlKeyHash) ==
                expectedTTLKeyHashes.end())
            {
                errorMsg = fmt::format(
                    FMT_STRING("BucketListStateConsistency invariant failed: "
                               "TTL entry has no corresponding CONTRACT_DATA "
                               "or CONTRACT_CODE entry: {}"),
                    xdrToCerealString(ttlKey, "ttlKey"));
                return Loop::COMPLETE;
            }

            // Check that the TTL exists in the in-memory snapshot
            auto inMemoryTTL = inMemorySnapshot.get(ttlKey);
            if (!inMemoryTTL)
            {
                errorMsg = fmt::format(
                    FMT_STRING("BucketListStateConsistency invariant failed: "
                               "Live TTL entry not found in "
                               "InMemorySorobanState: {}"),
                    xdrToCerealString(ttlKey, "ttlKey"));
                return Loop::COMPLETE;
            }

            // Check that the TTL values match
            if (*inMemoryTTL != be.liveEntry())
            {
                errorMsg = fmt::format(
                    FMT_STRING("BucketListStateConsistency invariant failed: "
                               "TTL entry mismatch for {}. "
                               "BucketList TTL: {}, InMemory TTL: {}"),
                    xdrToCerealString(ttlKey, "ttlKey"),
                    xdrToCerealString(be.liveEntry(), "bucketTTL"),
                    xdrToCerealString(*inMemoryTTL, "inMemoryTTL"));
                return Loop::COMPLETE;
            }

            // Mark TTL key as seen
            expectedTTLKeyHashes.erase(ttlKeyHash);
            seenLiveTTLKeys.emplace(ttlKey);
        }
        else if (be.type() == DEADENTRY)
        {
            auto ttlKey = be.deadEntry();

            // Skip if we've already seen this key
            if (seenLiveTTLKeys.find(ttlKey) != seenLiveTTLKeys.end() ||
                seenDeadKeys.find(ttlKey) != seenDeadKeys.end())
            {
                return Loop::INCOMPLETE;
            }

            seenDeadKeys.emplace(ttlKey);
        }
        return Loop::INCOMPLETE;
    };

    seenDeadKeys.clear();
    snapshot.scanLiveEntriesOfType(TTL, checkTTLEntry);
    if (shouldAbortInvariantScan(errorMsg, isStopping))
    {
        return errorMsg;
    }

    // Check property 4
    if (!expectedTTLKeyHashes.empty())
    {
        errorMsg = fmt::format(
            FMT_STRING("BucketListStateConsistency invariant failed: "
                       "{} CONTRACT_DATA/CONTRACT_CODE entries have no "
                       "corresponding TTL entry in BucketList"),
            expectedTTLKeyHashes.size());
        return errorMsg;
    }

    // Check property 6: only persistent CONTRACT_DATA and CONTRACT_CODE entries
    // should exist in the hot archive (TEMPORARY entries are deleted, not
    // archived)
    if (checkHotArchive)
    {
        auto checkHotArchiveEntry = [&errorMsg, &isStopping](
                                        HotArchiveBucketEntry const& habe) {
            if (isStopping())
            {
                return Loop::COMPLETE;
            }

            LedgerEntryType entryType;
            ContractDataDurability durability = PERSISTENT;
            if (habe.type() == HOT_ARCHIVE_ARCHIVED)
            {
                entryType = habe.archivedEntry().data.type();
                if (entryType == CONTRACT_DATA)
                {
                    durability =
                        habe.archivedEntry().data.contractData().durability;
                }
            }
            else
            {
                releaseAssertOrThrow(habe.type() == HOT_ARCHIVE_LIVE);
                entryType = habe.key().type();
                if (entryType == CONTRACT_DATA)
                {
                    durability = habe.key().contractData().durability;
                }
            }

            if (entryType != CONTRACT_DATA && entryType != CONTRACT_CODE)
            {
                errorMsg = fmt::format(
                    FMT_STRING("BucketListStateConsistency invariant failed: "
                               "Invalid entry type {} in hot archive. Only "
                               "CONTRACT_DATA and CONTRACT_CODE are allowed"),
                    static_cast<int>(entryType));
                return Loop::COMPLETE;
            }

            if (entryType == CONTRACT_DATA && durability != PERSISTENT)
            {
                errorMsg = fmt::format(
                    FMT_STRING("BucketListStateConsistency invariant failed: "
                               "TEMPORARY CONTRACT_DATA entry in hot archive. "
                               "Only PERSISTENT entries should be archived"));
                return Loop::COMPLETE;
            }

            return Loop::INCOMPLETE;
        };

        snapshot.scanAllArchiveEntries(checkHotArchiveEntry);
        if (shouldAbortInvariantScan(errorMsg, isStopping))
        {
            return errorMsg;
        }
    }

    // Check property 7: Verify cached size matches computed size from
    // BucketList
    uint64_t cachedSize = inMemorySnapshot.getSize();
    if (expectedSorobanSize != cachedSize)
    {
        errorMsg = fmt::format(
            FMT_STRING("BucketListStateConsistency invariant failed: "
                       "Soroban state size mismatch. "
                       "BucketList total size: {} bytes, "
                       "InMemorySorobanState cached size: {} bytes"),
            expectedSorobanSize, cachedSize);
        return errorMsg;
    }

    return std::string{};
}

std::shared_ptr<Invariant>
BucketListStateConsistency::registerInvariant(Application& app)
{
    return app.getInvariantManager()
        .registerInvariant<BucketListStateConsistency>();
}

std::string
BucketListStateConsistency::getName() const
{
    return "BucketListStateConsistency";
}
}
