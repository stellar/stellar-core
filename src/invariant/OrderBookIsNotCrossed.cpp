#ifdef BUILD_TESTS
// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/OrderBookIsNotCrossed.h"
#include "invariant/InvariantManager.h"
#include "ledger/InternalLedgerEntry.h"
#include "ledger/LedgerTxn.h"
#include "main/Application.h"
#include "util/Logging.h"
#include "util/XDROperators.h"
#include "xdr/Stellar-ledger-entries.h"
#include <fmt/format.h>
#include <functional>
#include <xdrpp/printer.h>

namespace stellar
{
namespace
{
double
priceAsDouble(Price const& price)
{
    return static_cast<double>(price.n) / static_cast<double>(price.d);
}

OrderBookIsNotCrossed::AssetPairSet
extractAssetPairs(LedgerTxnDelta const& ltxd)
{
    OrderBookIsNotCrossed::AssetPairSet assets;
    for (auto const& entry : ltxd.entry)
    {
        if (entry.first.type() == InternalLedgerEntryType::LEDGER_ENTRY &&
            entry.first.ledgerKey().type() == OFFER && entry.second.current)
        {
            auto const& offer =
                entry.second.current->ledgerEntry().data.offer();
            auto const assetPair =
                offer.selling < offer.buying
                    ? std::make_pair(offer.selling, offer.buying)
                    : std::make_pair(offer.buying, offer.selling);

            assets.insert(assetPair);
        }
    }

    return assets;
}

std::string
checkCrossed(Asset const& a, Asset const& b,
             OrderBookIsNotCrossed::OrderBook const& orderBook)
{
    // If either side of the order book for this asset pair is empty or does not
    // yet exist, then the order book cannot be crossed.
    auto const iterAB = orderBook.find({a, b});
    auto const iterBA = orderBook.find({b, a});
    if (iterAB == orderBook.end() || iterBA == orderBook.end())
    {
        return {};
    }

    auto const& asks = iterAB->second;
    auto const& bids = iterBA->second;
    if (asks.empty() || bids.empty())
    {
        return {};
    }

    // check if crossed
    auto lowestAsk = priceAsDouble(asks.cbegin()->price);
    auto highestBidInverse = bids.cbegin()->price;
    auto highestBid =
        priceAsDouble(Price{highestBidInverse.d, highestBidInverse.n});

    if (lowestAsk <= highestBid)
    {
        if (lowestAsk == highestBid)
        {
            // We ordered non-passive offers before passive offers so that if
            // the first offer at this price in the list is passive, then we
            // know that all offers at this price in the list are passive.
            if (((asks.cbegin()->flags & PASSIVE_FLAG) != 0) ||
                ((bids.cbegin()->flags & PASSIVE_FLAG) != 0))
            {
                // In at least one of the lists, all the offers at this price
                // are passive, so the equal-price crossing does not represent a
                // bug.
                return {};
            }

            // There is at least one non-passive offer in each list, so at least
            // one of them should have crossed with an offer on the other list,
            // so we fall through to the invariant failure.
        }

        return fmt::format(
            FMT_STRING(
                "Order book is in a crossed state for {} - {} "
                "asset pair.\nTop of the book is:\n\tAsk price: {}\n\tBid "
                "price: {}\n\nWhere {} >= {}!"),
            xdr::xdr_to_string(a), xdr::xdr_to_string(b), lowestAsk, highestBid,
            highestBid, lowestAsk);
    }
    return {};
}
}

bool
OrderBookIsNotCrossed::OfferEntryCmp::operator()(OfferEntry const& a,
                                                 OfferEntry const& b) const
{
    auto const priceA = priceAsDouble(a.price);
    auto const priceB = priceAsDouble(b.price);

    if (priceA < priceB)
    {
        return true;
    }
    if (priceA == priceB)
    {
        // We order non-passive offers before passive offers so that we
        // can find any non-passive offer at a given price at the front
        // of the list of offers at that price.
        if ((a.flags & PASSIVE_FLAG) != (b.flags & PASSIVE_FLAG))
        {
            return (a.flags & PASSIVE_FLAG) == 0;
        }
        return a.offerID < b.offerID;
    }
    return false;
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
        if (entry.first.type() == InternalLedgerEntryType::LEDGER_ENTRY &&
            entry.first.ledgerKey().type() == OFFER)
        {
            if (mRestoreBeforeNextUpdate)
            {
                mOrderBook = mOrderBookSnapshot;
                mRestoreBeforeNextUpdate = false;
            }
            if (entry.second.previous)
            {
                auto const& oe =
                    entry.second.previous->ledgerEntry().data.offer();
                mOrderBook[{oe.selling, oe.buying}].erase(oe);
            }
            if (entry.second.current)
            {
                auto const& oe =
                    entry.second.current->ledgerEntry().data.offer();
                mOrderBook[{oe.selling, oe.buying}].emplace(oe);
            }
        }
    }
}

std::string
OrderBookIsNotCrossed::check(AssetPairSet const& assetPairs)
{
    assert(!mRestoreBeforeNextUpdate);
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
OrderBookIsNotCrossed::registerAndEnableInvariant(Application& app)
{
    auto invariant =
        app.getInvariantManager().registerInvariant<OrderBookIsNotCrossed>();
    app.getInvariantManager().enableInvariant(invariant->getName());
    return invariant;
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
    mOrderBookSnapshot = mOrderBook;
}

void
OrderBookIsNotCrossed::resetForFuzzer()
{
    // This call indicates that the fuzzer has completed one test and its next
    // call to `checkOnOperationApply` will be in the context of the order book
    // state immediately after the completion of setup (`mOrderBookSnapshot`).
    // However, it's possible that the next one or more fuzz tests won't
    // generate any operations involving orders, in which case it wouldn't
    // matter if `mOrderBook` were stale.  Therefore, we defer copying the map
    // until we need it to be up to date.
    mOrderBook.clear();
    mRestoreBeforeNextUpdate = true;
}
}
#endif // BUILD_TESTS
