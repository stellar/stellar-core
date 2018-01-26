// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/OfferReference.h"

namespace stellar
{

OfferReference::OfferReference(std::shared_ptr<LedgerEntryReference> const& ler)
    : mEntry(ler)
{
}

OfferEntry&
OfferReference::offer()
{
    return mEntry->entry()->data.offer();
}

OfferEntry const&
OfferReference::offer() const
{
    return mEntry->entry()->data.offer();
}

Price const&
OfferReference::getPrice() const
{
    return offer().price;
}

int64_t
OfferReference::getAmount() const
{
    return offer().amount;
}

AccountID const&
OfferReference::getSellerID() const
{
    return offer().sellerID;
}

uint64
OfferReference::getOfferID() const
{
    return offer().offerID;
}

uint32
OfferReference::getFlags() const
{
    return offer().flags;
}
}
