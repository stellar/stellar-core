#include "transactions/PaymentFrame.h"
#include "lib/json/json.h"
#include "util/Logging.h"
#include "ledger/LedgerDelta.h"
#include "ledger/TrustFrame.h"
#include "ledger/OfferFrame.h"

#define MAX_PAYMENT_PATH_LENGTH 5

namespace stellar
{ 
    PaymentFrame::PaymentFrame(const TransactionEnvelope& envelope) : TransactionFrame(envelope)
    {

    }

    void PaymentFrame::doApply(TxDelta& delta,LedgerMaster& ledgerMaster)
    {
        int64_t minBalance = ledgerMaster.getMinBalance(mSigningAccount.mEntry.account().ownerCount);
        
        AccountFrame destAccount;
        bool isNew = false;

        if (!ledgerMaster.getDatabase().loadAccount(mEnvelope.tx.body.paymentTx().destination, destAccount))
        {   // this tx is creating an account
            if (mEnvelope.tx.body.paymentTx().currency.type() == NATIVE)
            {
                if (mEnvelope.tx.body.paymentTx().amount < ledgerMaster.getMinBalance(0))
                {   // not over the minBalance to make an account
                    mResultCode = txUNDERFUNDED;
                    return;
                }
                else
                {
                    destAccount.mEntry.account().accountID = mEnvelope.tx.body.paymentTx().destination;
                    destAccount.mEntry.account().balance = 0;
                    isNew = true;
                }
            }
            else
            {   // trying to send credit to an unmade account
                mResultCode = txNOACCOUNT;
                return;
            }
        }

        if(mEnvelope.tx.body.paymentTx().currency.type()==NATIVE)
        {   // sending STR

            if(mEnvelope.tx.body.paymentTx().path.size())
            {
                mResultCode = txMALFORMED;
                return;
            }

            if(mSigningAccount.mEntry.account().balance < minBalance + mEnvelope.tx.body.paymentTx().amount)
            {   // they don't have enough to send
                mResultCode = txUNDERFUNDED;
                return;
            }

            if(destAccount.getIndex() == mSigningAccount.getIndex())
            {   // sending to yourself
                mResultCode = txSUCCESS;
                return;
            }

            if (!isNew)
            {
                delta.setStart(destAccount);
            }
            mSigningAccount.mEntry.account().balance -= mEnvelope.tx.body.paymentTx().amount;
            destAccount.mEntry.account().balance += mEnvelope.tx.body.paymentTx().amount;
            delta.setFinal(destAccount);
            delta.setFinal(mSigningAccount);
            mResultCode = txSUCCESS;

        }else
        {   // sending credit
            TxDelta tempDelta;
            if(sendCredit(destAccount, tempDelta, ledgerMaster))
            {
                delta.merge(tempDelta);
                mResultCode = txSUCCESS;
            }

            return;
        }
    }
    

