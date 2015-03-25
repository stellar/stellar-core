// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

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

    if (mCreateOffer.amount == 0)
    {
        // canceling a previous offer
        return true;
    }

    if (sheep.type() != NATIVE)
    {
        if (!TrustFrame::loadTrustLine(getSourceID(), sheep, mSheepLineA, db))
        { // we don't have what we are trying to sell
            innerResult().code(CREATE_OFFER_NO_TRUST);
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
                innerResult().code(CREATE_OFFER_MALFORMED);
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


    if (mCreateOffer.amount == 0)
    {
        // Setting `amount` to zero signals we want to cancel an existing offer
        if (creatingNewOffer)
        {
            innerResult().code(CREATE_OFFER_MALFORMED);
            return false;
        }
        return doApplyCancelHelper(delta, ledgerManager);
    }

    int64_t maxSheepSend = mCreateOffer.amount;

    int64_t maxAmountOfSheepCanSell;
    if (sheep.type() == NATIVE)
    {
        maxAmountOfSheepCanSell = mSourceAccount->getBalanceAboveReserve(ledgerManager);
    }
    else
    {
        maxAmountOfSheepCanSell = mSheepLineA.getBalance();
    }

    // amount of sheep for sale is the lesser of amount we can sell and amount
    // put in the offer
    // int64_t amountOfSheepForSale = amountOfWheat*OFFER_PRICE_DIVISOR /
    // sheepPrice;
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
            sheep, maxSheepSend, sheepSent, wheat, INT64_MAX, wheatReceived,
            [this, maxWheatPrice](OfferFrame const& o)
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
            offerIsValid = true;
            break;
        case OfferExchange::ePartial:
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
                if(!wheatLineSigningAccount.addBalance(wheatReceived))
                {
                    innerResult().code(CREATE_OFFER_UNDERFUNDED);
                    return false;
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
                if(!mSheepLineA.addBalance(-sheepSent))
                {
                    return false;
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
                    innerResult().code(CREATE_OFFER_BELOW_MIN_BALANCE);
                    return false;
                }
                mSellSheepOffer.mEntry.offer().offerID =
                    tempDelta.getHeaderFrame().generateID();
                innerResult().success().offer.effect(CREATE_OFFER_CREATED);
                innerResult().success().offer.offerCreated() =
                    mSellSheepOffer.getOffer();
                mSellSheepOffer.storeAdd(tempDelta, db);

                mSourceAccount->storeChange(tempDelta, db);
            }
            else
            {
                innerResult().success().offer.effect(CREATE_OFFER_UPDATED);
                mSellSheepOffer.storeChange(tempDelta, db);
            }
        }
        else
        {
            innerResult().success().offer.effect(CREATE_OFFER_EMPTY);

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

bool
CreateOfferOpFrame::doApplyCancelHelper(LedgerDelta& delta, LedgerManager& ledgerManager) 
{
    innerResult().code(CREATE_OFFER_SUCCESS);

    mSourceAccount->getAccount().numSubEntries--;

    Database& db = ledgerManager.getDatabase();
    mSellSheepOffer.storeDelete(delta, db);
    mSourceAccount->storeChange(delta, db);
    innerResult().success().offer.effect(CREATE_OFFER_CANCELLED);

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

    return true;
}
}
