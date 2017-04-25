// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "CacheIsConsistentWithDatabase.h"
#include "ledger/LedgerDelta.h"
#include "lib/util/format.h"
#include "xdrpp/printer.h"

namespace stellar
{

CacheIsConsistentWithDatabase::CacheIsConsistentWithDatabase(
    Database& db)
    : mDb{db}
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
    for (auto const& l : delta.getLiveEntries())
    {
        auto s = EntryFrame::checkAgainstDatabase(l, mDb);
        if (!s.empty())
        {
            return s;
        }
    }

    for (auto const& d : delta.getDeadEntries())
    {
        if (EntryFrame::exists(mDb, d))
        {
            return fmt::format("Inconsistent state; entry should not exist in database: {}",
                                xdr::xdr_to_string(d));
        }
    }

    return {};
}
}
