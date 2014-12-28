#include "transactions/CreateOfferFrame.h"
#include "ledger/OfferFrame.h"

// TODO.2 still 
namespace stellar
{
    // you are selling sheep for wheat
    // need to check the counter offers selling wheat for sheep
// see if this is modifying an old offer
// TODO.2 make sure these issuers exist and you can hold the ask currency
// TODO.2 see if this offer crosses any existing offers
void CreateOfferFrame::doApply(TxDelta& delta, LedgerMaster& ledgerMaster)
{
    uint32_t offerSeq = mEnvelope.tx.body.createOfferTx().sequence;
    // plus 1 since the account seq has already been incremented at this point
    if(offerSeq+1 == mSigningAccount.mEntry.account().sequence)
    { // creating a new Offer

        Currency& sheep = mEnvelope.tx.body.createOfferTx().takerGets;
        Currency& wheat = mEnvelope.tx.body.createOfferTx().takerPays;

        TrustFrame sheepLineFrame;
        if( !sheep.native() && 
            !ledgerMaster.getDatabase().loadTrustLine(mEnvelope.tx.account, sheep.ci(), sheepLineFrame))
        {   // we don't have what we are trying to sell
            mResultCode = txNOTRUST;
            return;
        }

        if(!wheat.native())
        {
            TrustFrame wheatLineFrame;
            if( !ledgerMaster.getDatabase().loadTrustLine(mEnvelope.tx.account, wheat.ci(), wheatLineFrame))
            {   // we can't hold what we are trying to buy
                mResultCode = txNOTRUST;
                return;
            }

            if(!wheatLineFrame.mEntry.trustLine().authorized)
            {   // we are not authorized to hold what we are trying to buy
                mResultCode = txNOT_AUTHORIZED;
                return;
            }
        }
       

        int64_t amountOfWheat = mEnvelope.tx.body.createOfferTx().amount;
        int64_t sheepPrice = mEnvelope.tx.body.createOfferTx().price;

        int64_t amountOfSheepOwned = sheepLineFrame.mEntry.trustLine().balance;
        // tx gives Aw and Ps
        // Ps=Aw/As
        // As=Aw*D/Ps
        int64_t amountOfSheepForSale = amountOfWheat*OFFER_PRICE_DIVISOR / sheepPrice;
        if(amountOfSheepOwned < amountOfSheepForSale) amountOfSheepForSale = amountOfSheepOwned;
        
        int64_t amountSheepSold = convert(sheep, wheat, 
            amountOfSheepForSale, sheepPrice, delta, ledgerMaster);

         
        if( (amountSheepSold < amountOfSheepOwned) &&
            (amountSheepSold < amountOfSheepForSale))
        { // we still have sheep to sell so leave an offer
            OfferFrame offer(mEnvelope.tx);
            offer.mEntry.offer().amount = amountOfSheepForSale - amountSheepSold;
            mSigningAccount.mEntry.account().ownerCount++;
            delta.setFinal(offer);
            delta.setFinal(mSigningAccount);
        }
        
    } else
    { // modifying an old offer
        OfferFrame oldOffer;
        if(ledgerMaster.getDatabase().loadOffer(mEnvelope.tx.account, offerSeq, oldOffer))
        {
            // TODO.2 make sure the currencies are the same
                   
            delta.setStart(oldOffer);
            oldOffer.mEntry.offer().price = mEnvelope.tx.body.createOfferTx().price;
            oldOffer.mEntry.offer().amount = mEnvelope.tx.body.createOfferTx().amount;
            oldOffer.mEntry.offer().passive = mEnvelope.tx.body.createOfferTx().passive;
            
            delta.setFinal(oldOffer);
        } else
        {
            mResultCode = txOFFER_NOT_FOUND;
            return;
        }
    }
        
}



}