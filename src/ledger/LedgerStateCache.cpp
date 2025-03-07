// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerStateCache.h"
#include "util/GlobalChecks.h"

namespace stellar
{

void
LedgerStateCache::addContractDataEntry(LedgerEntry const& ledgerEntry,
                                       uint32_t liveUntilLedgerSeq)
{
    mEntries.emplace(
        InternalContractDataCacheEntry(ledgerEntry, liveUntilLedgerSeq));
}

void
LedgerStateCache::addContractCodeTTL(LedgerKey const& ledgerKey,
                                     uint32_t liveUntilLedgerSeq)
{
    mContractCodeTTLs.emplace(ledgerKey, liveUntilLedgerSeq);
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
