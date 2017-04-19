// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "transactions/ManageOfferOpFrame.h"
#include "OfferExchange.h"
#include "database/Database.h"
#include "ledger/AccountFrame.h"
#include "ledgerdelta/LedgerDeltaScope.h"
#include "ledgerdelta/LedgerDelta.h"
#include "ledger/LedgerEntries.h"
#include "ledger/LedgerHeaderFrame.h"
#include "ledger/OfferFrame.h"
#include "main/Application.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "util/Logging.h"
#include "util/types.h"

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

// make sure these issuers exist and you can hold the ask asset
bool
ManageOfferOpFrame::checkOfferValid(medida::MetricsRegistry& metrics,
                                    LedgerEntries& entries, LedgerDelta& ledgerDelta)
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
        auto issuer = ledgerDelta.loadAccount(getIssuer(sheep));
        if (!issuer)
        {
            metrics
                .NewMeter({"op-manage-offer", "invalid", "sell-no-issuer"},
                          "operation")
                .Mark();
            innerResult().code(MANAGE_OFFER_SELL_NO_ISSUER);
            return false;
        }
        auto sheepLineA = ledgerDelta.loadTrustLine(getSourceID(), sheep);
        if (!sheepLineA)
        { // we don't have what we are trying to sell
            metrics
                .NewMeter({"op-manage-offer", "invalid", "sell-no-trust"},
                          "operation")
                .Mark();
            innerResult().code(MANAGE_OFFER_SELL_NO_TRUST);
            return false;
        }

        auto sheepTrustA = TrustFrame{*sheepLineA};
        if (sheepTrustA.getBalance() == 0)
        {
            metrics
                .NewMeter({"op-manage-offer", "invalid", "underfunded"},
                          "operation")
                .Mark();
            innerResult().code(MANAGE_OFFER_UNDERFUNDED);
            return false;
        }
        if (!sheepTrustA.isAuthorized())
        {
            metrics
                .NewMeter({"op-manage-offer", "invalid", "sell-not-authorized"},
                          "operation")
                .Mark();
            // we are not authorized to sell
            innerResult().code(MANAGE_OFFER_SELL_NOT_AUTHORIZED);
            return false;
        }
    }

    if (wheat.type() != ASSET_TYPE_NATIVE)
    {
        auto issuer = ledgerDelta.loadAccount(getIssuer(wheat));
        if (!issuer)
        {
            metrics
                .NewMeter({"op-manage-offer", "invalid", "buy-no-issuer"},
                          "operation")
                .Mark();
            innerResult().code(MANAGE_OFFER_BUY_NO_ISSUER);
            return false;
        }
        auto wheatLineA = ledgerDelta.loadTrustLine(getSourceID(), wheat);
        if (!wheatLineA)
        { // we can't hold what we are trying to buy
            metrics
                .NewMeter({"op-manage-offer", "invalid", "buy-no-trust"},
                          "operation")
                .Mark();
            innerResult().code(MANAGE_OFFER_BUY_NO_TRUST);
            return false;
        }

        auto wheatTrustA = TrustFrame{*wheatLineA};
        if (!wheatTrustA.isAuthorized())
        { // we are not authorized to hold what we
            // are trying to buy
            metrics
                .NewMeter({"op-manage-offer", "invalid", "buy-not-authorized"},
                          "operation")
                .Mark();
            innerResult().code(MANAGE_OFFER_BUY_NOT_AUTHORIZED);
            return false;
        }
    }
    return true;
}

