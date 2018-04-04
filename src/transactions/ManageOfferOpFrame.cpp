// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "transactions/ManageOfferOpFrame.h"
#include "OfferExchange.h"
#include "database/Database.h"
#include "ledger/LedgerHeaderReference.h"
#include "ledger/LedgerState.h"
#include "main/Application.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "transactions/TransactionUtils.h"
#include "util/Logging.h"
#include "util/types.h"

#include "ledger/AccountReference.h"
#include "ledger/OfferReference.h"
#include "ledger/TrustLineReference.h"

// convert from sheep to wheat
// selling sheep
// buying wheat

namespace stellar
{

using namespace std;
using xdr::operator==;

ManageOfferOpFrame::ManageOfferOpFrame(Operation const& op,
                                       OperationResult& res,
                                       TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mManageOffer(mOperation.body.manageOfferOp())
{
    mPassive = false;
}

bool
ManageOfferOpFrame::checkOfferValid(medida::MetricsRegistry& metrics,
                                    LedgerState& ls)
{
    Asset const& sheep = mManageOffer.selling;
    Asset const& wheat = mManageOffer.buying;

    if (mManageOffer.amount == 0)
    {
        // don't bother loading trust lines as we're deleting the offer
        return true;
    }

    if (sheep.type() != ASSET_TYPE_NATIVE)
    {
        auto sheepLineA = loadTrustLine(ls, getSourceID(), sheep);
        auto issuer = stellar::loadAccount(ls, getIssuer(sheep));
        if (!issuer)
        {
            metrics
                .NewMeter({"op-manage-offer", "invalid", "sell-no-issuer"},
                          "operation")
                .Mark();
            innerResult().code(MANAGE_OFFER_SELL_NO_ISSUER);
            return false;
        }
        if (!sheepLineA)
        { // we don't have what we are trying to sell
            metrics
                .NewMeter({"op-manage-offer", "invalid", "sell-no-trust"},
                          "operation")
                .Mark();
            innerResult().code(MANAGE_OFFER_SELL_NO_TRUST);
            return false;
        }
        if (sheepLineA->getBalance() == 0)
        {
            metrics
                .NewMeter({"op-manage-offer", "invalid", "underfunded"},
                          "operation")
                .Mark();
            innerResult().code(MANAGE_OFFER_UNDERFUNDED);
            return false;
        }
        if (!sheepLineA->isAuthorized())
        {
            metrics
                .NewMeter({"op-manage-offer", "invalid", "sell-not-authorized"},
                          "operation")
                .Mark();
            // we are not authorized to sell
            innerResult().code(MANAGE_OFFER_SELL_NOT_AUTHORIZED);
            return false;
        }
        sheepLineA->forget(ls);
        issuer.forget(ls);
    }

    if (wheat.type() != ASSET_TYPE_NATIVE)
    {
        auto wheatLineA = loadTrustLine(ls, getSourceID(), wheat);
        auto issuer = stellar::loadAccount(ls, getIssuer(wheat));
        if (!issuer)
        {
            metrics
                .NewMeter({"op-manage-offer", "invalid", "buy-no-issuer"},
                          "operation")
                .Mark();
            innerResult().code(MANAGE_OFFER_BUY_NO_ISSUER);
            return false;
        }
        if (!wheatLineA)
        { // we can't hold what we are trying to buy
            metrics
                .NewMeter({"op-manage-offer", "invalid", "buy-no-trust"},
                          "operation")
                .Mark();
            innerResult().code(MANAGE_OFFER_BUY_NO_TRUST);
            return false;
        }
        if (!wheatLineA->isAuthorized())
        { // we are not authorized to hold what we
            // are trying to buy
            metrics
                .NewMeter({"op-manage-offer", "invalid", "buy-not-authorized"},
                          "operation")
                .Mark();
            innerResult().code(MANAGE_OFFER_BUY_NOT_AUTHORIZED);
            return false;
        }
        wheatLineA->forget(ls);
        issuer.forget(ls);
    }
    return true;
}

// you are selling sheep for wheat
// need to check the counter offers selling wheat for sheep
// see if this is modifying an old offer
// see if this offer crosses any existing offers
bool
ManageOfferOpFrame::doApply(Application& app, LedgerState& lsOuter)
{
    LedgerState ls(lsOuter);
    if (!checkOfferValid(app.getMetrics(), ls))
    {
        return false;
    }

    Asset const& sheep = mManageOffer.selling;
    Asset const& wheat = mManageOffer.buying;

    bool creatingNewOffer = false;
    uint64_t offerID = mManageOffer.offerID;

    LedgerEntry newOffer;
    newOffer.data.type(OFFER);
    if (offerID)
    { // modifying an old offer
        auto sellSheepOffer = loadOffer(ls, getSourceID(), offerID);
        if (!sellSheepOffer)
        {
            app.getMetrics()
                .NewMeter({"op-manage-offer", "invalid", "not-found"},
                          "operation")
                .Mark();
            innerResult().code(MANAGE_OFFER_NOT_FOUND);
            return false;
        }

        // rebuild offer based off the manage offer
        newOffer.data.offer() = buildOffer(
            getSourceID(), mManageOffer, sellSheepOffer->entry()->data.offer().flags);
        mPassive = sellSheepOffer->entry()->data.offer().flags & PASSIVE_FLAG;

        sellSheepOffer->erase();
    }
    else
    { // creating a new Offer
        creatingNewOffer = true;
        newOffer.data.offer() = buildOffer(getSourceID(), mManageOffer,
                                     mPassive ? PASSIVE_FLAG : 0);
    }

    int64_t maxSheepSend = newOffer.data.offer().amount;
    int64_t maxAmountOfSheepCanSell;

    innerResult().code(MANAGE_OFFER_SUCCESS);

    if (mManageOffer.amount == 0)
    {
        newOffer.data.offer().amount = 0;
    }
    else
    {
        if (sheep.type() == ASSET_TYPE_NATIVE)
        {
            auto sourceAccount = loadSourceAccount(ls);
            auto header = ls.loadHeader();
            if (creatingNewOffer && getCurrentLedgerVersion(header) > 8)
            {
                // we need to compute maxAmountOfSheepCanSell based on the
                // updated reserve to avoid selling too many and falling
                // below the reserve when we try to create the offer later on
                if (!sourceAccount.addNumEntries(header, 1))
                {
                    app.getMetrics()
                        .NewMeter({"op-manage-offer", "invalid", "low reserve"},
                                  "operation")
                        .Mark();
                    innerResult().code(MANAGE_OFFER_LOW_RESERVE);
                    return false;
                }
                maxAmountOfSheepCanSell =
                    sourceAccount.getBalanceAboveReserve(header);
                // restore the number back (will be re-incremented later if
                // the offer really needs to be created)
                sourceAccount.addNumEntries(header, -1);
            }
            else
            {
                maxAmountOfSheepCanSell =
                    sourceAccount.getBalanceAboveReserve(header);
            }
            header->invalidate();
            sourceAccount.forget(ls);
        }
        else
        {
            auto sheepLineA = loadTrustLine(ls, getSourceID(), sheep);
            maxAmountOfSheepCanSell = sheepLineA->getBalance();
            sheepLineA->forget(ls);
        }

        // the maximum is defined by how much wheat it can receive
        int64_t maxWheatCanBuy;
        if (wheat.type() == ASSET_TYPE_NATIVE)
        {
            maxWheatCanBuy = INT64_MAX;
        }
        else
        {
            auto wheatLineA = loadTrustLine(ls, getSourceID(), wheat);
            maxWheatCanBuy = wheatLineA->getMaxAmountReceive();
            if (maxWheatCanBuy == 0)
            {
                app.getMetrics()
                    .NewMeter({"op-manage-offer", "invalid", "line-full"},
                              "operation")
                    .Mark();
                innerResult().code(MANAGE_OFFER_LINE_FULL);
                return false;
            }
            wheatLineA->forget(ls);
        }

        Price const& sheepPrice = newOffer.data.offer().price;

        {
            int64_t maxSheepBasedOnWheat;
            if (!bigDivide(maxSheepBasedOnWheat, maxWheatCanBuy, sheepPrice.d,
                           sheepPrice.n, ROUND_DOWN))
            {
                maxSheepBasedOnWheat = INT64_MAX;
            }

            if (maxAmountOfSheepCanSell > maxSheepBasedOnWheat)
            {
                maxAmountOfSheepCanSell = maxSheepBasedOnWheat;
            }
        }

        // amount of sheep for sale is the lesser of amount we can sell and
        // amount put in the offer
        if (maxAmountOfSheepCanSell < maxSheepSend)
        {
            maxSheepSend = maxAmountOfSheepCanSell;
        }

        int64_t sheepSent, wheatReceived;

        OfferExchange oe;

        const Price maxWheatPrice(sheepPrice.d, sheepPrice.n);

        OfferExchange::ConvertResult r = oe.convertWithOffers(
            ls, sheep, maxSheepSend, sheepSent, wheat, maxWheatCanBuy,
            wheatReceived, [this, &maxWheatPrice](OfferReference o) {
                if ((mPassive && (o.getPrice() >= maxWheatPrice)) ||
                    (o.getPrice() > maxWheatPrice))
                {
                    return OfferExchange::eStop;
                }
                if (o.getSellerID() == getSourceID())
                {
                    // we are crossing our own offer
                    innerResult().code(MANAGE_OFFER_CROSS_SELF);
                    return OfferExchange::eStop;
                }
                return OfferExchange::eKeep;
            });

        assert(sheepSent >= 0);

        switch (r)
        {
        case OfferExchange::eOK:
        case OfferExchange::ePartial:
            break;
        case OfferExchange::eFilterStop:
            if (innerResult().code() != MANAGE_OFFER_SUCCESS)
            {
                return false;
            }
            break;
        }

        // updates the result with the offers that got taken on the way

        for (auto const& oatom : oe.getOfferTrail())
        {
            innerResult().success().offersClaimed.push_back(oatom);
        }

        if (wheatReceived > 0)
        {
            if (wheat.type() == ASSET_TYPE_NATIVE)
            {
                auto sourceAccount = loadSourceAccount(ls);
                if (!sourceAccount.addBalance(wheatReceived))
                {
                    // this would indicate a bug in OfferExchange
                    throw std::runtime_error("offer claimed over limit");
                }
                sourceAccount.invalidate();
            }
            else
            {
                auto wheatLineA = loadTrustLine(ls, getSourceID(), wheat);
                if (!wheatLineA->addBalance(wheatReceived))
                {
                    // this would indicate a bug in OfferExchange
                    throw std::runtime_error("offer claimed over limit");
                }
                wheatLineA->invalidate();
            }

            if (sheep.type() == ASSET_TYPE_NATIVE)
            {
                auto sourceAccount = loadSourceAccount(ls);
                if (!sourceAccount.addBalance(-sheepSent))
                {
                    // this would indicate a bug in OfferExchange
                    throw std::runtime_error("offer sold more than balance");
                }
                sourceAccount.invalidate();
            }
            else
            {
                auto sheepLineA = loadTrustLine(ls, getSourceID(), sheep);
                if (!sheepLineA->addBalance(-sheepSent))
                {
                    // this would indicate a bug in OfferExchange
                    throw std::runtime_error("offer sold more than balance");
                }
                sheepLineA->invalidate();
            }
        }

        // recomputes the amount of sheep for sale
        newOffer.data.offer().amount = maxSheepSend - sheepSent;
    }

    if (newOffer.data.offer().amount > 0)
    { // we still have sheep to sell so leave an offer
        if (creatingNewOffer)
        {
            // make sure we don't allow us to add offers when we don't have
            // the minbalance (should never happen at this stage in v9+)
            auto sourceAccount = loadSourceAccount(ls);
            auto header = ls.loadHeader();
            if (!sourceAccount.addNumEntries(header, 1))
            {
                app.getMetrics()
                    .NewMeter({"op-manage-offer", "invalid", "low reserve"},
                              "operation")
                    .Mark();
                innerResult().code(MANAGE_OFFER_LOW_RESERVE);
                return false;
            }

            newOffer.data.offer().offerID = ++header->header().idPool;
            header->invalidate();

            auto sellSheepOffer = ls.create(newOffer);
            innerResult().success().offer.effect(MANAGE_OFFER_CREATED);
        }
        else
        {
            auto sellSheepOffer = ls.create(newOffer);
            innerResult().success().offer.effect(MANAGE_OFFER_UPDATED);
        }
        innerResult().success().offer.offer() = newOffer.data.offer();
    }
    else
    {
        innerResult().success().offer.effect(MANAGE_OFFER_DELETED);

        if (!creatingNewOffer)
        {
            auto sourceAccount = loadSourceAccount(ls);
            auto header = ls.loadHeader();
            sourceAccount.addNumEntries(header, -1);
            header->invalidate();
            sourceAccount.invalidate();
        }
    }

    ls.commit();

    app.getMetrics()
        .NewMeter({"op-create-offer", "success", "apply"}, "operation")
        .Mark();
    return true;
}

// makes sure the currencies are different
bool
ManageOfferOpFrame::doCheckValid(Application& app, uint32_t ledgerVersion)
{
    Asset const& sheep = mManageOffer.selling;
    Asset const& wheat = mManageOffer.buying;

    if (!isAssetValid(sheep) || !isAssetValid(wheat))
    {
        app.getMetrics()
            .NewMeter({"op-manage-offer", "invalid", "invalid-asset"},
                      "operation")
            .Mark();
        innerResult().code(MANAGE_OFFER_MALFORMED);
        return false;
    }
    if (compareAsset(sheep, wheat))
    {
        app.getMetrics()
            .NewMeter({"op-manage-offer", "invalid", "equal-currencies"},
                      "operation")
            .Mark();
        innerResult().code(MANAGE_OFFER_MALFORMED);
        return false;
    }
    if (mManageOffer.amount < 0 || mManageOffer.price.d <= 0 ||
        mManageOffer.price.n <= 0)
    {
        app.getMetrics()
            .NewMeter({"op-manage-offer", "invalid", "negative-or-zero-values"},
                      "operation")
            .Mark();
        innerResult().code(MANAGE_OFFER_MALFORMED);
        return false;
    }
    if (ledgerVersion > 2 &&
        mManageOffer.offerID == 0 && mManageOffer.amount == 0)
    { // since version 3 of ledger you cannot send
        // offer operation with id and
        // amount both equal to 0
        app.getMetrics()
            .NewMeter({"op-manage-offer", "invalid", "create-with-zero"},
                      "operation")
            .Mark();
        innerResult().code(MANAGE_OFFER_NOT_FOUND);
        return false;
    }

    return true;
}

OfferEntry
ManageOfferOpFrame::buildOffer(AccountID const& account,
                               ManageOfferOp const& op, uint32 flags)
{
    OfferEntry o;
    o.sellerID = account;
    o.amount = op.amount;
    o.price = op.price;
    o.offerID = op.offerID;
    o.selling = op.selling;
    o.buying = op.buying;
    o.flags = flags;
    return o;
}
}
