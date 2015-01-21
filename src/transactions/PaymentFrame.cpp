#include "transactions/PaymentFrame.h"
#include "lib/json/json.h"
#include "util/Logging.h"
#include "ledger/LedgerDelta.h"
#include "ledger/TrustFrame.h"
#include "ledger/OfferFrame.h"
#include "database/Database.h"

#define MAX_PAYMENT_PATH_LENGTH 5
// TODO.2 handle sending to and from an issuer and with offers
// TODO.2 clean up trustlines 
namespace stellar
{ 

using namespace std;

    PaymentFrame::PaymentFrame(const TransactionEnvelope& envelope) : TransactionFrame(envelope)
    {

    }

    bool PaymentFrame::doApply(LedgerDelta& delta,LedgerMaster& ledgerMaster)
    {
        AccountFrame destAccount;

        // if sending to self directly, just mark as success
        if (mEnvelope.tx.body.paymentTx().destination == mSigningAccount->getID()
            && mEnvelope.tx.body.paymentTx().path.empty())
        {
            innerResult().result.code(Payment::SUCCESS);
            return true;
        }

        if (!ledgerMaster.getDatabase().loadAccount(mEnvelope.tx.body.paymentTx().destination, destAccount))
        {   // this tx is creating an account
            if (mEnvelope.tx.body.paymentTx().currency.type() == NATIVE)
            {
                if (mEnvelope.tx.body.paymentTx().amount < ledgerMaster.getMinBalance(0))
                {   // not over the minBalance to make an account
                    innerResult().result.code(Payment::UNDERFUNDED);
                    return false;
                }
                else
                {
                    destAccount.mEntry.account().accountID = mEnvelope.tx.body.paymentTx().destination;
                    destAccount.mEntry.account().balance = 0;

                    destAccount.storeAdd(delta, ledgerMaster);
                }
            }
            else
            {   // trying to send credit to an unmade account
                innerResult().result.code(Payment::NO_DESTINATION);
                return false;
            }
        }

        return sendNoCreate(destAccount, delta, ledgerMaster);
    }
    

