// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "CacheIsConsistentWithDatabase.h"
#include "database/EntryQueries.h"
#include "ledgerdelta/LedgerDeltaLayer.h"
#include "ledgerdelta/LedgerDelta.h"
#include "ledger/LedgerEntries.h"
#include "lib/util/format.h"
#include "xdrpp/printer.h"

namespace stellar
{

CacheIsConsistentWithDatabase::CacheIsConsistentWithDatabase(
    LedgerEntries& entries)
    : mEntries{entries}
{
}

CacheIsConsistentWithDatabase::
    ~CacheIsConsistentWithDatabase() = default;

std::string
CacheIsConsistentWithDatabase::getName() const
{
    return "cache is consistent with database";
}

std::string
CacheIsConsistentWithDatabase::check(LedgerDelta const& delta) const
{
    assert(delta.isCollapsed());

    for (auto const& l : delta.top().getLiveEntries())
    {
        auto s = checkAgainstDatabase(l, mEntries.getDatabase());
        if (!s.empty())
        {
            return s;
        }
    }

    for (auto const& d : delta.top().getDeadEntries())
    {
        if (entryExists(d, mEntries.getDatabase()))
        {
            return fmt::format("Inconsistent state; entry should not exist in database: {}",
                                xdr::xdr_to_string(d));
        }
    }

    return {};
}
}
