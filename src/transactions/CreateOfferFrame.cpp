#include "transactions/CreateOfferFrame.h"
#include "ledger/OfferFrame.h"
#include "util/Logging.h"
#include "util/types.h"
#include "database/Database.h"
#include "ledger/LedgerDelta.h"
#include "OfferExchange.h"

// This is pretty gnarly still. I'll clean it up as I write the tests
//
// convert from sheep to wheat
// selling sheep
// buying wheat

namespace stellar
{

using namespace std;

CreateOfferFrame::CreateOfferFrame(const TransactionEnvelope& envelope) : TransactionFrame(envelope)
{

}

// make sure these issuers exist and you can hold the ask currency
bool CreateOfferFrame::checkOfferValid(Database &db)
{
    Currency& sheep = mEnvelope.tx.body.createOfferTx().takerGets;
    Currency& wheat = mEnvelope.tx.body.createOfferTx().takerPays;

    if (sheep.type() != NATIVE)
    {
        if (!TrustFrame::loadTrustLine(mEnvelope.tx.account, sheep, mSheepLineA, db))
        {   // we don't have what we are trying to sell
            innerResult().code(CreateOffer::NO_TRUST);
            return false;
        }
        if (mSheepLineA.getBalance() == 0)
        {
            innerResult().code(CreateOffer::UNDERFUNDED);
            return false;
        }
    }

    if(wheat.type()!=NATIVE)
    {
        if(!TrustFrame::loadTrustLine(mEnvelope.tx.account, wheat, mWheatLineA, db))
        {   // we can't hold what we are trying to buy
            innerResult().code(CreateOffer::NO_TRUST);
            return false;
        }

        if(!mWheatLineA.getTrustLine().authorized)
        {   // we are not authorized to hold what we are trying to buy
            innerResult().code(CreateOffer::NOT_AUTHORIZED);
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
bool CreateOfferFrame::doApply(LedgerDelta& delta, LedgerMaster& ledgerMaster)
{
    Database &db = ledgerMaster.getDatabase();

    if (!checkOfferValid(db))
    {
        return false;
    }
    Currency& sheep = mEnvelope.tx.body.createOfferTx().takerGets;
    Currency& wheat = mEnvelope.tx.body.createOfferTx().takerPays;

    bool creatingNewOffer = false;
    uint32_t offerSeq = mEnvelope.tx.body.createOfferTx().sequence;

    // TODO: why using account seq number instead of a different sequence number?
    // plus 1 since the account seq has already been incremented at this point
    if(offerSeq + 1 == mSigningAccount->getAccount().sequence)
    { // creating a new Offer
        creatingNewOffer = true;
        mSellSheepOffer.from(mEnvelope.tx);
    } else
    { // modifying an old offer
        
        if(OfferFrame::loadOffer(mEnvelope.tx.account, offerSeq, mSellSheepOffer, db))
        {
            // make sure the currencies are the same
            if(!compareCurrency(mEnvelope.tx.body.createOfferTx().takerGets, mSellSheepOffer.getOffer().takerGets) ||
                !compareCurrency(mEnvelope.tx.body.createOfferTx().takerPays, mSellSheepOffer.getOffer().takerPays))
            {
                innerResult().code(CreateOffer::MALFORMED);
                return false;
            }
        } else
        {
            innerResult().code(CreateOffer::NOT_FOUND);
            return false;
        }
    }

    int64_t maxSheepSend = mEnvelope.tx.body.createOfferTx().amount;

    int64_t maxAmountOfSheepCanSell;
    if (sheep.type() == NATIVE)
    {
        maxAmountOfSheepCanSell = mSigningAccount->getAccount().balance -
            ledgerMaster.getMinBalance(mSigningAccount->getAccount().ownerCount);
    }
    else
    {
        maxAmountOfSheepCanSell = mSheepLineA.getTrustLine().balance;
    }

    // amount of sheep for sale is the lesser of amount we can sell and amount put in the offer
    //int64_t amountOfSheepForSale = amountOfWheat*OFFER_PRICE_DIVISOR / sheepPrice;
    if (maxAmountOfSheepCanSell < maxSheepSend)
    {
        maxSheepSend = maxAmountOfSheepCanSell;
    }

    Price sheepPrice = mEnvelope.tx.body.createOfferTx().price;

    innerResult().code(CreateOffer::SUCCESS);

    {
        soci::transaction sqlTx(db.getSession());
        LedgerDelta tempDelta;

        int64_t sheepSent, wheatReceived;

        OfferExchange oe(tempDelta, ledgerMaster);

        Price maxWheatPrice(sheepPrice.d, sheepPrice.n);

        OfferExchange::ConvertResult r = oe.convertWithOffers(
            sheep, maxSheepSend, sheepSent,
            wheat, INT64_MAX, wheatReceived,
            [this, maxWheatPrice](OfferFrame const& o) {
                if (o.getPrice() > maxWheatPrice)
                {
                    return OfferExchange::eStop;
                }
                if (o.getAccountID() == mSigningAccount->getID())
                {
                    // we are crossing our own offer
                    innerResult().code(CreateOffer::CROSS_SELF);
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
            if (innerResult().code() != CreateOffer::SUCCESS)
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
                mSigningAccount->getAccount().balance += wheatReceived;
                mSigningAccount->storeChange(delta, db);
            }
            else
            {
                TrustFrame wheatLineSigningAccount;
                if (!TrustFrame::loadTrustLine(mSigningAccount->getID(),
                    wheat, wheatLineSigningAccount, db))
                {
                    throw std::runtime_error("invalid database state: must have matching trust line");
                }
                wheatLineSigningAccount.getTrustLine().balance += wheatReceived;
                wheatLineSigningAccount.storeChange(delta, db);
            }

            if (sheep.type() == NATIVE)
            {
                mSigningAccount->getAccount().balance -= sheepSent;
                mSigningAccount->storeChange(delta, db);
            }
            else
            {
                mSheepLineA.getTrustLine().balance -= sheepSent;
                mSheepLineA.storeChange(delta, db);
            }
        }

        // recomputes the amount of sheep for sale
        mSellSheepOffer.getOffer().amount = maxSheepSend - sheepSent;

        if (offerIsValid && mSellSheepOffer.getOffer().amount > 0)
        { // we still have sheep to sell so leave an offer

            if (creatingNewOffer)
            {
                // make sure we don't allow us to add offers when we don't have the minbalance
                if (mSigningAccount->getAccount().balance <
                    ledgerMaster.getMinBalance(mSigningAccount->getAccount().ownerCount + 1))
                {
                    innerResult().code(CreateOffer::UNDERFUNDED);
                    return false;
                }

                innerResult().success().offer.effect(CreateOffer::CREATED);
                innerResult().success().offer.offerCreated() = mSellSheepOffer.getOffer();
                mSellSheepOffer.storeAdd(tempDelta, db);

                mSigningAccount->getAccount().ownerCount++;
                mSigningAccount->storeChange(tempDelta, db);
            }
            else
            {
                innerResult().success().offer.effect(CreateOffer::UPDATED);
                mSellSheepOffer.storeChange(tempDelta, db);
            }
        }
        else
        {
            innerResult().success().offer.effect(CreateOffer::EMPTY);

            if (!creatingNewOffer)
            {
                mSellSheepOffer.storeDelete(tempDelta, db);
            }
        }

        sqlTx.commit();
        delta.merge(tempDelta);
    }
    return true;
}

// makes sure the currencies are different 
bool CreateOfferFrame::doCheckValid(Application& app)
{
    Currency& sheep = mEnvelope.tx.body.createOfferTx().takerGets;
    Currency& wheat = mEnvelope.tx.body.createOfferTx().takerPays;
    if (compareCurrency(sheep, wheat))
    {
        return false;
    }
    
    return true;
}


}