    // A is sending to B
    // work backward to determine how much they need to send to get the 
    // specified amount of currency to the recipient
    bool PaymentFrame::sendNoCreate(AccountFrame& receiver, LedgerDelta& delta, LedgerMaster& ledgerMaster)
    {
        bool multi_mode = mEnvelope.tx.body.paymentTx().path.size();
        if (multi_mode)
        {
            innerResult().result.code(Payment::SUCCESS_MULTI);
        }
        else
        {
            innerResult().result.code(Payment::SUCCESS);
        }

        // tracks the last amount that was traded
        int64_t sendAmount = mEnvelope.tx.body.paymentTx().amount;
        Currency sendCurrency = mEnvelope.tx.body.paymentTx().currency;

        // update balances, walks backwards

        // update last balance in the chain
        {

            if (sendCurrency.type() == NATIVE)
            {
                receiver.mEntry.account().balance += sendAmount;
                receiver.storeChange(delta, ledgerMaster);
            }
            else if (receiver.getID() !=
                sendCurrency.isoCI().issuer)
            {
                TrustFrame destLine;

                if (!ledgerMaster.getDatabase().loadTrustLine(receiver.getID(),
                    sendCurrency, destLine))
                {
                    innerResult().result.code(Payment::NO_TRUST);
                    return false;
                }

                if (destLine.mEntry.trustLine().limit <
                    sendAmount + destLine.mEntry.trustLine().balance)
                {
                    innerResult().result.code(Payment::LINE_FULL);
                    return false;
                }

                if (!destLine.mEntry.trustLine().authorized)
                {
                    innerResult().result.code(Payment::NOT_AUTHORIZED);
                    return false;
                }

                destLine.mEntry.trustLine().balance += sendAmount;
                destLine.storeChange(delta, ledgerMaster);
            }

            if (multi_mode)
            {
                innerResult().result.multi().last = Payment::SimplePaymentResult(
                    receiver.getID(),
                    sendCurrency,
                    sendAmount);
            }
        }

        if (multi_mode)
        {
            // now, walk the path backwards
            int64_t lastAmount=sendAmount;
            for(int i = mEnvelope.tx.body.paymentTx().path.size()-1; i >= 0;  i--)
            {
                int64_t amountToSell = 0;
                if(!convert(mEnvelope.tx.body.paymentTx().path[i],
                    sendCurrency, lastAmount,
                    amountToSell,
                    delta, ledgerMaster))
                {
                    return false;
                }
                lastAmount = amountToSell;
                sendCurrency = mEnvelope.tx.body.paymentTx().path[i];
            }
            sendAmount = lastAmount;
        } 
        
        if (sendAmount > mEnvelope.tx.body.paymentTx().sendMax)
        { // make sure not over the max
            innerResult().result.code(Payment::OVERSENDMAX);
            return false;
        }

        // last step: we've reached the first account in the chain, update its balance

        if(sendCurrency.type() == NATIVE)
        {
            if (mEnvelope.tx.body.paymentTx().path.size())
            {
                innerResult().result.code(Payment::MALFORMED);
                return false;
            }

            int64_t minBalance = ledgerMaster.getMinBalance(mSigningAccount->mEntry.account().ownerCount);

            if (mSigningAccount->mEntry.account().balance < (minBalance + sendAmount))
            {   // they don't have enough to send
                innerResult().result.code(Payment::UNDERFUNDED);
                return false;
            }

            mSigningAccount->mEntry.account().balance -= sendAmount;
            mSigningAccount->storeChange(delta, ledgerMaster);
        }
        else
        {
            // issuer can always send its own credit
            if(getSourceID() != sendCurrency.isoCI().issuer)
            {
                AccountFrame issuer;
                if(!ledgerMaster.getDatabase().loadAccount(sendCurrency.isoCI().issuer, issuer))
                {
                    CLOG(ERROR, "Tx") << "PaymentTx::sendCredit Issuer not found";
                    innerResult().result.code(Payment::MALFORMED);
                    return false;
                }

                if(issuer.mEntry.account().transferRate != TRANSFER_RATE_DIVISOR)
                {
                    sendAmount = sendAmount +
                        bigDivide(sendAmount, issuer.mEntry.account().transferRate, TRANSFER_RATE_DIVISOR);
                }

                TrustFrame sourceLineFrame;
                if(!ledgerMaster.getDatabase().loadTrustLine(mEnvelope.tx.account,
                    sendCurrency, sourceLineFrame))
                {
                    innerResult().result.code(Payment::UNDERFUNDED);
                    return false;
                }

                if(sourceLineFrame.mEntry.trustLine().balance < sendAmount)
                {
                    innerResult().result.code(Payment::UNDERFUNDED);
                    return false;
                }
                
                sourceLineFrame.mEntry.trustLine().balance -= sendAmount;
                sourceLineFrame.storeChange(delta, ledgerMaster);

            }
        }
        

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
        Currency& wheat, int64_t amountWheat,
        int64_t& retAmountSheep,
        LedgerDelta& delta, LedgerMaster& ledgerMaster)
    {
        int64_t wheatTransferRate = getTransferRate(wheat, ledgerMaster);

        retAmountSheep = 0;
        size_t offerOffset = 0;
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
                innerResult().result.code(Payment::OVERSENDMAX);
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
        int64_t maxWheatToReceive, int64_t& numWheatReceived, 
        int64_t& numSheepReceived,
        int64_t wheatTransferRate,
        LedgerDelta& delta, LedgerMaster& ledgerMaster)
    {
        Currency& sheep = sellingWheatOffer.mEntry.offer().takerPays;
        Currency& wheat = sellingWheatOffer.mEntry.offer().takerGets;
        uint256& accountBID = sellingWheatOffer.mEntry.offer().accountID;


        AccountFrame accountB;
        if(!ledgerMaster.getDatabase().loadAccount(accountBID, accountB))
        {
            throw runtime_error("invalid database state: offer must have matching account");
        }

        TrustFrame wheatLineAccountB;
        if(wheat.type()!=NATIVE)
        {
            if(!ledgerMaster.getDatabase().loadTrustLine(accountBID,
                wheat, wheatLineAccountB))
            {
                throw runtime_error("invalid database state: offer must have matching trust line");
            }
        }


        TrustFrame sheepLineAccountB;
        if(sheep.type()!=NATIVE)
        {
            if(!ledgerMaster.getDatabase().loadTrustLine(accountBID,
                sheep, sheepLineAccountB))
            {
                throw runtime_error("invalid database state: offer must have matching trust line");
            }
        }

        numWheatReceived = 0;
        if (wheat.type() == NATIVE)
        {
            numWheatReceived = accountB.mEntry.account().balance;
        }
        else
        {
            numWheatReceived = wheatLineAccountB.mEntry.trustLine().balance;
        }

        // you can receive the lesser of the amount of wheat offered or the amount the guy has
        if (wheatTransferRate != TRANSFER_RATE_DIVISOR)
        {
            numWheatReceived = bigDivide(numWheatReceived, wheatTransferRate, TRANSFER_RATE_DIVISOR);
        }
        if (numWheatReceived > sellingWheatOffer.mEntry.offer().amount)
        {
            numWheatReceived = sellingWheatOffer.mEntry.offer().amount;
        }

        bool offerLeft = false;
        if(numWheatReceived > maxWheatToReceive)
        {
            offerLeft = true;
            numWheatReceived = maxWheatToReceive;
        }

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
            sellingWheatOffer.storeChange(delta, ledgerMaster);
        } else
        {   // entire offer is taken
            sellingWheatOffer.storeDelete(delta, ledgerMaster);
            accountB.mEntry.account().ownerCount--;
            accountB.storeChange(delta, ledgerMaster);
        }

        // Adjust balances
        if(sheep.type()==NATIVE)
        {
            accountB.mEntry.account().balance += numSheepReceived;
            accountB.storeChange(delta, ledgerMaster);
        } else
        {
            sheepLineAccountB.mEntry.trustLine().balance += numSheepReceived;
            sheepLineAccountB.storeChange(delta, ledgerMaster);
        }

        if(wheat.type()==NATIVE)
        {
            accountB.mEntry.account().balance -= numWheatSent;
            accountB.storeChange(delta, ledgerMaster);
        } else
        {
            wheatLineAccountB.mEntry.trustLine().balance -= numWheatSent;
            wheatLineAccountB.storeChange(delta, ledgerMaster);
        }

        innerResult().result.multi().offers.push_back(Payment::ClaimOfferAtom(
            accountB.getID(),
            sellingWheatOffer.getSequence(),
            sheep,
            numSheepReceived
            ));

        return true;
    }

    bool PaymentFrame::doCheckValid(Application& app)
    {
        if (mEnvelope.tx.body.paymentTx().path.size() > MAX_PAYMENT_PATH_LENGTH)
        {
            innerResult().result.code(Payment::MALFORMED);
            return false;
        }

        return true;
    }
}

