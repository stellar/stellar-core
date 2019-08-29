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
#include <xdrpp/printer.h>

namespace stellar
{

static std::set<std::pair<Asset, Asset>>
extractAssetPairs(LedgerTxnDelta const& ltxd)
{

    std::set<std::pair<Asset, Asset>> assets;
    for (auto const& entry : ltxd.entry)
    {
        if (entry.first.type() == OFFER && entry.second.current)
        {
            auto const& offer = entry.second.current->data.offer();

            auto const& assetPair = std::make_pair(offer.selling, offer.buying);
            auto const& oppositeAssetPair =
                std::make_pair(offer.buying, offer.selling);
            if (assets.find(oppositeAssetPair) == assets.end())
            {
                assets.insert(assetPair);
            }
        }
    }

    return assets;
}

static double
price(OfferEntry const& offer)
{
    return double(offer.price.n) / double(offer.price.d);
}

static std::string
checkCrossed(Asset const& a, Asset const& b, OrderBook const& orderBook)
{
    // if either side of order book for asset pair empty or does not yet exist,
    // order book cannot be crossed
    auto iterA = orderBook.find(a);
    auto iterB = orderBook.find(b);
    if (iterA == orderBook.end() || iterB == orderBook.end())
    {
        return {};
    }

    auto const& iterAB = orderBook.at(a).find(b);
    auto const& iterBA = orderBook.at(b).find(a);
    if (iterAB == (*iterA).second.end() || iterBA == (*iterB).second.end())
    {
        return {};
    }

    auto const& asks = orderBook.at(a).at(b);
    auto const& bids = orderBook.at(b).at(a);
    if (asks.empty() || bids.empty())
    {
        return {};
    }

    // check if crossed
    auto lowestAsk = price(*asks.cbegin());
    auto highestBid = 1 / price(*bids.cbegin());
    if (lowestAsk <= highestBid)
    {
        return fmt::format(
            "Order book is in a crossed state for {} - {} "
            "asset pair.\nTop of the book is:\n\tAsk price: {}\n\tBid "
            "price: {}\n\nWhere {} >= {}!",
            xdr::xdr_to_string(a), xdr::xdr_to_string(b), lowestAsk, highestBid,
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
OrderBookIsNotCrossed::check(
    std::set<std::pair<Asset, Asset>> const& assetPairs)
{
    for (auto const& assetPair : assetPairs)
    {
        auto checkCrossedResult =
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
    auto assetPairs = extractAssetPairs(ltxDelta);
    return assetPairs.size() > 0 ? check(assetPairs) : std::string{};
}

void
OrderBookIsNotCrossed::snapshotForFuzzer()
{
    mRolledBackOrderBook = mOrderBook;
};

void
OrderBookIsNotCrossed::resetForFuzzer()
{
    mOrderBook = mRolledBackOrderBook;
};
}
#endif // FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
