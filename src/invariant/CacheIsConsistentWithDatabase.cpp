// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/CacheIsConsistentWithDatabase.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerDelta.h"
#include "lib/util/format.h"
#include "main/Application.h"
#include "xdrpp/printer.h"

namespace stellar
{

std::shared_ptr<Invariant>
CacheIsConsistentWithDatabase::registerInvariant(Application& app)
{
    return app.getInvariantManager()
        .registerInvariant<CacheIsConsistentWithDatabase>(app.getDatabase());
}

CacheIsConsistentWithDatabase::CacheIsConsistentWithDatabase(Database& db)
    : Invariant(false), mDb{db}
{
}

std::string
CacheIsConsistentWithDatabase::getName() const
{
    return "CacheIsConsistentWithDatabase";
}

std::string
CacheIsConsistentWithDatabase::checkOnOperationApply(
    Operation const& operation, OperationResult const& result,
    LedgerDelta const& delta)
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
            return fmt::format(
                "Inconsistent state; entry should not exist in database: {}",
                xdr::xdr_to_string(d));
        }
    }
    return {};
}
}
