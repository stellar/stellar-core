#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
#pragma once

// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/ByteSliceHasher.h"
#include "invariant/Invariant.h"
#include "xdr/Stellar-ledger.h"

#include <set>
#include <unordered_map>

using namespace stellar;

namespace std
{
template <> class hash<stellar::Asset>
{
  public:
    size_t
    operator()(stellar::Asset const& asset) const
    {
        size_t res = 0;
        switch (asset.type())
        {
        case ASSET_TYPE_NATIVE:
            break;
        case ASSET_TYPE_CREDIT_ALPHANUM4:
        {
            auto const& tl4 = asset.alphaNum4();
            res ^= stellar::shortHash::computeHash(
                stellar::ByteSlice(tl4.issuer.ed25519().data(), 8));
            res ^= stellar::shortHash::computeHash(
                stellar::ByteSlice(tl4.assetCode));
            break;
        }
        case ASSET_TYPE_CREDIT_ALPHANUM12:
        {
            auto const& tl12 = asset.alphaNum12();
            res ^= stellar::shortHash::computeHash(
                stellar::ByteSlice(tl12.issuer.ed25519().data(), 8));
            res ^= stellar::shortHash::computeHash(
                stellar::ByteSlice(tl12.assetCode));
            break;
        }
        default:
            abort();
        }

        return res;
    }
};
}

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
        auto const& price = [](OfferEntry const& offer) {
            return double(offer.price.n) / double(offer.price.d);
        };

        return price(a) < price(b);
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

    void resetForFuzzer() override;

  private:
    // as of right now, since this is only used in fuzzing, the mOrderBook will
    // be empty to start. If used in production, since invraiants are
    // configurable, it is likely that this invariant will be enabled with
    // pre-existing ledger and thus the mOrderBook will need a way to sync
    OrderBook mOrderBook;
    void deleteFromOrderBook(OfferEntry const& oe);
    void addToOrderBook(OfferEntry const& oe);
    void updateOrderBook(LedgerTxnDelta const& ltxd);
    std::string check(std::vector<std::pair<Asset, Asset>> assetPairs);
};
}
#endif // FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
