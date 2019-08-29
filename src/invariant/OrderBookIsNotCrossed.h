#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
#pragma once

// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/ByteSliceHasher.h"
#include "invariant/Invariant.h"
#include "ledger/LedgerHashUtils.h"
#include "xdr/Stellar-ledger.h"

#include <set>
#include <unordered_map>

namespace stellar
{

class Application;
struct LedgerTxnDelta;
struct OfferEntry;

// compare two OfferEntry's by price
struct OfferEntryCmp
{
    bool
    operator()(OfferEntry const& a, OfferEntry const& b) const
    {
        double priceA = double(a.price.n) / double(a.price.d);
        double priceB = double(b.price.n) / double(b.price.d);

        if (priceA < priceB)
        {
            return true;
        }
        else if (priceA == priceB)
        {
            return a.offerID < b.offerID;
        }
        else
        {
            return false;
        }
    }
};

using Orders = std::set<OfferEntry, OfferEntryCmp>;
using AssetOrders = std::unordered_map<Asset, Orders>;
using OrderBook = std::unordered_map<Asset, AssetOrders>;

class OrderBookIsNotCrossed : public Invariant
{
  public:
    explicit OrderBookIsNotCrossed() : Invariant(true)
    {
    }

    static std::shared_ptr<Invariant> registerInvariant(Application& app);

    virtual std::string getName() const override;

    virtual std::string
    checkOnOperationApply(Operation const& operation,
                          OperationResult const& result,
                          LedgerTxnDelta const& ltxDelta) override;

    OrderBook const&
    getOrderBook() const
    {
        return mOrderBook;
    }

    void snapshotForFuzzer() override;
    void resetForFuzzer() override;

  private:
    // as of right now, since this is only used in fuzzing, the mOrderBook will
    // be empty to start. If used in production, since invraiants are
    // configurable, it is likely that this invariant will be enabled with
    // pre-existing ledger and thus the mOrderBook will need a way to sync
    OrderBook mOrderBook;
    OrderBook mRolledBackOrderBook;
    void deleteFromOrderBook(OfferEntry const& oe);
    void addToOrderBook(OfferEntry const& oe);
    void updateOrderBook(LedgerTxnDelta const& ltxd);
    std::string check(std::vector<std::pair<Asset, Asset>> assetPairs);
};
}
#endif // FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
