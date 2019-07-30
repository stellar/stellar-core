// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/OrderBookIsNotCrossed.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerTxn.h"
#include "lib/util/format.h"
#include "main/Application.h"
#include "xdr/Stellar-ledger-entries.h"

#include <numeric>

namespace stellar
{
AssetId
getAssetID(Asset const& asset)
{
    auto r = std::string{};
    switch (asset.type())
    {
    case stellar::ASSET_TYPE_NATIVE:
        return std::string{"XLM-native"};
    case stellar::ASSET_TYPE_CREDIT_ALPHANUM4:
        assetCodeToStr(asset.alphaNum4().assetCode, r);
        return r + "-" + KeyUtils::toStrKey(getIssuer(asset));
    case stellar::ASSET_TYPE_CREDIT_ALPHANUM12:
        assetCodeToStr(asset.alphaNum12().assetCode, r);
        return r + "-" + KeyUtils::toStrKey(getIssuer(asset));
    };
}

void
deleteOffer(OrderBook& orderBook, OfferEntry const& oe)
{
    auto offerId = oe.offerID;
    auto sellAssetId = getAssetID(oe.selling);
    auto buyAssetId = getAssetID(oe.buying);
    orderBook[sellAssetId][buyAssetId].erase(offerId);
}

void
createOrModifyOffer(OrderBook& orderBook, OfferEntry const& oe)
{
    auto offerId = oe.offerID;
    auto sellAssetId = getAssetID(oe.selling);
    auto buyAssetId = getAssetID(oe.buying);
    orderBook[sellAssetId][buyAssetId][offerId] = oe;
}

void
updateOrderBook(LedgerTxnDelta const& ltxd, OrderBook& orderBook)
{
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
                createOrModifyOffer(orderBook,
                                    entry.second.current->data.offer());
            }
            else
            {
                deleteOffer(orderBook, entry.second.previous->data.offer());
            }
        }
    }
}

std::string
check(Operation const& operation, OrderBook& orderBook)
{
    auto getCheckForCrossedMessage = [&](AssetId const& a, AssetId const& b) {
        auto asks = orderBook[a][b];
        auto bids = orderBook[b][a];

        auto lowestAsk =
            std::accumulate(asks.begin(), asks.end(), __DBL_MAX__,
                            [](double lowestAsk, auto curEntry) {
                                double curAsk =
                                    double(curEntry.second.price.n) /
                                    double(curEntry.second.price.d);

                                return lowestAsk > curAsk ? curAsk : lowestAsk;
                            });

        auto highestBid = std::accumulate(
            bids.begin(), bids.end(), 0.0,
            [](double highestBid, auto curEntry) {
                double curBid = 1.0 / (double(curEntry.second.price.n) /
                                       double(curEntry.second.price.d));

                return highestBid < curBid ? curBid : highestBid;
            });

        if (highestBid >= lowestAsk)
        {
            return fmt::format(
                "Order book is in a crossed state for {} - {} asset pair.\nTop "
                "of the book is:\n\tAsk price: {}\n\tBid price: {}\n\nWhere {} "
                ">= {}!",
                a, b, lowestAsk, highestBid, highestBid, lowestAsk);
        }

        return std::string{};
    };

    if (operation.body.type() == MANAGE_BUY_OFFER)
    {
        return getCheckForCrossedMessage(
            getAssetID(operation.body.manageBuyOfferOp().buying),
            getAssetID(operation.body.manageBuyOfferOp().selling));
    }
    else if (operation.body.type() == MANAGE_SELL_OFFER)
    {
        return getCheckForCrossedMessage(
            getAssetID(operation.body.manageSellOfferOp().buying),
            getAssetID(operation.body.manageSellOfferOp().selling));
    }
    else if (operation.body.type() == CREATE_PASSIVE_SELL_OFFER)
    {
        return getCheckForCrossedMessage(
            getAssetID(operation.body.createPassiveSellOfferOp().buying),
            getAssetID(operation.body.createPassiveSellOfferOp().selling));
    }

    return {};
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
    updateOrderBook(ltxDelta, mOrderBook);
    return check(operation, mOrderBook);
}

#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
void
OrderBookIsNotCrossed::resetForFuzzer()
{
    mOrderBook = {};
};
#endif // FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
}