    // A is sending to B
    // work backward to determine how much they need to send to get the 
    // specified amount of currency to the recipient
    bool PaymentFrame::sendCredit(AccountFrame& receiver, TxDelta& delta, LedgerMaster& ledgerMaster)
    {
        TrustFrame destLine;
        // make sure guy can hold what you are trying to send him
        if(!ledgerMaster.getDatabase().loadTrustLine(mEnvelope.tx.body.paymentTx().destination, 
            mEnvelope.tx.body.paymentTx().currency, destLine))
        {
            mResultCode = txNOTRUST;
            return false;
        }

        if(destLine.mEntry.trustLine().limit < mEnvelope.tx.body.paymentTx().amount + destLine.mEntry.trustLine().balance)
        {
            mResultCode = txLINEFULL;
            return false;
        }

        if(!destLine.mEntry.trustLine().authorized)
        {
            mResultCode = txNOT_AUTHORIZED;
            return false;
        }
        
        int64_t sendAmount = mEnvelope.tx.body.paymentTx().amount;
        Currency sendCurrency=mEnvelope.tx.body.paymentTx().currency;
        
        if(mEnvelope.tx.body.paymentTx().path.size())
        {      
            int64_t lastAmount=mEnvelope.tx.body.paymentTx().amount;
            for(int n = mEnvelope.tx.body.paymentTx().path.size(); n >= 0;  n--)
            {   // convert from link to last
                lastAmount = 0;
                int64_t amountToSell = 0;
                if(!convert(mEnvelope.tx.body.paymentTx().path[n], sendCurrency, lastAmount, amountToSell, delta, ledgerMaster))
                {
                    return false;
                }
                lastAmount = amountToSell;

                sendCurrency = mEnvelope.tx.body.paymentTx().path[n];
            }
            sendAmount = lastAmount;
        } 
        
        
        // make sure you have enough to send him
        if(sendCurrency.type()==NATIVE)
        {
            if(mSigningAccount.mEntry.account().balance < sendAmount + ledgerMaster.getMinBalance(mSigningAccount.mEntry.account().ownerCount))
            {
                mResultCode = txUNDERFUNDED;
                return false;
            }
        }else
        { // make sure source has enough credit
            AccountFrame issuer;
            if(!ledgerMaster.getDatabase().loadAccount(sendCurrency.isoCI().issuer, issuer))
            {
                CLOG(ERROR, "Tx") << "PaymentTx::sendCredit Issuer not found";
                mResultCode = txMALFORMED;
                return false;
            }

            if(issuer.mEntry.account().transferRate != TRANSFER_RATE_DIVISOR)
            {
                sendAmount = sendAmount +
                    bigDivide(sendAmount,issuer.mEntry.account().transferRate,TRANSFER_RATE_DIVISOR);  
            } 

            TrustFrame sourceLineFrame;
            if(!ledgerMaster.getDatabase().loadTrustLine(mEnvelope.tx.account,
                sendCurrency, sourceLineFrame))
            {
                mResultCode = txUNDERFUNDED;
                return false;
            }

            if(sourceLineFrame.mEntry.trustLine().balance < sendAmount)
            {
                mResultCode = txUNDERFUNDED;
                return false;
            }
            delta.setStart(sourceLineFrame);
            sourceLineFrame.mEntry.trustLine().balance -= sendAmount;
            delta.setFinal(sourceLineFrame);
        }
        
        if(sendAmount > mEnvelope.tx.body.paymentTx().sendMax)
        { // make sure not over the max
            mResultCode = txOVERSENDMAX;
            return false;
        }

        
        delta.setStart(destLine);
        destLine.mEntry.trustLine().balance += mEnvelope.tx.body.paymentTx().amount;
        delta.setFinal(destLine);

        mResultCode = txSUCCESS;
        return true;
    }

    /*
    For each link in the chain we need to try to buy the amount of the dest currency
    Keep track of how much of the source currency we needed to buy
    


    */
    
    // try to gather enough orders to convert retAmountSheep into amountWheat
    // will adjust all the offers and balances in the middle
    // returns false if not possible or something else goes wrong
    bool PaymentFrame::convert(Currency& sheep,
        Currency& wheat, int64_t amountWheat, int64_t& retAmountSheep,
        TxDelta& delta, LedgerMaster& ledgerMaster)
    {
        int64_t wheatTransferRate = getTransferRate(wheat, ledgerMaster);

        retAmountSheep = 0;
        int offerOffset = 0;
        while(amountWheat > 0)
        {
            vector<OfferFrame> retList;
            ledgerMaster.getDatabase().loadBestOffers(5, offerOffset, sheep, wheat, retList);
            for(auto offer : retList)
            { 
                
                int64_t numSheepReceived, numWheatReceived;
                if(!crossOffer(offer, amountWheat, numWheatReceived, numSheepReceived, wheatTransferRate, delta, ledgerMaster))
                {
                    return false;
                }

                amountWheat -= numWheatReceived;
                retAmountSheep += numSheepReceived;

            }
            // still stuff to fill but no more offers
            if(amountWheat > 0 && retList.size() < 5)
            { // there isn't enough offer depth
                mResultCode = txOVERSENDMAX;
                return false;
            }
            offerOffset += retList.size();
        }

        return true;
        
    }

