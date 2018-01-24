// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/TrustLineReference.h"
#include "util/types.h"

namespace stellar
{
using xdr::operator==;

TrustLineReference::TrustLineReference(
    std::shared_ptr<LedgerEntryReference> const& ler)
    : mEntry(ler)
{
}

TrustLineEntry&
TrustLineReference::trustLine()
{
    return mEntry->entry()->data.trustLine();
}

TrustLineEntry const&
TrustLineReference::trustLine() const
{
    return mEntry->entry()->data.trustLine();
}

int64_t
TrustLineReference::getBalance() const
{
    return trustLine().balance;
}

bool
TrustLineReference::addBalance(int64_t delta)
{
    if (delta == 0)
    {
        return true;
    }
    if (!isAuthorized())
    {
        return false;
    }
    return stellar::addBalance(trustLine().balance, delta, trustLine().limit);
}

bool
TrustLineReference::isAuthorized() const
{
    return (trustLine().flags & AUTHORIZED_FLAG) != 0;
}

void
TrustLineReference::setAuthorized(bool authorized)
{
    if (authorized)
    {
        trustLine().flags |= AUTHORIZED_FLAG;
    }
    else
    {
        trustLine().flags &= ~AUTHORIZED_FLAG;
    }
}

int64_t
TrustLineReference::getMaxAmountReceive() const
{
    int64_t amount = 0;
    if (isAuthorized())
    {
        amount = trustLine().limit - trustLine().balance;
    }
    return amount;
}

bool
IssuerLine::addBalance(int64_t delta)
{
    return true;
}

bool
IssuerLine::isAuthorized() const
{
    return true;
}

int64_t
IssuerLine::getMaxAmountReceive() const
{
    return INT64_MAX;
}
}
