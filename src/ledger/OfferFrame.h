#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/EntryFrame.h"

namespace stellar
{

LedgerKey offerKey(AccountID sellerID, uint64 offerID);

class OfferFrame : public EntryFrame
{
  public:
    OfferFrame();
    explicit OfferFrame(OfferEntry offer);
    explicit OfferFrame(LedgerEntry entry);

    OfferEntry
    getOffer() const
    {
        return mEntry.data.offer();
    }

    Price const& getPrice() const;
    void setPrice(Price price);
    int64_t getAmount() const;
    void setAmount(int64_t amount);
    AccountID const& getSellerID() const;
    void setSellerID(AccountID sellerID);
    Asset const& getBuying() const;
    void setBuying(Asset buying);
    Asset const& getSelling() const;
    void setSelling(Asset selling);
    uint64 getOfferID() const;
    void setOfferID(uint64 offerID);
    uint32 getFlags() const;
    bool isPassive() const;

    double computePrice() const;
    bool isValid() const;

    friend bool operator==(OfferFrame const& x, OfferFrame const& y);
    friend bool operator!=(OfferFrame const& x, OfferFrame const& y);
};
}
