#include "transactions/PaymentFrame.h"
#include "util/Logging.h"
#include "ledger/LedgerDelta.h"
#include "ledger/TrustFrame.h"
#include "ledger/OfferFrame.h"
#include "database/Database.h"
#include "OfferExchange.h"

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

        Database &db = ledgerMaster.getDatabase();

        if (!AccountFrame::loadAccount(mEnvelope.tx.body.paymentTx().destination,
            destAccount, db))
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

                    destAccount.storeAdd(delta, db);
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
    

    // work backward to determine how much they need to send to get the 
    // specified amount of currency to the recipient
    bool PaymentFrame::sendNoCreate(AccountFrame& destination, LedgerDelta& delta, LedgerMaster& ledgerMaster)
    {
        Database &db = ledgerMaster.getDatabase();

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
        int64_t curBReceived = mEnvelope.tx.body.paymentTx().amount;
        Currency curB = mEnvelope.tx.body.paymentTx().currency;

        // update balances, walks backwards

        // update last balance in the chain
        {

            if (curB.type() == NATIVE)
            {
                destination.mEntry.account().balance += curBReceived;
                destination.storeChange(delta, db);
            }
            else if (destination.getID() !=
                curB.isoCI().issuer)
            {
                TrustFrame destLine;

                if (!TrustFrame::loadTrustLine(destination.getID(),
                    curB, destLine, db))
                {
                    innerResult().result.code(Payment::NO_TRUST);
                    return false;
                }

                if (destLine.mEntry.trustLine().limit <
                    curBReceived + destLine.mEntry.trustLine().balance)
                {
                    innerResult().result.code(Payment::LINE_FULL);
                    return false;
                }

                if (!destLine.mEntry.trustLine().authorized)
                {
                    innerResult().result.code(Payment::NOT_AUTHORIZED);
                    return false;
                }

                destLine.mEntry.trustLine().balance += curBReceived;
                destLine.storeChange(delta, db);
            }

            if (multi_mode)
            {
                innerResult().result.multi().last = Payment::SimplePaymentResult(
                    destination.getID(),
                    curB,
                    curBReceived);
            }
        }

        if (multi_mode)
        {
            // now, walk the path backwards
            for(int i = (int)mEnvelope.tx.body.paymentTx().path.size()-1; i >= 0;  i--)
            {
                int64_t curASent, actualCurBReceived;
                Currency &curA = mEnvelope.tx.body.paymentTx().path[i];

                OfferExchange oe(delta, ledgerMaster);

                // curA -> curB
                OfferExchange::ConvertResult r = oe.convertWithOffers(
                    curA, INT64_MAX, curASent,
                    curB, curBReceived, actualCurBReceived,
                    nullptr);
                switch (r)
                {
                case OfferExchange::eOK:
                    break;
                case OfferExchange::eFilterFail:
                case OfferExchange::eFilterStop:
                    assert(false); // no filter -> should not happen
                    break;
                case OfferExchange::eBadOffer:
                    throw std::runtime_error("Could not process offer");
                case OfferExchange::ePartial:
                case OfferExchange::eNotEnoughOffers:
                    innerResult().result.code(Payment::OVERSENDMAX);
                    return false;
                }
                assert(curBReceived == actualCurBReceived);
                curBReceived = curASent; // next round, we need to send enough
                curB = curA;
            }
        }
        
        // last step: we've reached the first account in the chain, update its balance

        int64_t curBSent;

        curBSent = curBReceived;

        if (curBSent > mEnvelope.tx.body.paymentTx().sendMax)
        { // make sure not over the max
            innerResult().result.code(Payment::OVERSENDMAX);
            return false;
        }

        if(curB.type() == NATIVE)
        {
            if (mEnvelope.tx.body.paymentTx().path.size())
            {
                innerResult().result.code(Payment::MALFORMED);
                return false;
            }

            int64_t minBalance = ledgerMaster.getMinBalance(mSigningAccount->mEntry.account().ownerCount);

            if (mSigningAccount->mEntry.account().balance < (minBalance + curBSent))
            {   // they don't have enough to send
                innerResult().result.code(Payment::UNDERFUNDED);
                return false;
            }

            mSigningAccount->mEntry.account().balance -= curBSent;
            mSigningAccount->storeChange(delta, db);
        }
        else
        {
            // issuer can always send its own credit
            if(getSourceID() != curB.isoCI().issuer)
            {
                AccountFrame issuer;
                if(!AccountFrame::loadAccount(curB.isoCI().issuer, issuer, db))
                {
                    CLOG(ERROR, "Tx") << "PaymentTx::sendCredit Issuer not found";
                    innerResult().result.code(Payment::MALFORMED);
                    return false;
                }

                TrustFrame sourceLineFrame;
                if(!TrustFrame::loadTrustLine(mEnvelope.tx.account,
                    curB, sourceLineFrame, db))
                {
                    innerResult().result.code(Payment::UNDERFUNDED);
                    return false;
                }

                if(sourceLineFrame.mEntry.trustLine().balance < curBSent)
                {
                    innerResult().result.code(Payment::UNDERFUNDED);
                    return false;
                }
                
                sourceLineFrame.mEntry.trustLine().balance -= curBSent;
                sourceLineFrame.storeChange(delta, db);

            }
        }
        

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

