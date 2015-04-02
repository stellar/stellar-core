// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/EntryFrame.h"
#include "LedgerManager.h"
#include "ledger/AccountFrame.h"
#include "ledger/OfferFrame.h"
#include "ledger/TrustFrame.h"

namespace stellar
{

EntryFrame::pointer
EntryFrame::FromXDR(LedgerEntry const& from)
{
    EntryFrame::pointer res;

    switch (from.type())
    {
    case ACCOUNT:
        res = std::make_shared<AccountFrame>(from);
        break;
    case TRUSTLINE:
        res = std::make_shared<TrustFrame>(from);
        break;
    case OFFER:
        res = std::make_shared<OfferFrame>(from);
        break;
    }
    return res;
}

EntryFrame::EntryFrame(LedgerEntryType type)
    : mKeyCalculated(false), mEntry(type)
{
}

EntryFrame::EntryFrame(const LedgerEntry& from)
    : mKeyCalculated(false), mEntry(from)
{
}

LedgerKey const&
EntryFrame::getKey() const
{
    if (!mKeyCalculated)
    {
        mKey = LedgerEntryKey(mEntry);
        mKeyCalculated = true;
    }
    return mKey;
}

void
EntryFrame::storeAddOrChange(LedgerDelta& delta, Database& db) const
{
    if (exists(db, getKey()))
    {
        storeChange(delta, db);
    }
    else
    {
        storeAdd(delta, db);
    }
}

bool
EntryFrame::exists(Database& db, LedgerKey const& key)
{
    switch (key.type())
    {
    case ACCOUNT:
        return AccountFrame::exists(db, key);
    case TRUSTLINE:
        return TrustFrame::exists(db, key);
    case OFFER:
        return OfferFrame::exists(db, key);
    default:
        abort();
    }
}

void
EntryFrame::storeDelete(LedgerDelta& delta, Database& db, LedgerKey const& key)
{
    switch (key.type())
    {
    case ACCOUNT:
        AccountFrame::storeDelete(delta, db, key);
        break;
    case TRUSTLINE:
        TrustFrame::storeDelete(delta, db, key);
        break;
    case OFFER:
        OfferFrame::storeDelete(delta, db, key);
        break;
    }
}
}
