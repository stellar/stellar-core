// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/TrustFrame.h"
#include "util/types.h"

namespace stellar
{
using xdr::operator==;

LedgerKey trustLineKey(AccountID accountID, Asset line)
{
    auto k = LedgerKey{};
    k.type(TRUSTLINE);
    k.trustLine().accountID = std::move(accountID);
    k.trustLine().asset = std::move(line);
    return k;
}

TrustFrame::TrustFrame()
{
    mEntry.data.type(TRUSTLINE);
}

TrustFrame::TrustFrame(AccountID accountID, Asset line, int64 limit,
                       bool authorized)
{
    mEntry.data.type(TRUSTLINE);
    mEntry.data.trustLine().accountID = std::move(accountID);
    if (authorized)
    {
        mEntry.data.trustLine().flags |= AUTHORIZED_FLAG;
    }
    mEntry.data.trustLine().balance = 0;
    mEntry.data.trustLine().asset = std::move(line);
    mEntry.data.trustLine().limit = limit;
}

TrustFrame::TrustFrame(Asset line)
{
    mEntry.data.type(TRUSTLINE);
    mEntry.data.trustLine().accountID = getIssuer(line);
    mEntry.data.trustLine().flags |= AUTHORIZED_FLAG;
    mEntry.data.trustLine().balance = INT64_MAX;
    mEntry.data.trustLine().asset = std::move(line);
    mEntry.data.trustLine().limit = INT64_MAX;
}

TrustFrame::TrustFrame(TrustLineEntry trustLine)
{
    mEntry.data.type(TRUSTLINE);
    mEntry.data.trustLine() = std::move(trustLine);
}

TrustFrame::TrustFrame(LedgerEntry entry) : EntryFrame{std::move(entry)}
{
    assert(mEntry.data.type() == TRUSTLINE);
}

AccountID const&
TrustFrame::getAccountID() const
{
    return mEntry.data.trustLine().accountID;
}

void
TrustFrame::setAccountID(AccountID accountID)
{
    mEntry.data.trustLine().accountID = std::move(accountID);
}

Asset const&
TrustFrame::getAsset() const
{
    return mEntry.data.trustLine().asset;
}

void
TrustFrame::setAsset(Asset asset)
{
    mEntry.data.trustLine().asset = std::move(asset);
}

int64_t
TrustFrame::getBalance() const
{
    assert(isValid());
    return mEntry.data.trustLine().balance;
}
bool
TrustFrame::addBalance(int64_t delta)
{
    if (isIssuer() || delta == 0)
    {
        return true;
    }
    if ((mEntry.data.trustLine().flags & AUTHORIZED_FLAG) == 0)
    {
        return false;
    }
    return stellar::addBalance(mEntry.data.trustLine().balance,
                               delta, mEntry.data.trustLine().limit);
}

int64_t
TrustFrame::getLimit() const
{
    return mEntry.data.trustLine().limit;
}

void
TrustFrame::setLimit(int64_t limit)
{
    mEntry.data.trustLine().limit = limit;
}

uint32_t
TrustFrame::getFlags() const
{
    return mEntry.data.trustLine().flags;
}

bool
TrustFrame::isAuthorized() const
{
    return (mEntry.data.trustLine().flags & AUTHORIZED_FLAG) != 0;
}

void
TrustFrame::setAuthorized(bool authorized)
{
    if (authorized)
    {
        mEntry.data.trustLine().flags |= AUTHORIZED_FLAG;
    }
    else
    {
        mEntry.data.trustLine().flags &= ~AUTHORIZED_FLAG;
    }
}

int64_t
TrustFrame::getMaxAmountReceive() const
{
    assert(mEntry.data.type() == TRUSTLINE);

    if (isIssuer())
    {
        return INT64_MAX;
    }
    else if (mEntry.data.trustLine().flags & AUTHORIZED_FLAG)
    {
        return mEntry.data.trustLine().limit - mEntry.data.trustLine().balance;
    }
    return 0;
}

bool
TrustFrame::isIssuer() const
{
    return mEntry.data.trustLine().accountID ==
           getIssuer(mEntry.data.trustLine().asset);
}

bool
TrustFrame::isValid() const
{
    bool res = mEntry.data.trustLine().asset.type() != ASSET_TYPE_NATIVE;
    res = res && isAssetValid(mEntry.data.trustLine().asset);
    res = res && (mEntry.data.trustLine().balance >= 0);
    res = res && (mEntry.data.trustLine().limit > 0);
    res = res &&
          (mEntry.data.trustLine().balance <= mEntry.data.trustLine().limit);
    return res;
}
}
