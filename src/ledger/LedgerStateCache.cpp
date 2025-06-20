// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerStateCache.h"
#include "bucket/SearchableBucketList.h"
#include "ledger/LedgerTypeUtils.h"
#include "util/GlobalChecks.h"

namespace stellar
{

void
LedgerStateCache::updateContractDataTTL(
    std::unordered_set<InternalContractDataCacheEntry,
                       InternalContractDataEntryHash>::iterator dataIt,
    uint32_t newLiveUntilLedgerSeq)
{
    // Since entries are immutable, we must erase and re-insert
    auto ledgerEntryPtr = dataIt->get().ledgerEntry;
    mContractDataEntries.erase(dataIt);
    mContractDataEntries.emplace(InternalContractDataCacheEntry(
        std::move(ledgerEntryPtr), newLiveUntilLedgerSeq));
}

void
LedgerStateCache::updateTTL(LedgerEntry const& ttlEntry)
{
    releaseAssertOrThrow(ttlEntry.data.type() == TTL);

    auto lk = LedgerEntryKey(ttlEntry);
    auto newLiveUntilLedgerSeq = ttlEntry.data.ttl().liveUntilLedgerSeq;

    // TTL updates can apply to either ContractData or ContractCode entries.
    // First check if this TTL belongs to a cached ContractData entry.
    auto dataIt = mContractDataEntries.find(InternalContractDataCacheEntry(lk));
    if (dataIt != mContractDataEntries.end())
    {
        updateContractDataTTL(dataIt, newLiveUntilLedgerSeq);
    }
    else
    {
        // Since we're updating a TTL that exists, if we get here it must belong
        // to a contract code entry.
        auto codeIt = mContractCodeEntries.find(lk.ttl().keyHash);
        releaseAssertOrThrow(codeIt != mContractCodeEntries.end());
        codeIt->second.liveUntilLedgerSeq = newLiveUntilLedgerSeq;
    }
}

void
LedgerStateCache::updateContractData(LedgerEntry const& ledgerEntry)
{
    releaseAssertOrThrow(ledgerEntry.data.type() == CONTRACT_DATA);

    // Entry must already exist since this is an update
    auto lk = LedgerEntryKey(ledgerEntry);
    auto dataIt = mContractDataEntries.find(InternalContractDataCacheEntry(lk));
    releaseAssertOrThrow(dataIt != mContractDataEntries.end());

    // Preserve the existing TTL while updating the data
    auto preservedTTL = dataIt->get().liveUntilLedgerSeq;
    mContractDataEntries.erase(dataIt);
    mContractDataEntries.emplace(
        InternalContractDataCacheEntry(ledgerEntry, preservedTTL));
}

void
LedgerStateCache::createContractDataEntry(LedgerEntry const& ledgerEntry)
{
    releaseAssertOrThrow(ledgerEntry.data.type() == CONTRACT_DATA);

    // Verify entry doesn't already exist
    auto dataIt = mContractDataEntries.find(
        InternalContractDataCacheEntry(LedgerEntryKey(ledgerEntry)));
    releaseAssertOrThrow(dataIt == mContractDataEntries.end());

    // Check if we've already seen this entry's TTL (can happen during
    // initialization when TTL is written before the data)
    auto ttlKey = getTTLKey(LedgerEntryKey(ledgerEntry));
    uint32_t liveUntilLedgerSeq = 0;

    auto ttlIt = mPendingTTLs.find(ttlKey.ttl().keyHash);
    if (ttlIt != mPendingTTLs.end())
    {
        // Found orphaned TTL - adopt it and remove from temporary storage
        liveUntilLedgerSeq = ttlIt->second;
        mPendingTTLs.erase(ttlIt);
    }
    // else: TTL hasn't arrived yet, initialize to 0 (will be updated later)

    mContractDataEntries.emplace(
        InternalContractDataCacheEntry(ledgerEntry, liveUntilLedgerSeq));
}

void
LedgerStateCache::createTTL(LedgerEntry const& ttlEntry)
{
    releaseAssertOrThrow(ttlEntry.data.type() == TTL);

    auto lk = LedgerEntryKey(ttlEntry);
    auto newLiveUntilLedgerSeq = ttlEntry.data.ttl().liveUntilLedgerSeq;

    // Check if the corresponding ContractData entry already exists
    // (can happen during initialization when entries arrive out of order)
    auto dataIt = mContractDataEntries.find(InternalContractDataCacheEntry(lk));
    if (dataIt != mContractDataEntries.end())
    {
        // ContractData exists but has no TTL yet - update it
        // Verify TTL hasn't been set yet (should be default initialized)
        releaseAssertOrThrow(dataIt->get().liveUntilLedgerSeq == 0);

        updateContractDataTTL(dataIt, newLiveUntilLedgerSeq);
    }
    else
    {
        // Check if this TTL belongs to a ContractCode entry that hasn't arrived
        // yet
        auto codeIt = mContractCodeEntries.find(lk.ttl().keyHash);
        if (codeIt != mContractCodeEntries.end())
        {
            // ContractCode exists but has no TTL yet - update it
            // Verify TTL hasn't been set yet (should be default initialized)
            releaseAssertOrThrow(codeIt->second.liveUntilLedgerSeq == 0);
            codeIt->second.liveUntilLedgerSeq = newLiveUntilLedgerSeq;
        }
        else
        {
            // No ContractData or ContractCode yet - store TTL for later
            auto [_, inserted] =
                mPendingTTLs.emplace(lk.ttl().keyHash, newLiveUntilLedgerSeq);
            releaseAssertOrThrow(inserted);
        }
    }
}

void
LedgerStateCache::evictContractData(LedgerKey const& ledgerKey)
{
    releaseAssertOrThrow(ledgerKey.type() == CONTRACT_DATA);
    releaseAssertOrThrow(mContractDataEntries.erase(
                             InternalContractDataCacheEntry(ledgerKey)) == 1);
}

std::optional<ContractDataCacheT>
LedgerStateCache::getContractDataEntry(LedgerKey const& ledgerKey) const
{
    releaseAssertOrThrow(ledgerKey.type() == LedgerEntryType::CONTRACT_DATA);

    auto it =
        mContractDataEntries.find(InternalContractDataCacheEntry(ledgerKey));
    if (it == mContractDataEntries.end())
    {
        return std::nullopt;
    }

    return it->get();
}

void
LedgerStateCache::createContractCodeEntry(LedgerEntry const& ledgerEntry)
{
    releaseAssertOrThrow(ledgerEntry.data.type() == CONTRACT_CODE);

    // Get the TTL key hash
    auto ttlKey = getTTLKey(LedgerEntryKey(ledgerEntry));
    auto keyHash = ttlKey.ttl().keyHash;

    // Verify entry doesn't already exist
    auto codeIt = mContractCodeEntries.find(keyHash);
    releaseAssertOrThrow(codeIt == mContractCodeEntries.end());

    // Check if we've already seen this entry's TTL (can happen during
    // initialization when TTL is written before the code)
    uint32_t liveUntilLedgerSeq = 0;

    auto ttlIt = mPendingTTLs.find(keyHash);
    if (ttlIt != mPendingTTLs.end())
    {
        // Found orphaned TTL - adopt it and remove from temporary storage
        liveUntilLedgerSeq = ttlIt->second;
        mPendingTTLs.erase(ttlIt);
    }
    // else: TTL hasn't arrived yet, initialize to 0 (will be updated later)

    mContractCodeEntries.emplace(
        keyHash,
        ContractCodeCacheT(std::make_shared<LedgerEntry const>(ledgerEntry),
                           liveUntilLedgerSeq));
}

void
LedgerStateCache::updateContractCode(LedgerEntry const& ledgerEntry)
{
    releaseAssertOrThrow(ledgerEntry.data.type() == CONTRACT_CODE);
    auto ttlKey = getTTLKey(LedgerEntryKey(ledgerEntry));
    auto keyHash = ttlKey.ttl().keyHash;

    // Entry must already exist since this is an update
    auto codeIt = mContractCodeEntries.find(keyHash);
    releaseAssertOrThrow(codeIt != mContractCodeEntries.end());

    // Preserve the existing TTL while updating the code
    auto ttl = codeIt->second.liveUntilLedgerSeq;
    codeIt->second = ContractCodeCacheT(
        std::make_shared<LedgerEntry const>(ledgerEntry), ttl);
}

void
LedgerStateCache::evictContractCode(LedgerKey const& ledgerKey)
{
    releaseAssertOrThrow(ledgerKey.type() == CONTRACT_CODE);

    auto ttlKey = getTTLKey(ledgerKey);
    auto keyHash = ttlKey.ttl().keyHash;
    releaseAssertOrThrow(mContractCodeEntries.erase(keyHash) == 1);
}

std::optional<ContractCodeCacheT>
LedgerStateCache::getContractCodeEntry(LedgerKey const& ledgerKey) const
{
    releaseAssertOrThrow(ledgerKey.type() == LedgerEntryType::CONTRACT_CODE);

    auto ttlKey = getTTLKey(ledgerKey);
    auto keyHash = ttlKey.ttl().keyHash;

    auto it = mContractCodeEntries.find(keyHash);
    if (it == mContractCodeEntries.end())
    {
        return std::nullopt;
    }

    return it->second;
}

bool
LedgerStateCache::hasTTL(LedgerKey const& ledgerKey) const
{
    releaseAssertOrThrow(ledgerKey.type() == TTL);

    // Check if this is a pending TTL
    if (mPendingTTLs.find(ledgerKey.ttl().keyHash) != mPendingTTLs.end())
    {
        return true;
    }

    // Check if this is a ContractData TTL (stored with the data)
    auto dataIt =
        mContractDataEntries.find(InternalContractDataCacheEntry(ledgerKey));
    if (dataIt != mContractDataEntries.end())
    {
        // Only return true if TTL has been set (non-zero)
        // During initialization, entries may exist with TTL == 0
        return dataIt->get().liveUntilLedgerSeq != 0;
    }

    // Check if this is a ContractCode TTL (stored with the code)
    auto codeIt = mContractCodeEntries.find(ledgerKey.ttl().keyHash);
    if (codeIt != mContractCodeEntries.end())
    {
        // Only return true if TTL has been set (non-zero)
        // During initialization, entries may exist with TTL == 0
        return codeIt->second.liveUntilLedgerSeq != 0;
    }

    return false;
}

void
LedgerStateCache::initializeStateFromSnapshot(SearchableSnapshotConstPtr snap)
{
    releaseAssertOrThrow(mContractDataEntries.empty());
    releaseAssertOrThrow(mContractCodeEntries.empty());
    releaseAssertOrThrow(mPendingTTLs.empty());

    // Check if entry is a DEADENTRY and add it to deletedKeys. Otherwise, check
    // if the entry is shadowed by a DEADENTRY.
    std::unordered_set<LedgerKey> deletedKeys;
    auto shouldAddToCache = [&deletedKeys](BucketEntry const& be,
                                           LedgerEntryType expectedType) {
        if (be.type() == DEADENTRY)
        {
            deletedKeys.insert(be.deadEntry());
            return false;
        }

        releaseAssertOrThrow(be.type() == LIVEENTRY || be.type() == INITENTRY);
        auto lk = LedgerEntryKey(be.liveEntry());
        releaseAssertOrThrow(lk.type() == expectedType);
        return deletedKeys.find(lk) == deletedKeys.end();
    };

    auto contractDataHandler = [this,
                                &shouldAddToCache](BucketEntry const& be) {
        if (!shouldAddToCache(be, CONTRACT_DATA))
        {
            return Loop::INCOMPLETE;
        }

        auto lk = LedgerEntryKey(be.liveEntry());
        if (!getContractDataEntry(lk))
        {
            createContractDataEntry(be.liveEntry());
        }

        return Loop::INCOMPLETE;
    };

    auto ttlHandler = [this, &shouldAddToCache](BucketEntry const& be) {
        if (!shouldAddToCache(be, TTL))
        {
            return Loop::INCOMPLETE;
        }

        auto lk = LedgerEntryKey(be.liveEntry());
        if (!hasTTL(lk))
        {
            createTTL(be.liveEntry());
        }

        return Loop::INCOMPLETE;
    };

    auto contractCodeHandler = [this,
                                &shouldAddToCache](BucketEntry const& be) {
        if (!shouldAddToCache(be, CONTRACT_CODE))
        {
            return Loop::INCOMPLETE;
        }

        auto lk = LedgerEntryKey(be.liveEntry());
        if (!getContractCodeEntry(lk))
        {
            createContractCodeEntry(be.liveEntry());
        }

        return Loop::INCOMPLETE;
    };

    snap->scanForEntriesOfType(CONTRACT_DATA, contractDataHandler);
    snap->scanForEntriesOfType(TTL, ttlHandler);
    snap->scanForEntriesOfType(CONTRACT_CODE, contractCodeHandler);

    checkUpdateInvariants();
}

void
LedgerStateCache::updateState(std::vector<LedgerEntry> const& initEntries,
                              std::vector<LedgerEntry> const& liveEntries,
                              std::vector<LedgerKey> const& deadEntries)
{
    for (auto const& entry : initEntries)
    {
        if (entry.data.type() == CONTRACT_DATA)
        {
            createContractDataEntry(entry);
        }
        else if (entry.data.type() == CONTRACT_CODE)
        {
            createContractCodeEntry(entry);
        }
        else if (entry.data.type() == TTL)
        {
            createTTL(entry);
        }
    }

    for (auto const& entry : liveEntries)
    {
        if (entry.data.type() == CONTRACT_DATA)
        {
            updateContractData(entry);
        }
        else if (entry.data.type() == CONTRACT_CODE)
        {
            updateContractCode(entry);
        }
        else if (entry.data.type() == TTL)
        {
            updateTTL(entry);
        }
    }

    for (auto const& key : deadEntries)
    {
        if (key.type() == CONTRACT_DATA)
        {
            evictContractData(key);
        }
        else if (key.type() == CONTRACT_CODE)
        {
            evictContractCode(key);
        }
        // No need to evict TTLs, they are stored with their associated entry
    }

    checkUpdateInvariants();
}

void
LedgerStateCache::checkUpdateInvariants() const
{
    // No TTLs should be orphaned after finishing an update
    releaseAssertOrThrow(mPendingTTLs.empty());
}
}
