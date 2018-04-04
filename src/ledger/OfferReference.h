#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerEntryReference.h"
#include "xdr/Stellar-ledger-entries.h"
#include <memory>

namespace stellar
{

class LedgerManager;

class OfferReference
{
    std::shared_ptr<LedgerEntryReference> mEntry;

  public:
    OfferReference(std::shared_ptr<LedgerEntryReference> const& ler);

    OfferEntry& offer();
    OfferEntry const& offer() const;

    Price const& getPrice() const;

    int64_t getAmount() const;

    AccountID const& getSellerID() const;

    uint64 getOfferID() const;

    uint32 getFlags() const;

    void invalidate();
};
}
