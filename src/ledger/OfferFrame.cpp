// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/OfferFrame.h"
#include "util/types.h"

namespace stellar
{

using xdr::operator==;

LedgerKey offerKey(AccountID sellerID, uint64 offerID)
{
    auto k = LedgerKey{};
    k.type(OFFER);
    k.offer().sellerID = std::move(sellerID);
    k.offer().offerID = offerID;
    return k;
}

OfferFrame::OfferFrame()
{
    mEntry.data.type(OFFER);
}

OfferFrame::OfferFrame(OfferEntry offer)
{
    mEntry.data.type(OFFER);
    mEntry.data.offer() = offer;
}

OfferFrame::OfferFrame(LedgerEntry entry) : EntryFrame{std::move(entry)}
{
    assert(mEntry.data.type() == OFFER);
}

Price const&
OfferFrame::getPrice() const
{
    return mEntry.data.offer().price;
}

void
OfferFrame::setPrice(Price price)
{
    mEntry.data.offer().price = std::move(price);
}

int64_t
OfferFrame::getAmount() const
{
    return mEntry.data.offer().amount;
}

void
OfferFrame::setAmount(int64_t amount)
{
    mEntry.data.offer().amount = amount;
}

AccountID const&
OfferFrame::getSellerID() const
{
    return mEntry.data.offer().sellerID;
}

void
OfferFrame::setSellerID(AccountID sellerID)
{
    mEntry.data.offer().sellerID = std::move(sellerID);
}

Asset const&
OfferFrame::getBuying() const
{
    return mEntry.data.offer().buying;
}

void
OfferFrame::setBuying(Asset buying)
{
    mEntry.data.offer().buying = buying;
}

Asset const&
OfferFrame::getSelling() const
{
    return mEntry.data.offer().selling;
}

void
OfferFrame::setSelling(Asset selling)
{
    mEntry.data.offer().selling = selling;
}

uint64
OfferFrame::getOfferID() const
{
    return mEntry.data.offer().offerID;
}

void
OfferFrame::setOfferID(uint64 offerID)
{
    mEntry.data.offer().offerID = offerID;
}

uint32
OfferFrame::getFlags() const
{
    return mEntry.data.offer().flags;
}

bool
OfferFrame::isPassive() const
{
    return (getFlags() & PASSIVE_FLAG) == PASSIVE_FLAG;
}

double
OfferFrame::computePrice() const
{
    return double(mEntry.data.offer().price.n) /
           double(mEntry.data.offer().price.d);
}

bool
OfferFrame::isValid() const
{
    return isAssetValid(mEntry.data.offer().buying) &&
           isAssetValid(mEntry.data.offer().selling);
}

bool
operator==(OfferFrame const& x, OfferFrame const& y)
{
    return x.mEntry == y.mEntry;
}

bool
operator!=(OfferFrame const& x, OfferFrame const& y)
{
    return !(x == y);
}
}
