// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "EntryQueries.h"
#include "database/AccountQueries.h"
#include "database/DataQueries.h"
#include "database/OfferQueries.h"
#include "database/TrustLineQueries.h"
#include "xdr/Stellar-ledger.h"

namespace stellar
{

optional<LedgerEntry const>
selectEntry(LedgerKey const& key, Database& db)
{
    switch (key.type())
    {
    case ACCOUNT:
        return selectAccount(key.account().accountID, db);
    case TRUSTLINE:
        return selectTrustLine(key.trustLine().accountID, key.trustLine().asset,
                               db);
    case OFFER:
        return selectOffer(key.offer().sellerID, key.offer().offerID, db);
    case DATA:
        return selectData(key.data().accountID, key.data().dataName, db);
    default:
        assert(false);
    }
}

void
insertEntry(LedgerEntry const& entry, Database& db)
{
    switch (entry.data.type())
    {
    case ACCOUNT:
        return insertAccount(entry, db);
    case TRUSTLINE:
        return insertTrustLine(entry, db);
    case OFFER:
        return insertOffer(entry, db);
    case DATA:
        return insertData(entry, db);
    default:
        assert(false);
    }
}

void
updateEntry(LedgerEntry const& entry, Database& db)
{
    switch (entry.data.type())
    {
    case ACCOUNT:
        return updateAccount(entry, db);
    case TRUSTLINE:
        return updateTrustLine(entry, db);
    case OFFER:
        return updateOffer(entry, db);
    case DATA:
        return updateData(entry, db);
    default:
        assert(false);
    }
}

bool
entryExists(LedgerKey const& key, Database& db)
{
    switch (key.type())
    {
    case ACCOUNT:
        return accountExists(key, db);
    case TRUSTLINE:
        return trustLineExists(key, db);
    case OFFER:
        return offerExists(key, db);
    case DATA:
        return dataExists(key, db);
    default:
        assert(false);
    }
}

void
deleteEntry(LedgerKey const& key, Database& db)
{
    switch (key.type())
    {
    case ACCOUNT:
        return deleteAccount(key, db);
    case TRUSTLINE:
        return deleteTrustLine(key, db);
    case OFFER:
        return deleteOffer(key, db);
    case DATA:
        return deleteData(key, db);
    default:
        assert(false);
    }
}
}
