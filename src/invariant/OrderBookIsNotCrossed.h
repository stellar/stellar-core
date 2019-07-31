#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
#pragma once

// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/Invariant.h"
#include "xdr/Stellar-ledger-entries.h"

#include <unordered_map>

namespace stellar
{

class Application;
struct LedgerTxnDelta;
struct OfferEntry;

// AssetId is defined as a concatenation of issuer and asset code:
// ASSET_CODE | '-' | ISSUER_ACCOUNT_ID
using AssetId = std::string;
// Orders is a map of OfferId (int64_t) --> OfferEntry
using Orders = std::unordered_map<int64_t, OfferEntry>;
// AssetOrders, orders by asset, a map of AssetId --> map of Orders
using AssetOrders = std::unordered_map<AssetId, Orders>;
// OrderBook is a mapping of AssetId --> map of asset pair's orders
using OrderBook = std::unordered_map<AssetId, AssetOrders>;

class OrderBookIsNotCrossed : public Invariant
{
  public:
    explicit OrderBookIsNotCrossed() : Invariant(false)
    {
    }

    static std::shared_ptr<Invariant> registerInvariant(Application& app);

    virtual std::string getName() const override;

    virtual std::string
    checkOnOperationApply(Operation const& operation,
                          OperationResult const& result,
                          LedgerTxnDelta const& ltxDelta) override;

    OrderBook
    getOrderBook()
    {
        return mOrderBook;
    }

#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    void resetForFuzzer() override;
#endif // FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION

  private:
    // as of right now, since this is only used in fuzzing, the mOrderBook will
    // be empty to start. If used in production, since invraiants are
    // configurable, it is likely that this invariant will be enabled with
    // pre-existing ledger and thus the mOrderBook will need a way to sync
    OrderBook mOrderBook;
};
}
#endif // FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
