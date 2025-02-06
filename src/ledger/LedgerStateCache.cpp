// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerStateCache.h"
#include "ledger/LedgerHashUtils.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/types.h"
#include "xdrpp/printer.h"

namespace stellar
{
size_t
LedgerEntryHash::operator()(LedgerEntry const& entry) const
{
    // Extract the key from the entry and return the key's hash.
    const LedgerKey& key = LedgerEntryKey(entry);
    return std::hash<stellar::LedgerKey>()(key);
}

std::optional<LedgerEntry>
LedgerStateCache::getEntry(LedgerKey const& key) const
{
    // TODO support other types, for test mode.
    if (!supportedKeyType(key.type()))
    {
        return std::nullopt;
    }

    std::shared_lock lock(mMutex);
    if (mState.empty())
    {
        CLOG_DEBUG(Ledger, "LedgerStateCache has not yet been populated.");
        return std::nullopt;
    }

    auto it = mState.find(KeyToDummyLedgerEntry(key));
    if (it != mState.end())
    {
        return std::optional<LedgerEntry>(*it);
    }
    else
    {
        // TODO potentially return an optional if we cannot find the entry, but
        // the cache should contain all entries that are in the ledger so
        // perhaps a runtime error is appropriate. For now we will just return
        // nullopt.
        CLOG_ERROR(Ledger, "LedgerStateCache does not contain entry for key {}",
                   xdr::xdr_to_string(key));
        return std::nullopt;
    }
}

void
LedgerStateCache::addEntries(std::vector<LedgerEntry> const& initEntries,
                             std::vector<LedgerEntry> const& liveEntries,
                             std::vector<LedgerKey> const& deadEntries)
{
    std::unique_lock lock(mMutex);
    // Remove dead entries
    for (auto const& key : deadEntries)
    {
        if (!supportedKeyType(key.type()))
        {
            CLOG_DEBUG(Ledger, "Skipping unsupported key type {}", key.type());
            continue;
        }
        mState.erase(KeyToDummyLedgerEntry(key));
    }

    // Add/update live entries
    for (auto const& entry : liveEntries)
    {
        if (!supportedKeyType(entry.data.type()))
        {
            CLOG_DEBUG(Ledger, "Skipping unsupported key type {}",
                       entry.data.type());
            continue;
        }
        mState.erase(entry); // Remove old version if it exists
        mState.emplace(entry);
    }
}

void
LedgerStateCache::addEntry(LedgerEntry const& entry)
{
    std::unique_lock lock(mMutex);
    if (!supportedKeyType(entry.data.type()))
    {
        CLOG_DEBUG(Ledger, "Skipping unsupported key type {}",
                   entry.data.type());
        return;
    }
    mState.erase(entry);
    mState.emplace(entry);
}

size_t
LedgerStateCache::size() const
{
    std::shared_lock lock(mMutex);
    return mState.size();
}

bool
LedgerStateCache::supportedKeyType(LedgerEntryType type)
{
    // TODO support other types
    // possibly do something more sophisticated here
    return type == CONTRACT_DATA || type == CONTRACT_CODE || type == TTL;
}

}