    // take up to amountToTake of the offer
    // amountToTake is reduced by the amount taken
    // returns false if there was an error
    bool PaymentFrame::crossOffer(OfferFrame& sellingWheatOffer,
        int64_t maxWheatReceived, int64_t& numWheatReceived, 
        int64_t& numSheepReceived,
        int64_t wheatTransferRate,
        TxDelta& delta, LedgerMaster& ledgerMaster)
    {
        Currency& sheep = sellingWheatOffer.mEntry.offer().takerPays;
        Currency& wheat = sellingWheatOffer.mEntry.offer().takerGets;
        uint256& accountBID = sellingWheatOffer.mEntry.offer().accountID;


        AccountFrame accountB;
        if(!ledgerMaster.getDatabase().loadAccount(accountBID, accountB))
        {
            mResultCode = txINTERNAL_ERROR;
            return false;
        }

        TrustFrame wheatLineAccountB;
        if(wheat.type()!=NATIVE)
        {
            if(!ledgerMaster.getDatabase().loadTrustLine(accountBID,
                wheat, wheatLineAccountB))
            {
                mResultCode = txINTERNAL_ERROR;
                return false;
            }
        }


        TrustFrame sheepLineAccountB;
        if(sheep.type()!=NATIVE)
        {
            if(!ledgerMaster.getDatabase().loadTrustLine(accountBID,
                sheep, sheepLineAccountB))
            {
                mResultCode = txINTERNAL_ERROR;
                return false;
            }
        }

        numWheatReceived = 0;
        if(wheat.type()==NATIVE) numWheatReceived = accountB.mEntry.account().balance;
        else numWheatReceived = wheatLineAccountB.mEntry.trustLine().balance;

        // you can receive the lesser of the amount of wheat offered or the amount the guy has
        if(wheatTransferRate != TRANSFER_RATE_DIVISOR)
            numWheatReceived = bigDivide(numWheatReceived,wheatTransferRate,TRANSFER_RATE_DIVISOR);
        if(numWheatReceived > sellingWheatOffer.mEntry.offer().amount)
            numWheatReceived = sellingWheatOffer.mEntry.offer().amount;

        bool offerLeft = false;
        if(numWheatReceived > maxWheatReceived)
        {
            offerLeft = true;
            numWheatReceived = maxWheatReceived;
        }

        delta.setStart(sellingWheatOffer);

        int64_t numWheatSent;

        // this guy can get X wheat to you. How many sheep does that get him?
        numSheepReceived = bigDivide(numWheatReceived,sellingWheatOffer.mEntry.offer().price,OFFER_PRICE_DIVISOR);
        if(offerLeft)
        { // need to only take part of the wheat offer
            sellingWheatOffer.mEntry.offer().amount -= numWheatReceived;
        } else
        { // take whole wheat offer
            sellingWheatOffer.mEntry.offer().amount = 0;
        }

        numWheatSent = bigDivide(numWheatReceived,TRANSFER_RATE_DIVISOR,wheatTransferRate);

        if(sellingWheatOffer.mEntry.offer().amount < 0)
        {
            CLOG(ERROR, "Tx") << "PaymentFrame::crossOffer offer amount below 0 :" << sellingWheatOffer.mEntry.offer().amount;
            sellingWheatOffer.mEntry.offer().amount = 0;
        }
        if(sellingWheatOffer.mEntry.offer().amount)
        {
            delta.setFinal(sellingWheatOffer);
        } else
        {   // entire offer is taken
            accountB.mEntry.account().ownerCount--;
            delta.setFinal(accountB);
        }

        // Adjust balances
        if(sheep.type()==NATIVE)
        {
            accountB.mEntry.account().balance += numSheepReceived;
            delta.setFinal(accountB);
        } else
        {
            sheepLineAccountB.mEntry.trustLine().balance += numSheepReceived;
            delta.setFinal(sheepLineAccountB);
        }

        if(wheat.type()==NATIVE)
        {
            accountB.mEntry.account().balance -= numWheatSent;
            delta.setFinal(accountB);
        } else
        {
            wheatLineAccountB.mEntry.trustLine().balance -= numWheatSent;
            delta.setFinal(wheatLineAccountB);
        }
        return true;
    }

    // make sure there is no path for native transfer
    // make sure there are no loops in the path
    // make sure the path is less than N steps
    bool PaymentFrame::doCheckValid(Application& app)
    {
        if(mEnvelope.tx.body.paymentTx().currency.type()==NATIVE)
        {
            if (mEnvelope.tx.body.paymentTx().path.size())
            {
                mResultCode = txMALFORMED;
                return false;
            }
        }
        else
        {
            if (mEnvelope.tx.body.paymentTx().path.size() > MAX_PAYMENT_PATH_LENGTH)
            {
                mResultCode = txMALFORMED;
                return false;
            }

            // make sure there are no loops in the path
            for(auto step : mEnvelope.tx.body.paymentTx().path)
            {
                bool seen = false;
                for(auto inner : mEnvelope.tx.body.paymentTx().path)
                {
                    if(compareCurrency(step, inner))
                    {
                        if (seen)
                        {
                            mResultCode = txMALFORMED;
                            return false;
                        }
                        seen=true;
                    }
                }
            }
        }

        return true;
    }

}

