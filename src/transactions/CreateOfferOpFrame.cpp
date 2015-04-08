// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/CreateOfferOpFrame.h"
#include "ledger/OfferFrame.h"
#include "util/Logging.h"
#include "util/types.h"
#include "database/Database.h"
#include "ledger/LedgerDelta.h"
#include "OfferExchange.h"

// convert from sheep to wheat
// selling sheep
// buying wheat

namespace stellar
{

using namespace std;

CreateOfferOpFrame::CreateOfferOpFrame(Operation const& op,
                                       OperationResult& res,
                                       TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mCreateOffer(mOperation.body.createOfferOp())
{
}

// make sure these issuers exist and you can hold the ask currency
bool
CreateOfferOpFrame::checkOfferValid(Database& db)
{
    Currency const& sheep = mCreateOffer.takerGets;
    Currency const& wheat = mCreateOffer.takerPays;

    if (sheep.type() != NATIVE)
    {
        if (!TrustFrame::loadTrustLine(getSourceID(), sheep, mSheepLineA, db))
        { // we don't have what we are trying to sell
            innerResult().code(CREATE_OFFER_UNDERFUNDED);
            return false;
        }
        if (mSheepLineA.getBalance() == 0)
        {
            innerResult().code(CREATE_OFFER_UNDERFUNDED);
            return false;
        }
    }

    if (wheat.type() != NATIVE)
    {
        if (!TrustFrame::loadTrustLine(getSourceID(), wheat, mWheatLineA, db))
        { // we can't hold what we are trying to buy
            innerResult().code(CREATE_OFFER_NO_TRUST);
            return false;
        }

        if (!mWheatLineA.getTrustLine().authorized)
        { // we are not authorized to hold what we are trying to buy
            innerResult().code(CREATE_OFFER_NOT_AUTHORIZED);
            return false;
        }
    }
    return true;
}

// you are selling sheep for wheat
// need to check the counter offers selling wheat for sheep
// see if this is modifying an old offer
// see if this offer crosses any existing offers

// TODO: revisit this, offer code should share logic with payment code
//      to keep the code working, I ended up duplicating the error codes
bool
CreateOfferOpFrame::doApply(LedgerDelta& delta, LedgerManager& ledgerManager)
{
    Database& db = ledgerManager.getDatabase();

    if (!checkOfferValid(db))
    {
        return false;
    }

    Currency const& sheep = mCreateOffer.takerGets;
    Currency const& wheat = mCreateOffer.takerPays;

    bool creatingNewOffer = false;
    uint64_t offerID = mCreateOffer.offerID;

    if (offerID)
    { // modifying an old offer
        if (OfferFrame::loadOffer(getSourceID(), offerID, mSellSheepOffer, db))
        {
            // make sure the currencies are the same
            if (!compareCurrency(mCreateOffer.takerGets,
                                 mSellSheepOffer.getOffer().takerGets) ||
                !compareCurrency(mCreateOffer.takerPays,
                                 mSellSheepOffer.getOffer().takerPays))
            {
                innerResult().code(CREATE_OFFER_MISMATCH);
                return false;
            }
        }
        else
        {
            innerResult().code(CREATE_OFFER_NOT_FOUND);
            return false;
        }
    }
    else
    { // creating a new Offer
        creatingNewOffer = true;
        mSellSheepOffer.from(*this);
    }

    int64_t maxSheepSend = mCreateOffer.amount;

    int64_t maxAmountOfSheepCanSell;
    if (sheep.type() == NATIVE)
    {
        maxAmountOfSheepCanSell =
            mSourceAccount->getBalanceAboveReserve(ledgerManager);
    }
    else
    {
        maxAmountOfSheepCanSell = mSheepLineA.getBalance();
    }

    // the maximum is defined by how much wheat it can receive
    int64_t maxWheatCanSell;
    if (wheat.type() == NATIVE)
    {
        maxWheatCanSell = INT64_MAX;
    }
    else
    {
        maxWheatCanSell = mWheatLineA.getMaxAmountReceive();
        if (maxWheatCanSell == 0)
        {
            innerResult().code(CREATE_OFFER_LINE_FULL);
            return false;
        }
    }

    {
        int64_t maxSheepBasedOnWheat;
        if (!bigDivide(maxSheepBasedOnWheat, maxWheatCanSell,
                       mCreateOffer.price.d, mCreateOffer.price.n))
        {
            maxSheepBasedOnWheat = INT64_MAX;
        }

        if (maxAmountOfSheepCanSell > maxSheepBasedOnWheat)
        {
            maxAmountOfSheepCanSell = maxSheepBasedOnWheat;
        }
    }

    // amount of sheep for sale is the lesser of amount we can sell and amount
    // put in the offer
    if (maxAmountOfSheepCanSell < maxSheepSend)
    {
        maxSheepSend = maxAmountOfSheepCanSell;
    }

    Price sheepPrice = mCreateOffer.price;

    innerResult().code(CREATE_OFFER_SUCCESS);

    {
        soci::transaction sqlTx(db.getSession());
        LedgerDelta tempDelta(delta);

        int64_t sheepSent, wheatReceived;

        OfferExchange oe(tempDelta, ledgerManager);

        Price maxWheatPrice(sheepPrice.d, sheepPrice.n);

        OfferExchange::ConvertResult r = oe.convertWithOffers(
            sheep, maxSheepSend, sheepSent, wheat, maxWheatCanSell,
            wheatReceived, [this, maxWheatPrice](OfferFrame const& o)
            {
                if (o.getPrice() > maxWheatPrice)
                {
                    return OfferExchange::eStop;
                }
                if (o.getAccountID() == getSourceID())
                {
                    // we are crossing our own offer
                    innerResult().code(CREATE_OFFER_CROSS_SELF);
                    return OfferExchange::eStop;
                }
                return OfferExchange::eKeep;
            });

        bool offerIsValid = false;

        switch (r)
        {
        case OfferExchange::eOK:
        case OfferExchange::ePartial:
            offerIsValid = true;
            break;
        case OfferExchange::eFilterStop:
            if (innerResult().code() != CREATE_OFFER_SUCCESS)
            {
                return false;
            }
            offerIsValid = true;
            break;
        }

        // updates the result with the offers that got taken on the way

        for (auto oatom : oe.getOfferTrail())
        {
            innerResult().success().offersClaimed.push_back(oatom);
        }

        if (wheatReceived > 0)
        {
            if (wheat.type() == NATIVE)
            {
                mSourceAccount->getAccount().balance += wheatReceived;
                mSourceAccount->storeChange(delta, db);
            }
            else
            {
                TrustFrame wheatLineSigningAccount;
                if (!TrustFrame::loadTrustLine(getSourceID(), wheat,
                                               wheatLineSigningAccount, db))
                {
                    throw std::runtime_error("invalid database state: must "
                                             "have matching trust line");
                }
                if (!wheatLineSigningAccount.addBalance(wheatReceived))
                {
                    // this would indicate a bug in OfferExchange
                    throw std::runtime_error("offer claimed over limit");
                }

                wheatLineSigningAccount.storeChange(delta, db);
            }

            if (sheep.type() == NATIVE)
            {
                mSourceAccount->getAccount().balance -= sheepSent;
                mSourceAccount->storeChange(delta, db);
            }
            else
            {
                if (!mSheepLineA.addBalance(-sheepSent))
                {
                    // this would indicate a bug in OfferExchange
                    throw std::runtime_error("offer sold more than balance");
                }
                mSheepLineA.storeChange(delta, db);
            }
        }

        // recomputes the amount of sheep for sale
        mSellSheepOffer.getOffer().amount = maxSheepSend - sheepSent;

        if (offerIsValid && mSellSheepOffer.getOffer().amount > 0)
        { // we still have sheep to sell so leave an offer

            if (creatingNewOffer)
            {
                // make sure we don't allow us to add offers when we don't have
                // the minbalance
                if (!mSourceAccount->addNumEntries(1, ledgerManager))
                {
                    innerResult().code(CREATE_OFFER_LOW_RESERVE);
                    return false;
                }
                mSellSheepOffer.mEntry.offer().offerID =
                    tempDelta.getHeaderFrame().generateID();
                innerResult().success().offer.effect(CREATE_OFFER_CREATED);
                mSellSheepOffer.storeAdd(tempDelta, db);
                mSourceAccount->storeChange(tempDelta, db);
            }
            else
            {
                innerResult().success().offer.effect(CREATE_OFFER_UPDATED);
                mSellSheepOffer.storeChange(tempDelta, db);
            }
            innerResult().success().offer.offer() = mSellSheepOffer.getOffer();
        }
        else
        {
            innerResult().success().offer.effect(CREATE_OFFER_DELETED);

            if (!creatingNewOffer)
            {
                mSellSheepOffer.storeDelete(tempDelta, db);
            }
        }

        sqlTx.commit();
        tempDelta.commit();
    }
    return true;
}

// makes sure the currencies are different
bool
CreateOfferOpFrame::doCheckValid(Application& app)
{
    Currency const& sheep = mCreateOffer.takerGets;
    Currency const& wheat = mCreateOffer.takerPays;
    if (compareCurrency(sheep, wheat))
    {
        innerResult().code(CREATE_OFFER_MALFORMED);
        return false;
    }
    if (mCreateOffer.amount < 0 || mCreateOffer.price.d <= 0 ||
        mCreateOffer.price.n <= 0)
    {
        innerResult().code(CREATE_OFFER_MALFORMED);
        return false;
    }

    return true;
}
}
