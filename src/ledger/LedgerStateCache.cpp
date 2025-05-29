// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerStateCache.h"
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
    mContractDataEntries.emplace(
        InternalContractDataCacheEntry(ledgerEntryPtr, newLiveUntilLedgerSeq));
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
        auto ttlIt = mTTLs.find(lk.ttl().keyHash);
        releaseAssertOrThrow(ttlIt != mTTLs.end());
        ttlIt->second = newLiveUntilLedgerSeq;
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

    auto ttlIt = mTTLs.find(ttlKey.ttl().keyHash);
    if (ttlIt != mTTLs.end())
    {
        // Found orphaned TTL - adopt it and remove from temporary storage
        liveUntilLedgerSeq = ttlIt->second;
        mTTLs.erase(ttlIt);
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
        // No ContractData yet - store TTL for later
        // (Could be ContractCode TTL or orphaned ContractData TTL)
        auto ttlIt = mTTLs.find(lk.ttl().keyHash);
        releaseAssertOrThrow(ttlIt == mTTLs.end());
        mTTLs.emplace(lk.ttl().keyHash, newLiveUntilLedgerSeq);
    }
}

void
LedgerStateCache::evictTTL(LedgerKey const& ledgerKey)
{
    releaseAssertOrThrow(ledgerKey.type() == TTL);

    // Only evict from mTTLs (ContractCode TTLs)
    // ContractData TTLs are stored with their data and will be evicted
    // when evictContractData() is called
    mTTLs.erase(ledgerKey.ttl().keyHash);
}

void
LedgerStateCache::evictContractData(LedgerKey const& ledgerKey)
{
    releaseAssertOrThrow(ledgerKey.type() == CONTRACT_DATA);

    // This removes both the ContractData entry and its embedded TTL
    mContractDataEntries.erase(InternalContractDataCacheEntry(ledgerKey));
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

std::optional<uint32_t>
LedgerStateCache::getContractCodeTTL(LedgerKey const& ledgerKey) const
{
    releaseAssertOrThrow(ledgerKey.type() == LedgerEntryType::CONTRACT_CODE);

    auto ttlKey = getTTLKey(ledgerKey);
    auto it = mTTLs.find(ttlKey.ttl().keyHash);
    if (it == mTTLs.end())
    {
        return std::nullopt;
    }

    return it->second;
}

bool
LedgerStateCache::hasTTL(LedgerKey const& ledgerKey) const
{
    releaseAssertOrThrow(ledgerKey.type() == TTL);

    // Check if this is a ContractCode TTL
    if (mTTLs.find(ledgerKey.ttl().keyHash) != mTTLs.end())
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

    return false;
}
}
