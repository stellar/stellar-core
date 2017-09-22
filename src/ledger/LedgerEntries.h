#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/NonCopyable.h"
#include "util/lrucache.hpp"
#include "util/optional.h"
#include "xdr/Stellar-ledger-entries.h"

#include <memory>
#include <string>

namespace stellar
{
class Database;
class LedgerDelta;
struct LedgerEntry;
struct LedgerKey;

class LedgerEntries : NonMovableOrCopyable
{
    Database& mDb;
    cache::lru_cache<std::string, optional<LedgerEntry const>> mEntryCache;

  public:
    explicit LedgerEntries(Database& db);

    Database&
    getDatabase() const
    {
        return mDb;
    }

    optional<LedgerEntry const> load(LedgerKey const& key);
    void add(LedgerEntry const& entry);
    void update(LedgerEntry const& entry);
    void remove(LedgerKey const& key);
    bool exists(LedgerKey const& key);

    bool isCached(LedgerKey const& key);
    optional<LedgerEntry const> getFromCache(LedgerKey const& key);
    void addToCache(LedgerKey const& key, optional<LedgerEntry const> p);
    void flushCache();

    void apply(LedgerDelta const& delta);
};
}
