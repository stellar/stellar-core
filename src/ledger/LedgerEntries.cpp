// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerEntries.h"
#include "ledger/EntryFrame.h"
#include "ledgerdelta/LedgerDeltaLayer.h"
#include "crypto/Hex.h"
#include "crypto/SecretKey.h"
#include "database/Database.h"
#include "database/EntryQueries.h"
#include "ledgerdelta/LedgerDelta.h"
#include "util/types.h"
#include "xdrpp/marshal.h"

namespace stellar
{

LedgerEntries::LedgerEntries(Database& db) : mDb(db), mEntryCache(4096)
{
}

optional<LedgerEntry const>
LedgerEntries::load(LedgerKey const& key)
{
    if (isCached(key))
    {
        return getFromCache(key);
    }

    auto entry = selectEntry(key, getDatabase());
    if (key.type() == ACCOUNT || key.type() == TRUSTLINE)
    {
        addToCache(key, entry);
    }

    return entry;
}

void
LedgerEntries::add(LedgerEntry const& entry)
{
    if (entry.data.type() == ACCOUNT || entry.data.type() == TRUSTLINE)
    {
        addToCache(entryKey(entry), make_optional<LedgerEntry const>(entry));
    }
}

void
LedgerEntries::update(LedgerEntry const& entry)
{
    if (entry.data.type() == ACCOUNT || entry.data.type() == TRUSTLINE)
    {
        addToCache(entryKey(entry), make_optional<LedgerEntry const>(entry));
    }
}

void
LedgerEntries::remove(LedgerKey const& key)
{
    if (key.type() == ACCOUNT || key.type() == TRUSTLINE)
    {
        addToCache(key, nullptr);
    }
}

bool
LedgerEntries::exists(LedgerKey const& key)
{
    if (isCached(key))
    {
        return getFromCache(key) != nullptr;
    }

    return entryExists(key, getDatabase());
}

bool
LedgerEntries::isCached(LedgerKey const& key)
{
    auto s = binToHex(xdr::xdr_to_opaque(key));
    return mEntryCache.exists(s);
}

optional<LedgerEntry const>
LedgerEntries::getFromCache(LedgerKey const& key)
{
    auto s = binToHex(xdr::xdr_to_opaque(key));
    return mEntryCache.get(s);
}

void
LedgerEntries::addToCache(LedgerKey const& key, optional<LedgerEntry const> p)
{
    auto s = binToHex(xdr::xdr_to_opaque(key));
    mEntryCache.put(s, p);
}

void
LedgerEntries::flushCache()
{
    mEntryCache.clear();
}

void
LedgerEntries::apply(LedgerDelta const& delta)
{
    assert(delta.isCollapsed());

    for (auto const& deleted : delta.top().deletedEntries())
    {
        remove(deleted);
    }
    for (auto const& added : delta.top().newEntries())
    {
        add(added.second);
    }
    for (auto const& updated : delta.top().updatedEntries())
    {
        update(updated.second);
    }
}
}
