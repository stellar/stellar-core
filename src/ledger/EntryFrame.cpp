// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/EntryFrame.h"
#include "LedgerManager.h"
#include "crypto/Hex.h"
#include "database/Database.h"
#include "database/EntryQueries.h"
#include "ledger/AccountFrame.h"
#include "ledger/DataFrame.h"
#include "ledger/LedgerEntries.h"
#include "ledger/OfferFrame.h"
#include "ledger/TrustFrame.h"
#include "xdrpp/marshal.h"
#include "xdrpp/printer.h"

namespace stellar
{
using xdr::operator==;
using xdr::operator!=;

std::string
checkAgainstDatabase(LedgerEntry const& entry, Database& db)
{
    auto key = entryKey(entry);
    auto const& fromDb = selectEntry(key, db);
    if (fromDb && (*fromDb == entry))
    {
        return {};
    }

    auto s = std::string{"Inconsistent state between objects: "};
    if (fromDb)
    {
        s += xdr::xdr_to_string(*fromDb, "db");
    }
    else
    {
        s += "db: (null)\n";
    }
    s += xdr::xdr_to_string(entry, "live");
    return s;
}

EntryFrame::EntryFrame(LedgerEntry entry) : mEntry{std::move(entry)}
{
}

LedgerKey
EntryFrame::getKey() const
{
    return entryKey(mEntry);
}

LedgerKey
entryKey(LedgerEntry const& e)
{
    auto& d = e.data;
    switch (d.type())
    {
    case ACCOUNT:
        return accountKey(d.account().accountID);
    case TRUSTLINE:
        return trustLineKey(d.trustLine().accountID, d.trustLine().asset);
    case OFFER:
        return offerKey(d.offer().sellerID, d.offer().offerID);
    case DATA:
        return dataKey(d.data().accountID, d.data().dataName);
    default:
        assert(false);
    }
}
}
