// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerState.h"
#include "ledger/TrustLineReference.h"
#include "util/types.h"

namespace stellar
{
using xdr::operator==;

ExplicitTrustLineReference::ExplicitTrustLineReference(
    std::shared_ptr<LedgerEntryReference> const& ler)
    : mEntry(ler)
{
}

TrustLineEntry&
ExplicitTrustLineReference::trustLine()
{
    return mEntry->entry()->data.trustLine();
}

TrustLineEntry const&
ExplicitTrustLineReference::trustLine() const
{
    return mEntry->entry()->data.trustLine();
}

int64_t
ExplicitTrustLineReference::getBalance() const
{
    return trustLine().balance;
}

bool
ExplicitTrustLineReference::addBalance(int64_t delta)
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
ExplicitTrustLineReference::isAuthorized() const
{
    return (trustLine().flags & AUTHORIZED_FLAG) != 0;
}

void
ExplicitTrustLineReference::setAuthorized(bool authorized)
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

void
ExplicitTrustLineReference::setLimit(int64_t limit)
{
    trustLine().limit = limit;
}

int64_t
ExplicitTrustLineReference::getMaxAmountReceive() const
{
    int64_t amount = 0;
    if (isAuthorized())
    {
        amount = trustLine().limit - trustLine().balance;
    }
    return amount;
}

void
ExplicitTrustLineReference::invalidate()
{
    mEntry->invalidate();
}

void
ExplicitTrustLineReference::forget(LedgerState& ls)
{
    ls.forget(mEntry);
}

void
ExplicitTrustLineReference::erase()
{
    mEntry->erase();
}

IssuerTrustLineReference::IssuerTrustLineReference()
    : mValid(true)
{
}

int64_t
IssuerTrustLineReference::getBalance() const
{
    return INT64_MAX;
}

bool
IssuerTrustLineReference::addBalance(int64_t delta)
{
    assert(mValid);
    return true;
}

bool
IssuerTrustLineReference::isAuthorized() const
{
    assert(mValid);
    return true;
}

void
IssuerTrustLineReference::setAuthorized(bool authorized)
{
    assert(mValid);
}

void
IssuerTrustLineReference::setLimit(int64_t limit)
{
    assert(mValid);
}

int64_t
IssuerTrustLineReference::getMaxAmountReceive() const
{
    assert(mValid);
    return INT64_MAX;
}

void
IssuerTrustLineReference::invalidate()
{
    mValid = false;
}

void
IssuerTrustLineReference::forget(LedgerState& ls)
{
    assert(mValid);
    // TODO(jonjove): Other checks here?
}

void
IssuerTrustLineReference::erase()
{
    assert(mValid);
}
}
