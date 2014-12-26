#include "transactions/PaymentTx.h"
#include "lib/json/json.h"
#include "util/Logging.h"
#include "ledger/LedgerDelta.h"
#include "ledger/TrustLine.h"
#include "ledger/OfferEntry.h"

namespace stellar
{
  
    void PaymentTx::doApply(TxDelta& delta,LedgerMaster& ledgerMaster)
    {
        uint32_t minBalance = ledgerMaster.getCurrentLedger()->getMinBalance();
        
        stellarxdr::LedgerEntry entry;
        if(ledgerMaster.getDatabase().loadAccount(mEnvelope.tx.body.paymentTx().destination, entry))
        {
            AccountEntry receiver(entry);

            if(isNativeCurrency(mEnvelope.tx.body.paymentTx().currency.currency))
            {   // sending STR

                if(mEnvelope.tx.body.paymentTx().path.size())
                {
                    mResultCode = txMALFORMED;
                    return;
                }

                if(mSigningAccount->mEntry.account().balance < minBalance + mEnvelope.tx.body.paymentTx().amount)
                {   // they don't have enough to send
                    mResultCode = txUNDERFUNDED;
                    return;
                }
                
                if(receiver.getIndex() == mSigningAccount->getIndex())
                {   // sending to yourself
                    mResultCode = txSUCCESS;
                    return;
                }
                
                delta.setStart(receiver);
                mSigningAccount->mEntry.account().balance -= mEnvelope.tx.body.paymentTx().amount;
                receiver.mEntry.account().balance += mEnvelope.tx.body.paymentTx().amount;
                delta.setFinal(receiver);
                delta.setFinal(*mSigningAccount.get());

            }else
            {   // sending credit
                sendCredit(receiver, delta,ledgerMaster);
                return;
            }
        } else
        {   // this tx is creating an account
            if(isNativeCurrency(mEnvelope.tx.body.paymentTx().currency.currency))
            {
                if(mEnvelope.tx.body.paymentTx().amount < minBalance)
                {   // not over the minBalance to make an account
                    mResultCode = txNOACCOUNT;
                    return;
                } else
                {
                    AccountEntry receiver(mEnvelope.tx.body.paymentTx().destination);
                    mSigningAccount->mEntry.account().balance -= mEnvelope.tx.body.paymentTx().amount;
                    receiver.mEntry.account().balance = mEnvelope.tx.body.paymentTx().amount;
                    delta.setFinal(receiver);
                    delta.setFinal(*mSigningAccount.get());
                }
            } else
            {   // trying to send credit to an unmade account
                mResultCode = txNOACCOUNT;
                return;
            }
        }
    }
    

    // convert between two currencies at the best rate
    // returns amount of the source currency or 0 for not possible
    int64_t PaymentTx::convert(int64_t amountToFill,stellarxdr::CurrencyIssuer& source,
        stellarxdr::CurrencyIssuer& dest, TxDelta& delta,LedgerMaster& ledgerMaster)
    {
     
        int64_t amountToSend = 0;
        int offerOffset = 0;
        while(amountToFill > 0)
        {
            vector<stellarxdr::LedgerEntry> retList;
            ledgerMaster.getDatabase().loadBestOffers(5, offerOffset, source, dest, retList);
            for(auto entry : retList)
            {
                OfferEntry offer(entry);
                // TODO.2 make sure the offer is funded

                delta.setStart(offer);
                int64_t canFill=(entry.offer().amount*entry.offer().price) / OFFER_PRICE_DIVISOR;
                if(canFill > amountToFill)
                { // need to only take part of the offer
                    int64_t amountToTake = amountToFill*OFFER_PRICE_DIVISOR / entry.offer().price;
                    offer.mEntry.offer().amount -= amountToTake;
                    if(offer.mEntry.offer().amount < 0)
                    {
                        CLOG(ERROR, "Tx") << "PaymentTx::convert offer amount below 0";
                    }else delta.setFinal(offer);

                    amountToFill = 0;
                    break;
                } else
                {   // take the whole offer                
                    amountToFill -= canFill;
                }
                
            }
            // still stuff to fill but no more offers
            if(amountToFill > 0 && retList.size() < 5) return 0;
            offerOffset += retList.size();
        }
        
        return amountToSend;
    }

