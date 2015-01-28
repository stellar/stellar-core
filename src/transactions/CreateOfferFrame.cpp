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
bool CreateOfferFrame::checkOfferValid(LedgerMaster& ledgerMaster)
{
    Currency& sheep = mEnvelope.tx.body.createOfferTx().takerGets;
    Currency& wheat = mEnvelope.tx.body.createOfferTx().takerPays;

    

    if (sheep.type() != NATIVE)
    {
        if (!ledgerMaster.getDatabase().loadTrustLine(mEnvelope.tx.account, sheep, mSheepLineA))
        {   // we don't have what we are trying to sell
            innerResult().result.code(CreateOffer::NO_TRUST);
            return false;
        }
        if (mSheepLineA.getBalance() == 0)
        {
            innerResult().result.code(CreateOffer::UNDERFUNDED);
            return false;
        }
    }

    if(wheat.type()!=NATIVE)
    {
        if(!ledgerMaster.getDatabase().loadTrustLine(mEnvelope.tx.account, wheat, mWheatLineA))
        {   // we can't hold what we are trying to buy
            innerResult().result.code(CreateOffer::NO_TRUST);
            return false;
        }

        if(!mWheatLineA.mEntry.trustLine().authorized)
        {   // we are not authorized to hold what we are trying to buy
            innerResult().result.code(CreateOffer::NOT_AUTHORIZED);
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
    if (!checkOfferValid(ledgerMaster))
    {
        return false;
    }
    Currency& sheep = mEnvelope.tx.body.createOfferTx().takerGets;
    Currency& wheat = mEnvelope.tx.body.createOfferTx().takerPays;

    bool creatingNewOffer = false;
    uint32_t offerSeq = mEnvelope.tx.body.createOfferTx().sequence;

    // TODO: why using account seq number instead of a different sequence number?
    // plus 1 since the account seq has already been incremented at this point
    if(offerSeq + 1 == mSigningAccount->mEntry.account().sequence)
    { // creating a new Offer
        creatingNewOffer = true;
        mSellSheepOffer.from(mEnvelope.tx);
    } else
    { // modifying an old offer
        
        if(ledgerMaster.getDatabase().loadOffer(mEnvelope.tx.account, offerSeq, mSellSheepOffer))
        {
            // make sure the currencies are the same
            if(!compareCurrency(mEnvelope.tx.body.createOfferTx().takerGets, mSellSheepOffer.mEntry.offer().takerGets) ||
                !compareCurrency(mEnvelope.tx.body.createOfferTx().takerPays, mSellSheepOffer.mEntry.offer().takerPays))
            {
                innerResult().result.code(CreateOffer::MALFORMED);
                return false;
            }
        } else
        {
            innerResult().result.code(CreateOffer::NOT_FOUND);
            return false;
        }
    }

    mWheatTransferRate = getTransferRate(wheat, ledgerMaster);
    mSheepTransferRate = getTransferRate(sheep, ledgerMaster);


    int64_t maxSheepReceived = mEnvelope.tx.body.createOfferTx().amount;

    int64_t maxAmountOfSheepCanSell;
    if (sheep.type() == NATIVE)
    {
        maxAmountOfSheepCanSell = mSigningAccount->mEntry.account().balance -
            ledgerMaster.getMinBalance(mSigningAccount->mEntry.account().ownerCount);
    }
    else
    {
        maxAmountOfSheepCanSell = mSheepLineA.mEntry.trustLine().balance;
    }

    // compute the amount that can be received with what we own
    if (mSheepTransferRate != TRANSFER_RATE_DIVISOR)
    {
        maxAmountOfSheepCanSell = bigDivide(maxAmountOfSheepCanSell, mSheepTransferRate, TRANSFER_RATE_DIVISOR);
    }

    // amount of sheep for sale is the lesser of amount we can sell and amount put in the offer
    //int64_t amountOfSheepForSale = amountOfWheat*OFFER_PRICE_DIVISOR / sheepPrice;
    if (maxAmountOfSheepCanSell < maxSheepReceived)
    {
        maxSheepReceived = maxAmountOfSheepCanSell;
    }

    int64_t sheepPrice = mEnvelope.tx.body.createOfferTx().price;
    
    {
        soci::transaction sqlTx(ledgerMaster.getDatabase().getSession());
        LedgerDelta tempDelta;

        int64_t sheepSent, sheepReceived, wheatReceived;

        OfferExchange oe(tempDelta, ledgerMaster);

        int64_t maxWheatPrice = bigDivide(OFFER_PRICE_DIVISOR, OFFER_PRICE_DIVISOR, sheepPrice);

        OfferExchange::ConvertResult r = oe.convertWithOffers(
            sheep, maxSheepReceived, sheepReceived, sheepSent,
            wheat, INT64_MAX, wheatReceived,
            [this, maxWheatPrice](OfferFrame const& o) {
                if (o.getPrice() > maxWheatPrice)
                {
                    return OfferExchange::eStop;
                }
                // TODO: we should either just skip
                // or not allow at all competing offers from the same account
                // check below does not garantee that those offers won't become
                // the best offers later
                if (o.getAccountID() == mSigningAccount->getID())
                {
                    // we are crossing our own offer
                    // TODO: revisit
                    //mResultCode = txCROSS_SELF;
                    innerResult().result.code(CreateOffer::MALFORMED);
                    return OfferExchange::eFail;
                }
                return OfferExchange::eKeep;
            });
        switch (r)
        {
        case OfferExchange::eOK:
        case OfferExchange::eFilterStop:
        case OfferExchange::eNotEnoughOffers:
            break;
        case OfferExchange::eFilterFail:
            return false;
        case OfferExchange::eBadOffer:
            throw std::runtime_error("Could not process offer");
        }

        if (wheatReceived > 0)
        {
            if (wheat.type() == NATIVE)
            {
                mSigningAccount->mEntry.account().balance += wheatReceived;
                mSigningAccount->storeChange(delta, ledgerMaster);
            }
            else
            {
                TrustFrame wheatLineSigningAccount;
                if (!ledgerMaster.getDatabase().loadTrustLine(mSigningAccount->getID(),
                    wheat, wheatLineSigningAccount))
                {
                    throw std::runtime_error("invalid database state: must have matching trust line");
                }
                wheatLineSigningAccount.mEntry.trustLine().balance += wheatReceived;
                wheatLineSigningAccount.storeChange(delta, ledgerMaster);
            }

            if (sheep.type() == NATIVE)
            {
                mSigningAccount->mEntry.account().balance -= sheepSent;
                mSigningAccount->storeChange(delta, ledgerMaster);
            }
            else
            {
                mSheepLineA.mEntry.trustLine().balance -= sheepSent;
                mSheepLineA.storeChange(delta, ledgerMaster);
            }
        }

        // recomputes the amount of sheep for sale
        mSellSheepOffer.mEntry.offer().amount = maxSheepReceived - sheepReceived;

        int64_t minAmount = 0;
        {
            // TODO: compute the minimum amount that can be represented in an offer
        }

        if (mSellSheepOffer.mEntry.offer().amount > minAmount)
        { // we still have sheep to sell so leave an offer

            if (creatingNewOffer)
            {
                // make sure we don't allow us to add offers when we don't have the minbalance
                if (mSigningAccount->mEntry.account().balance <
                    ledgerMaster.getMinBalance(mSigningAccount->mEntry.account().ownerCount + 1))
                {
                    innerResult().result.code(CreateOffer::UNDERFUNDED);
                    return false;
                }

                mSellSheepOffer.storeAdd(tempDelta, ledgerMaster);

                mSigningAccount->mEntry.account().ownerCount++;
                mSigningAccount->storeChange(tempDelta, ledgerMaster);
            }
            else
            {
                mSellSheepOffer.storeChange(tempDelta, ledgerMaster);
            }
        }
        else
        {
            if (!creatingNewOffer)
            {
                mSellSheepOffer.storeDelete(tempDelta, ledgerMaster);
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
