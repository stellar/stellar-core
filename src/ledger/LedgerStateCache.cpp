// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerStateCache.h"
#include "util/GlobalChecks.h"

namespace stellar
{

void
LedgerStateCache::updateContractDataTTL(LedgerKey const& ledgerKey,
                                        uint32_t liveUntilLedgerSeq)
{
    releaseAssertOrThrow(ledgerKey.type() == LedgerEntryType::CONTRACT_DATA);
    auto it = mEntries.find(InternalContractDataCacheEntry(ledgerKey));
    releaseAssertOrThrow(it != mEntries.end());
    it->get().updateTTL(liveUntilLedgerSeq);
}

void
LedgerStateCache::updateContractDataEntry(
    LedgerEntry const& ledgerEntry,
    std::optional<uint32_t> liveUntilLedgerSeqOp)
{
    releaseAssertOrThrow(ledgerEntry.data.type() == CONTRACT_DATA);
    auto it = mEntries.find(
        InternalContractDataCacheEntry(LedgerEntryKey(ledgerEntry)));
    if (it == mEntries.end())
    {
        mEntries.emplace(InternalContractDataCacheEntry(
            ledgerEntry, liveUntilLedgerSeqOp ? *liveUntilLedgerSeqOp : 0));
    }
    else
    {
        if (liveUntilLedgerSeqOp)
        {
            mEntries.erase(it);
            mEntries.emplace(InternalContractDataCacheEntry(
                ledgerEntry, *liveUntilLedgerSeqOp));
        }
        else
        {
            auto liveUntilLedgerSeq = it->get().liveUntilLedgerSeq;
            mEntries.erase(it);
            mEntries.emplace(InternalContractDataCacheEntry(
                ledgerEntry, liveUntilLedgerSeq));
        }
    }
}

void
LedgerStateCache::updateContractCodeTTL(LedgerKey const& ledgerKey,
                                        uint32_t liveUntilLedgerSeq)
{
    auto it = mContractCodeTTLs.find(ledgerKey);
    if (it == mContractCodeTTLs.end())
    {
        mContractCodeTTLs.emplace(ledgerKey, liveUntilLedgerSeq);
    }
    else
    {
        it->second = liveUntilLedgerSeq;
    }
}

void
LedgerStateCache::evictKey(LedgerKey const& ledgerKey)
{
    if (ledgerKey.type() == LedgerEntryType::CONTRACT_DATA)
    {
        mEntries.erase(InternalContractDataCacheEntry(ledgerKey));
    }
    else if (ledgerKey.type() == LedgerEntryType::CONTRACT_CODE)
    {
        mContractCodeTTLs.erase(ledgerKey);
    }
    else
    {
        throw std::runtime_error("LedgerStateCache: Invalid ledger key type");
    }
}

std::optional<ContractDataCacheT>
LedgerStateCache::getContractDataEntry(LedgerKey const& ledgerKey) const
{
    releaseAssertOrThrow(ledgerKey.type() == LedgerEntryType::CONTRACT_DATA);

    auto it = mEntries.find(InternalContractDataCacheEntry(ledgerKey));
    if (it == mEntries.end())
    {
        return std::nullopt;
    }

    return it->get();
}

std::optional<uint32_t>
LedgerStateCache::getContractCodeTTL(LedgerKey const& ledgerKey) const
{
    releaseAssertOrThrow(ledgerKey.type() == LedgerEntryType::CONTRACT_CODE);

    auto it = mContractCodeTTLs.find(ledgerKey);
    if (it == mContractCodeTTLs.end())
    {
        return std::nullopt;
    }

    return it->second;
}
}