    // work backward to determine how much they need to send to get the 
    // specified amount of currency to the recipient
    // TODO.2 make sure path doesn't loop
    void PaymentTx::sendCredit(AccountEntry& receiver, TxDelta& delta, LedgerMaster& ledgerMaster)
    {
        int64_t needToSend = 0;
        int64_t amountToSend = mEnvelope.tx.body.paymentTx().amount;

        stellarxdr::LedgerEntry destEntry;
        // make sure guy can hold what you are trying to send him
        if(!ledgerMaster.getDatabase().loadTrustLine(mEnvelope.tx.body.paymentTx().destination, 
            mEnvelope.tx.body.paymentTx().currency, destEntry))
        {
            mResultCode = txNOTRUST;
            return;
        }

        if(destEntry.trustLine().limit < amountToSend + destEntry.trustLine().balance)
        {
            mResultCode = txLINEFULL;
            return;
        }

        TrustLine destLine(destEntry);

        if(mEnvelope.tx.body.paymentTx().path.size())
        {
            TxDelta tempDelta;
            stellarxdr::CurrencyIssuer lastCI=mEnvelope.tx.body.paymentTx().currency;
            int64_t lastAmount=mEnvelope.tx.body.paymentTx().amount;
            for(int n = mEnvelope.tx.body.paymentTx().path.size(); n >= 0;  n--)
            {   // convert from link to last
                lastAmount = convert(lastAmount,mEnvelope.tx.body.paymentTx().path[n], lastCI, tempDelta,ledgerMaster);
                if(!lastAmount)
                {   // there isn't enough offer depth
                    mResultCode = txOVERSENDMAX;
                    return;
                }

                lastCI = mEnvelope.tx.body.paymentTx().path[n];
            }
            needToSend = lastAmount;

            if(needToSend > mEnvelope.tx.body.paymentTx().sendMax)
            { // make sure not over the max
                mResultCode = txOVERSENDMAX;
                return;
            }

            delta.merge(tempDelta);

            mResultCode = txSUCCESS;
            return;
        } else
        {   // straight credit transfer
            // make sure you have enough to send him
            stellarxdr::LedgerEntry issuerEntry;
            if(!ledgerMaster.getDatabase().loadAccount(*mEnvelope.tx.body.paymentTx().currency.issuer, issuerEntry))
            {
                CLOG(ERROR, "Tx") << "PaymentTx::sendCredit Issuer not found";
                mResultCode = txMALFORMED;
                return;
            }

            if(issuerEntry.account().transferRate)
            {
                needToSend = amountToSend + (amountToSend*issuerEntry.account().transferRate) / 1000000;  // TODO.3 This probably needs to be some big number thing
            }else needToSend = amountToSend;
         
            if(needToSend > mEnvelope.tx.body.paymentTx().sendMax)
            { // make sure not over the max
                mResultCode = txOVERSENDMAX;
                return;
            }

            // make sure source has enough credit
            stellarxdr::LedgerEntry sourceLineEntry;
            if(!ledgerMaster.getDatabase().loadTrustLine(mEnvelope.tx.account, mEnvelope.tx.body.paymentTx().currency, sourceLineEntry))
            {
                mResultCode = txUNDERFUNDED;
                return;
            }
            
            if(sourceLineEntry.trustLine().balance < needToSend)
            {
                mResultCode = txUNDERFUNDED;
                return;
            }

            TrustLine sourceLine(sourceLineEntry);
           
            delta.setStart(sourceLine);
            delta.setStart(destLine);

            sourceLine.mEntry.trustLine().balance -= needToSend;
            destLine.mEntry.trustLine().balance += amountToSend;

            delta.setFinal(sourceLine);
            delta.setFinal(destLine);

            mResultCode = txSUCCESS;
            return;
        }
    }

    // make sure there is no path for native transfer
    // make sure account has enough to send
    bool PaymentTx::doCheckValid(LedgerMaster& ledgerMaster)
    {
        // TODO.2
        return(false);
    }

}

