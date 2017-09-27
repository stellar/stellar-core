// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ChangedAccountsSubentriesCountIsValid.h"
#include "crypto/KeyUtils.h"
#include "crypto/SecretKey.h"
#include "database/AccountQueries.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerDelta.h"
#include "lib/util/format.h"
#include "main/Application.h"

namespace stellar
{

std::shared_ptr<Invariant>
ChangedAccountsSubentriesCountIsValid::registerInvariant(Application& app)
{
    return app.getInvariantManager()
        .registerInvariant<ChangedAccountsSubentriesCountIsValid>(
            app.getDatabase());
}

AccountID
getAccount(LedgerEntry const& entry)
{
    auto& d = entry.data;
    switch (d.type())
    {
    case ACCOUNT:
        return d.account().accountID;
    case TRUSTLINE:
        return d.trustLine().accountID;
    case OFFER:
        return d.offer().sellerID;
    case DATA:
        return d.data().accountID;
    default:
        abort();
    }
}

AccountID
getAccount(LedgerKey const& key)
{
    switch (key.type())
    {
    case ACCOUNT:
        return key.account().accountID;
    case TRUSTLINE:
        return key.trustLine().accountID;
    case OFFER:
        return key.offer().sellerID;
    case DATA:
        return key.data().accountID;
    default:
        abort();
    }
}

std::set<AccountID>
getAddedOrUpdatedAccounts(LedgerDelta const& delta)
{
    auto result = std::set<AccountID>{};
    for (auto const& c : delta.getChanges())
    {
        switch (c.type())
        {
        case LEDGER_ENTRY_CREATED:
            result.insert(getAccount(c.created()));
            break;
        case LEDGER_ENTRY_UPDATED:
            result.insert(getAccount(c.updated()));
            break;
        case LEDGER_ENTRY_REMOVED:
            if (c.removed().type() != ACCOUNT)
            {
                result.insert(getAccount(c.removed()));
            }
            break;
        default:
            break;
        }
    }
    return result;
}

std::set<AccountID>
getDeletedAccounts(LedgerDelta const& delta)
{
    auto result = std::set<AccountID>{};
    for (auto const& c : delta.getChanges())
    {
        if (c.type() == LEDGER_ENTRY_REMOVED && c.removed().type() == ACCOUNT)
        {
            result.insert(getAccount(c.removed()));
        }
    }
    return result;
}

ChangedAccountsSubentriesCountIsValid::ChangedAccountsSubentriesCountIsValid(
    Database& db)
    : mDb{db}
{
}

std::string
ChangedAccountsSubentriesCountIsValid::getName() const
{
    return "ChangedAccountsSubentriesCountIsValid";
}

std::string
ChangedAccountsSubentriesCountIsValid::checkOnLedgerClose(
    LedgerDelta const& delta)
{
    for (auto const& account : getAddedOrUpdatedAccounts(delta))
    {
        auto subentries = numberOfSubentries(account, mDb);
        if (subentries.inAccountsTable != subentries.calculated)
        {
            return fmt::format("account {} subentries count mismatch: "
                               "{} in accounts table vs {} calculated",
                               KeyUtils::toStrKey(account),
                               subentries.inAccountsTable,
                               subentries.calculated);
        }
    }

    for (auto const& account : getDeletedAccounts(delta))
    {
        auto subentries = numberOfSubentries(account, mDb);
        if (subentries.inAccountsTable != subentries.calculated ||
            subentries.inAccountsTable != 0)
        {
            return fmt::format(
                "non-exsiting account {} subentries count mismatch: {} "
                "in accounts table vs {} calculated vs 0 expected",
                KeyUtils::toStrKey(account), subentries.inAccountsTable,
                subentries.calculated);
        }
    }

    return {};
}
}
