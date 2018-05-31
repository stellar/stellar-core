// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "transactions/ManageOfferOpFrame.h"
#include "OfferExchange.h"
#include "database/Database.h"
#include "ledger/LedgerDelta.h"
#include "ledger/OfferFrame.h"
#include "main/Application.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "util/Logging.h"
#include "util/XDROperators.h"
#include "util/types.h"

// convert from sheep to wheat
// selling sheep
// buying wheat

namespace stellar
{

using namespace std;

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
                                    Database& db, LedgerDelta& delta)
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
        auto tlI =
            TrustFrame::loadTrustLineIssuer(getSourceID(), sheep, db, delta);
        mSheepLineA = tlI.first;
        if (!tlI.second)
        {
            metrics
                .NewMeter({"op-manage-offer", "invalid", "sell-no-issuer"},
                          "operation")
                .Mark();
            innerResult().code(MANAGE_OFFER_SELL_NO_ISSUER);
            return false;
        }
        if (!mSheepLineA)
        { // we don't have what we are trying to sell
            metrics
                .NewMeter({"op-manage-offer", "invalid", "sell-no-trust"},
                          "operation")
                .Mark();
            innerResult().code(MANAGE_OFFER_SELL_NO_TRUST);
            return false;
        }
        if (mSheepLineA->getBalance() == 0)
        {
            metrics
                .NewMeter({"op-manage-offer", "invalid", "underfunded"},
                          "operation")
                .Mark();
            innerResult().code(MANAGE_OFFER_UNDERFUNDED);
            return false;
        }
        if (!mSheepLineA->isAuthorized())
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
        auto tlI =
            TrustFrame::loadTrustLineIssuer(getSourceID(), wheat, db, delta);
        mWheatLineA = tlI.first;
        if (!tlI.second)
        {
            metrics
                .NewMeter({"op-manage-offer", "invalid", "buy-no-issuer"},
                          "operation")
                .Mark();
            innerResult().code(MANAGE_OFFER_BUY_NO_ISSUER);
            return false;
        }
        if (!mWheatLineA)
        { // we can't hold what we are trying to buy
            metrics
                .NewMeter({"op-manage-offer", "invalid", "buy-no-trust"},
                          "operation")
                .Mark();
            innerResult().code(MANAGE_OFFER_BUY_NO_TRUST);
            return false;
        }
        if (!mWheatLineA->isAuthorized())
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
ManageOfferOpFrame::doApply(Application& app, LedgerDelta& delta,
                            LedgerManager& ledgerManager)
{
    Database& db = ledgerManager.getDatabase();

    if (!checkOfferValid(app.getMetrics(), db, delta))
    {
        return false;
    }

    Asset const& sheep = mManageOffer.selling;
    Asset const& wheat = mManageOffer.buying;

    bool creatingNewOffer = false;
    uint64_t offerID = mManageOffer.offerID;

    soci::transaction sqlTx(db.getSession());
    LedgerDelta tempDelta(delta);

    if (offerID)
    { // modifying an old offer
        mSellSheepOffer =
            OfferFrame::loadOffer(getSourceID(), offerID, db, &tempDelta);

        if (!mSellSheepOffer)
        {
            app.getMetrics()
                .NewMeter({"op-manage-offer", "invalid", "not-found"},
                          "operation")
                .Mark();
            innerResult().code(MANAGE_OFFER_NOT_FOUND);
            return false;
        }

        // WARNING: mSellSheepOffer is deleted but mSourceAccount is not updated
        // to reflect the change in numSubEntries at this point. However, we
        // can't delete it here since doing so would modify mSourceAccount,
        // which would lead to different buckets being generated.
        mSellSheepOffer->storeDelete(tempDelta, db);

        // rebuild offer based off the manage offer
        mSellSheepOffer->getOffer() = buildOffer(
            getSourceID(), mManageOffer, mSellSheepOffer->getOffer().flags);
        mPassive = mSellSheepOffer->getFlags() & PASSIVE_FLAG;
    }
    else
    { // creating a new Offer
        creatingNewOffer = true;
        LedgerEntry le;
        le.data.type(OFFER);
        le.data.offer() = buildOffer(getSourceID(), mManageOffer,
                                     mPassive ? PASSIVE_FLAG : 0);
        mSellSheepOffer = std::make_shared<OfferFrame>(le);
    }

    innerResult().code(MANAGE_OFFER_SUCCESS);

    bool adjusted = false;

    if (mManageOffer.amount == 0)
    {
        // deleting the offer
        mSellSheepOffer->getOffer().amount = 0;
    }
    else
    {
        if (sheep.type() == ASSET_TYPE_NATIVE)
        {
            if (creatingNewOffer &&
                app.getLedgerManager().getCurrentLedgerVersion() > 8)
            {
                // we need to compute maxAmountOfSheepCanSell based on the
                // updated reserve to avoid selling too many and falling
                // below the reserve when we try to create the offer later on
                if (!mSourceAccount->addNumEntries(1, ledgerManager))
                {
                    app.getMetrics()
                        .NewMeter({"op-manage-offer", "invalid", "low reserve"},
                                  "operation")
                        .Mark();
                    innerResult().code(MANAGE_OFFER_LOW_RESERVE);
                    return false;
                }
                adjusted = true;
            }
        }

        Price const& sheepPrice = mSellSheepOffer->getPrice();
        const Price maxWheatPrice(sheepPrice.d, sheepPrice.n);

        int64_t maxWheatReceive = canBuyAtMost(wheat, mWheatLineA);
        int64_t maxSheepSend;
        if (app.getLedgerManager().getCurrentLedgerVersion() >= 10)
        {
            maxSheepSend = canSellAtMost(mSourceAccount, sheep, mSheepLineA,
                                         ledgerManager);
        }
        else
        {
            int64_t maxSheepCanSell = canSellAtMost(mSourceAccount, sheep,
                                                    mSheepLineA, ledgerManager);
            int64_t maxSheepBasedOnWheat;
            if (!bigDivide(maxSheepBasedOnWheat, maxWheatReceive, sheepPrice.d,
                           sheepPrice.n, ROUND_DOWN))
            {
                maxSheepBasedOnWheat = INT64_MAX;
            }

            maxSheepSend = std::min({maxSheepCanSell, maxSheepBasedOnWheat});
        }
        // amount of sheep for sale is the lesser of amount we can sell and
        // amount put in the offer
        maxSheepSend = std::min(mSellSheepOffer->getAmount(), maxSheepSend);

        if (adjusted)
        {
            // restore the number back (will be re-incremented later if
            // the offer really needs to be created)
            mSourceAccount->addNumEntries(-1, ledgerManager);
        }

        if (maxWheatReceive == 0)
        {
            app.getMetrics()
                .NewMeter({"op-manage-offer", "invalid", "line-full"},
                          "operation")
                .Mark();
            innerResult().code(MANAGE_OFFER_LINE_FULL);
            return false;
        }

        int64_t sheepSent, wheatReceived;
        OfferExchange oe(tempDelta, ledgerManager);
        OfferExchange::ConvertResult r = oe.convertWithOffers(
            sheep, maxSheepSend, sheepSent, wheat, maxWheatReceive,
            wheatReceived, false, [this, &maxWheatPrice](OfferFrame const& o) {
                assert(o.getOfferID() != mSellSheepOffer->getOfferID());
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

        bool sheepStays;
        switch (r)
        {
        case OfferExchange::eOK:
            sheepStays = false;
            break;
        case OfferExchange::ePartial:
            sheepStays = true;
            break;
        case OfferExchange::eFilterStop:
            if (innerResult().code() != MANAGE_OFFER_SUCCESS)
            {
                return false;
            }
            sheepStays = true;
            break;
        default:
            abort();
        }

        // updates the result with the offers that got taken on the way

        for (auto const& oatom : oe.getOfferTrail())
        {
            innerResult().success().offersClaimed.push_back(oatom);
        }

        if (wheatReceived > 0)
        {
            // it's OK to use mSourceAccount, mWheatLineA and mSheepLineA
            // here as OfferExchange won't cross offers from source account
            if (wheat.type() == ASSET_TYPE_NATIVE)
            {
                if (!mSourceAccount->addBalance(wheatReceived))
                {
                    // this would indicate a bug in OfferExchange
                    throw std::runtime_error("offer claimed over limit");
                }

                mSourceAccount->storeChange(delta, db);
            }
            else
            {
                if (!mWheatLineA->addBalance(wheatReceived))
                {
                    // this would indicate a bug in OfferExchange
                    throw std::runtime_error("offer claimed over limit");
                }

                mWheatLineA->storeChange(delta, db);
            }

            if (sheep.type() == ASSET_TYPE_NATIVE)
            {
                if (!mSourceAccount->addBalance(-sheepSent))
                {
                    // this would indicate a bug in OfferExchange
                    throw std::runtime_error("offer sold more than balance");
                }
                mSourceAccount->storeChange(delta, db);
            }
            else
            {
                if (!mSheepLineA->addBalance(-sheepSent))
                {
                    // this would indicate a bug in OfferExchange
                    throw std::runtime_error("offer sold more than balance");
                }
                mSheepLineA->storeChange(delta, db);
            }
        }

        mSellSheepOffer->getOffer().amount = maxSheepSend - sheepSent;
        if (ledgerManager.getCurrentLedgerVersion() >= 10)
        {
            if (sheepStays)
            {
                adjustOffer(*mSellSheepOffer, ledgerManager, mSourceAccount,
                            sheep, mSheepLineA, wheat, mWheatLineA);
            }
            else
            {
                mSellSheepOffer->getOffer().amount = 0;
            }
        }
    }

    if (mSellSheepOffer->getOffer().amount > 0)
    { // we still have sheep to sell so leave an offer

        if (creatingNewOffer)
        {
            // make sure we don't allow us to add offers when we don't have
            // the minbalance (should never happen at this stage in v9+)
            if (!mSourceAccount->addNumEntries(1, ledgerManager))
            {
                app.getMetrics()
                    .NewMeter({"op-manage-offer", "invalid", "low reserve"},
                              "operation")
                    .Mark();
                innerResult().code(MANAGE_OFFER_LOW_RESERVE);
                return false;
            }
            mSellSheepOffer->mEntry.data.offer().offerID =
                tempDelta.getHeaderFrame().generateID();
            innerResult().success().offer.effect(MANAGE_OFFER_CREATED);
            mSourceAccount->storeChange(tempDelta, db);
        }
        else
        {
            innerResult().success().offer.effect(MANAGE_OFFER_UPDATED);
        }
        mSellSheepOffer->storeAdd(tempDelta, db);
        innerResult().success().offer.offer() = mSellSheepOffer->getOffer();
    }
    else
    {
        innerResult().success().offer.effect(MANAGE_OFFER_DELETED);

        if (!creatingNewOffer)
        {
            mSourceAccount->addNumEntries(-1, ledgerManager);
            mSourceAccount->storeChange(tempDelta, db);
        }
    }

    sqlTx.commit();
    tempDelta.commit();

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
