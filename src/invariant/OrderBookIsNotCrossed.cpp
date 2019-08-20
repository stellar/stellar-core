#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/OrderBookIsNotCrossed.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerTxn.h"
#include "lib/util/format.h"
#include "main/Application.h"
#include "xdr/Stellar-ledger-entries.h"

namespace stellar
{

std::vector<std::pair<Asset, Asset>>
extractAssetPairs(Operation const& op, LedgerTxnDelta const& ltxd)
{
    switch (op.body.type())
    {
    case MANAGE_BUY_OFFER:
    {
        auto const& offer = op.body.manageBuyOfferOp();
        return {std::make_pair(offer.selling, offer.buying)};
    }
    case MANAGE_SELL_OFFER:
    {
        auto const& offer = op.body.manageSellOfferOp();
        return {std::make_pair(offer.selling, offer.buying)};
    }
    case CREATE_PASSIVE_SELL_OFFER:
    {
        auto const& offer = op.body.createPassiveSellOfferOp();
        return {std::make_pair(offer.selling, offer.buying)};
    }
    case PATH_PAYMENT:
    {
        auto const& pp = op.body.pathPaymentOp();
       
        // if no path, only have a pair between send and dest
        if (pp.path.size() == 0)
        {
            return {std::make_pair(pp.sendAsset, pp.destAsset)};
        } 

        // For send, dest, {A, B} we get: {send, A}, {A, B}, {B, dest}
        std::vector<std::pair<Asset, Asset>> assets;
        
        // beginning: send -> A
        assets.emplace_back(pp.sendAsset, pp.path.front()); 
        for (int i = 1; i < pp.path.size(); ++i)
        {
            // middle: A -> B
            assets.emplace_back(pp.path[i - 1], pp.path[i]);
        }
        // end: B -> dest
        assets.emplace_back(pp.path.back(), pp.destAsset); 

        return assets;
    }
    case ALLOW_TRUST:
    {
        auto const& at = op.body.allowTrustOp();

        // if revoke auth, all offers for that user against that asset are
        // deleted
        if (!at.authorize)
        {
            std::vector<std::pair<Asset, Asset>> assets;
            // since we only get one side of the asset pair from the operation,
            // we derive the rest of the information from the
            // LedgerTxnDelta entries
            for (auto const& entry : ltxd.entry)
            {
                if (entry.second.previous &&
                    entry.second.previous->data.type() == OFFER)
                {
                    auto const& offer = entry.second.previous->data.offer();
                    assets.emplace_back(offer.selling, offer.buying);
                }
            }

            return assets;
        }

        return {};
    }
    default:
        return {};
    }
}

double
price(OfferEntry const& offer)
{
    return double(offer.price.n) / double(offer.price.d);
}

double
getMinOfferPrice(Orders const& orders)
{
    return orders.cbegin() == orders.cend() ? __DBL_MAX__
                                            : price(*orders.cbegin());
}

std::string
checkCrossed(Asset const& a, Asset const& b, OrderBook const& orderBook)
{
    // if either side of order book for asset pair empty or does not yet exist,
    // order book cannot be crossed
    if (orderBook.find(a) == orderBook.end() ||
        orderBook.find(b) == orderBook.end() ||
        orderBook.at(a).find(b) == orderBook.at(a).end() ||
        orderBook.at(b).find(a) == orderBook.at(b).end())
    {
        return {};
    }

    auto const& asks = orderBook.at(a).at(b);
    auto const& bids = orderBook.at(b).at(a);

    auto lowestAsk = getMinOfferPrice(asks);
    auto highestBid = 1.0 / getMinOfferPrice(bids);

    if (highestBid >= lowestAsk)
    {
        auto assetToString = [](Asset const& asset) {
            auto r = std::string{};
            switch (asset.type())
            {
            case stellar::ASSET_TYPE_NATIVE:
                r = std::string{"XLM"};
                break;
            case stellar::ASSET_TYPE_CREDIT_ALPHANUM4:
                assetCodeToStr(asset.alphaNum4().assetCode, r);
                break;
            case stellar::ASSET_TYPE_CREDIT_ALPHANUM12:
                assetCodeToStr(asset.alphaNum12().assetCode, r);
                break;
            }
            return r;
        };
        return fmt::format(
            "Order book is in a crossed state for {} - {} "
            "asset pair.\nTop of the book is:\n\tAsk price: {}\n\tBid "
            "price: {}\n\nWhere {} >= {}!",
            assetToString(a), assetToString(b), lowestAsk, highestBid,
            highestBid, lowestAsk);
    }
    return {};
}

void
OrderBookIsNotCrossed::deleteFromOrderBook(OfferEntry const& oe)
{
    mOrderBook[oe.selling][oe.buying].erase(oe);
}

void
OrderBookIsNotCrossed::addToOrderBook(OfferEntry const& oe)
{
    mOrderBook[oe.selling][oe.buying].emplace(oe);
}

void
OrderBookIsNotCrossed::updateOrderBook(LedgerTxnDelta const& ltxd)
{
    for (auto const& entry : ltxd.entry)
    {
        // there are three possible "deltas" for an offer:
        //      CREATED:  (nil)     -> LedgerKey
        //      MODIFIED: LedgerKey -> LedgerKey
        //      DELETED:  LedgerKey -> (nil)
        if (entry.first.type() == OFFER)
        {
            if (entry.second.previous)
            {
                deleteFromOrderBook(entry.second.previous->data.offer());
            }
            if (entry.second.current)
            {
                addToOrderBook(entry.second.current->data.offer());
            }
        }
    }
}

std::string
OrderBookIsNotCrossed::check(std::vector<std::pair<Asset, Asset>> assetPairs)
{
    for (auto const& assetPair : assetPairs)
    {
        auto const& checkCrossedResult =
            checkCrossed(assetPair.first, assetPair.second, mOrderBook);
        if (!checkCrossedResult.empty())
        {
            return checkCrossedResult;
        }
    }

    return std::string{};
}

std::shared_ptr<Invariant>
OrderBookIsNotCrossed::registerInvariant(Application& app)
{
    return app.getInvariantManager().registerInvariant<OrderBookIsNotCrossed>();
}
std::string
OrderBookIsNotCrossed::getName() const
{
    return "OrderBookIsNotCrossed";
}

std::string
OrderBookIsNotCrossed::checkOnOperationApply(Operation const& operation,
                                             OperationResult const& result,
                                             LedgerTxnDelta const& ltxDelta)
{
    updateOrderBook(ltxDelta);
    auto assetPairs = extractAssetPairs(operation, ltxDelta);
    return assetPairs.size() > 0 ? check(assetPairs) : std::string{};
}

void
OrderBookIsNotCrossed::resetForFuzzer()
{
    mOrderBook.clear();
};
}
#endif // FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