// you are selling sheep for wheat
// need to check the counter offers selling wheat for sheep
// see if this is modifying an old offer
// see if this offer crosses any existing offers
bool
ManageOfferOpFrame::doApply(Application& app, LedgerDelta& ledgerDelta,
                            LedgerManager& ledgerManager)
{
    auto& entries = app.getLedgerEntries();
    auto& db = entries.getDatabase();

    if (!checkOfferValid(app.getMetrics(), entries, ledgerDelta))
    {
        return false;
    }

    Asset const& sheep = mManageOffer.selling;
    Asset const& wheat = mManageOffer.buying;

    bool creatingNewOffer = false;
    uint64_t offerID = mManageOffer.offerID;

    auto sourceFrame = AccountFrame{*mSourceAccount};
    auto sellSheepOffer = OfferFrame{};
    if (offerID)
    { // modifying an old offer
        auto offer = ledgerDelta.loadOffer(getSourceID(), offerID);
        if (!offer)
        {
            app.getMetrics()
                .NewMeter({"op-manage-offer", "invalid", "not-found"},
                          "operation")
                .Mark();
            innerResult().code(MANAGE_OFFER_NOT_FOUND);
            return false;
        }

        // rebuild offer based off the manage offer
        sellSheepOffer = OfferFrame{makeOffer(
            getSourceID(), mManageOffer, OfferFrame{*offer}.getFlags())};

        mPassive = sellSheepOffer.isPassive();
    }
    else
    { // creating a new Offer
        creatingNewOffer = true;
        sellSheepOffer = OfferFrame{makeOffer(
            getSourceID(), mManageOffer, mPassive ? PASSIVE_FLAG : 0)};
    }

    int64_t maxSheepSend = sellSheepOffer.getAmount();
    int64_t maxAmountOfSheepCanSell;

    innerResult().code(MANAGE_OFFER_SUCCESS);

    soci::transaction sqlTx(db.getSession());
    LedgerDeltaScope offerDeltaScope{ledgerDelta};

    auto sheepLineA = sheep.type() != ASSET_TYPE_NATIVE
        ? ledgerDelta.loadTrustLine(getSourceID(), sheep)
        : nullptr;
    auto wheatLineA = wheat.type() != ASSET_TYPE_NATIVE
        ? ledgerDelta.loadTrustLine(getSourceID(), wheat)
        : nullptr;

    if (mManageOffer.amount == 0)
    {
        sellSheepOffer.setAmount(0);
    }
    else
    {
        if (sheep.type() == ASSET_TYPE_NATIVE)
        {
            maxAmountOfSheepCanSell = sourceFrame.getBalanceAboveReserve(ledgerManager);
        }
        else
        {
            maxAmountOfSheepCanSell = TrustFrame{*sheepLineA}.getBalance();
        }

        // the maximum is defined by how much wheat it can receive
        int64_t maxWheatCanSell;
        if (wheat.type() == ASSET_TYPE_NATIVE)
        {
            maxWheatCanSell = INT64_MAX;
        }
        else
        {
            maxWheatCanSell = TrustFrame{*wheatLineA}.getMaxAmountReceive();
            if (maxWheatCanSell == 0)
            {
                app.getMetrics()
                    .NewMeter({"op-manage-offer", "invalid", "line-full"},
                              "operation")
                    .Mark();
                innerResult().code(MANAGE_OFFER_LINE_FULL);
                return false;
            }
        }

        auto const& sheepPrice = sellSheepOffer.getPrice();

        {
            int64_t maxSheepBasedOnWheat;
            if (!bigDivide(maxSheepBasedOnWheat, maxWheatCanSell, sheepPrice.d,
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
        // amount
        // put in the offer
        if (maxAmountOfSheepCanSell < maxSheepSend)
        {
            maxSheepSend = maxAmountOfSheepCanSell;
        }

        int64_t sheepSent, wheatReceived;

        OfferExchange oe{app};

        const Price maxWheatPrice(sheepPrice.d, sheepPrice.n);

        OfferExchange::ConvertResult r = oe.convertWithOffers(
            ledgerDelta,
            sheep, maxSheepSend, sheepSent, wheat, maxWheatCanSell,
            wheatReceived, [this, &maxWheatPrice, &sellSheepOffer](LedgerEntry const& o) {
                auto offer = OfferFrame{o};
                if (offer.getOfferID() == sellSheepOffer.getOfferID())
                {
                    // don't let the offer cross itself when updating it
                    return OfferExchange::eSkip;
                }
                if ((mPassive && (offer.getPrice() >= maxWheatPrice)) ||
                    (offer.getPrice() > maxWheatPrice))
                {
                    return OfferExchange::eStop;
                }
                if (offer.getSellerID() == getSourceID())
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
            // it's OK to use mSourceAccount, wheatLineA and sheepLineA
            // here as OfferExchange won't cross offers from source account
            if (wheat.type() == ASSET_TYPE_NATIVE)
            {
                if (!sourceFrame.addBalance(wheatReceived))
                {
                    // this would indicate a bug in OfferExchange
                    throw std::runtime_error("offer claimed over limit");
                }
                ledgerDelta.updateEntry(sourceFrame);
            }
            else
            {
                auto wheatTrust = TrustFrame{*wheatLineA};
                if (!wheatTrust.addBalance(wheatReceived))
                {
                    // this would indicate a bug in OfferExchange
                    throw std::runtime_error("offer claimed over limit");
                }
                ledgerDelta.updateEntry(wheatTrust);
            }

            if (sheep.type() == ASSET_TYPE_NATIVE)
            {
                if (!sourceFrame.addBalance(-sheepSent))
                {
                    // this would indicate a bug in OfferExchange
                    throw std::runtime_error("offer sold more than balance");
                }
                ledgerDelta.updateEntry(sourceFrame);
            }
            else
            {
                auto sheepTrust = TrustFrame{*sheepLineA};
                if (!sheepTrust.addBalance(-sheepSent))
                {
                    // this would indicate a bug in OfferExchange
                    throw std::runtime_error("offer sold more than balance");
                }
                ledgerDelta.updateEntry(sheepTrust);
            }
        }

        // recomputes the amount of sheep for sale
        sellSheepOffer.setAmount(maxSheepSend - sheepSent);
    }

    if (sellSheepOffer.getAmount() > 0)
    { // we still have sheep to sell so leave an offer

        if (creatingNewOffer)
        {
            // make sure we don't allow us to add offers when we don't have
            // the minbalance
            if (!sourceFrame.addNumEntries(1, ledgerManager))
            {
                app.getMetrics()
                    .NewMeter({"op-manage-offer", "invalid", "low reserve"},
                              "operation")
                    .Mark();
                innerResult().code(MANAGE_OFFER_LOW_RESERVE);
                return false;
            }
            sellSheepOffer.setOfferID(ledgerDelta.getHeaderFrame().generateID());
            innerResult().success().offer.effect(MANAGE_OFFER_CREATED);
            ledgerDelta.addEntry(sellSheepOffer);
            ledgerDelta.updateEntry(sourceFrame);
        }
        else
        {
            innerResult().success().offer.effect(MANAGE_OFFER_UPDATED);
            ledgerDelta.updateEntry(sellSheepOffer);
        }
        innerResult().success().offer.offer() = sellSheepOffer.getOffer();
    }
    else
    {
        innerResult().success().offer.effect(MANAGE_OFFER_DELETED);

        if (!creatingNewOffer)
        {
            ledgerDelta.deleteEntry(sellSheepOffer.getKey());
            sourceFrame.addNumEntries(-1, ledgerManager);
            ledgerDelta.updateEntry(sourceFrame);
        }
    }

    sqlTx.commit();
    offerDeltaScope.commit();

    app.getMetrics()
        .NewMeter({"op-create-offer", "success", "apply"}, "operation")
        .Mark();
    return true;
}

// makes sure the currencies are different
bool
ManageOfferOpFrame::doCheckValid(Application& app)
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
    if (app.getLedgerManager().getCurrentLedgerVersion() > 2 &&
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
ManageOfferOpFrame::makeOffer(AccountID const& account,
                              ManageOfferOp const& op, uint32 flags)
{
    auto o = OfferEntry{};
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
