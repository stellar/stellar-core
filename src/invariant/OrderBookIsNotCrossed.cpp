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

double
getMinOfferPrice(Orders const& orders)
{
    auto const& price = [](OfferEntry const& offer) {
        return double(offer.price.n) / double(offer.price.d);
    };

    auto const& comparator = [&](auto const& a, auto const& b) {
        return price(a.second) < price(b.second);
    };

    auto const& it = std::min_element(orders.begin(), orders.end(), comparator);

    return it == orders.end() ? __DBL_MAX__ : price(it->second);
}

std::string
check(OfferEntry const& oe, OrderBook const& orderBook)
{
    auto const a = oe.selling;
    auto const b = oe.buying;

    // if either side of order book for asset pair empty or does not yet exist,
    // order book cannot be crossed
    if (orderBook.find(a) == orderBook.end() ||
        orderBook.find(b) == orderBook.end() ||
        orderBook.at(a).find(b) == orderBook.at(a).end() ||
        orderBook.at(b).find(a) == orderBook.at(b).end())
    {
        return std::string{};
    }

    auto asks = orderBook.at(a).at(b);
    auto bids = orderBook.at(b).at(a);

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

        return fmt::format("Order book is in a crossed state for {} - {} "
                           "asset pair.\nTop "
                           "of the book is:\n\tAsk price: {}\n\tBid "
                           "price: {}\n\nWhere {} "
                           ">= {}!",
                           assetToString(oe.selling), assetToString(oe.buying),
                           lowestAsk, highestBid, highestBid, lowestAsk);
    }

    return std::string{};
}

void
OrderBookIsNotCrossed::deleteOffer(OfferEntry const& oe)
{
    mOrderBook[oe.selling][oe.buying].erase(oe.offerID);
}

void
OrderBookIsNotCrossed::createOrModifyOffer(OfferEntry const& oe)
{
    mOrderBook[oe.selling][oe.buying][oe.offerID] = oe;
}

std::string
OrderBookIsNotCrossed::updateOrderBookAndCheck(LedgerTxnDelta const& ltxd)
{
    std::shared_ptr<const stellar::LedgerEntry> lastOffer;
    for (auto const& entry : ltxd.entry)
    {
        // there are three possible "deltas" for an offer:
        //      CREATED:  (nil)     -> LedgerKey
        //      MODIFIED: LedgerKey -> LedgerKey
        //      DELETED:  LedgerKey -> (nil)
        if (entry.first.type() == OFFER)
        {
            if (entry.second.current)
            {
                createOrModifyOffer(entry.second.current->data.offer());
            }
            else
            {
                deleteOffer(entry.second.previous->data.offer());
            }

            lastOffer = entry.second.current ? entry.second.current
                                             : entry.second.previous;
        }
    }

    return lastOffer ? check(lastOffer->data.offer(), mOrderBook)
                     : std::string{};
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
    return updateOrderBookAndCheck(ltxDelta);
}

void
OrderBookIsNotCrossed::resetForFuzzer()
{
    mOrderBook.clear();
};
}
#endif // FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
