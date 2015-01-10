#include "transactions/SetOptionsFrame.h"
#include "crypto/Base58.h"

// TODO.2 Handle all SQL exceptions
namespace stellar
{
    SetOptionsFrame::SetOptionsFrame(const TransactionEnvelope& envelope) : TransactionFrame(envelope)
    {

    }

    int32_t SetOptionsFrame::getNeededThreshold()
    {
        // threshold depends on 
        if(mEnvelope.tx.body.setOptionsTx().thresholds ||
            mEnvelope.tx.body.setOptionsTx().signer) return mSigningAccount.getHighThreshold();
        return mSigningAccount.getMidThreshold();
    }

    // make sure it doesn't allow us to add signers when we don't have the minbalance
    bool SetOptionsFrame::doApply(TxDelta& delta, LedgerMaster& ledgerMaster)
    {
        if(mEnvelope.tx.body.setOptionsTx().inflationDest)
        {
            mSigningAccount.mEntry.account().inflationDest.activate()=*mEnvelope.tx.body.setOptionsTx().inflationDest;
        }
        if(mEnvelope.tx.body.setOptionsTx().transferRate)
        {
            // make sure no one holds your credit
            int64_t b=0;
            std::string base58ID = toBase58Check(VER_ACCOUNT_ID, mSigningAccount.mEntry.account().accountID);
            ledgerMaster.getDatabase().getSession() <<
                "SELECT balance from TrustLines where issuer=:v1 and balance>0 limit 1",
                soci::into(b), soci::use(base58ID);
            if(b)
            {
                mResultCode = txRATE_FIXED;
                return false;
            }
            mSigningAccount.mEntry.account().transferRate = *mEnvelope.tx.body.setOptionsTx().transferRate;
        }
        if(mEnvelope.tx.body.setOptionsTx().flags)
        {   
            mSigningAccount.mEntry.account().flags = *mEnvelope.tx.body.setOptionsTx().flags;
        }
        /* TODO.2
        if(mEnvelope.tx.body.setOptionsTx().thresholds)
        {
            mSigningAccount.mEntry.account().thresholds = *mEnvelope.tx.body.setOptionsTx().thresholds;
        }
        */
        if(mEnvelope.tx.body.setOptionsTx().signer)
        {
            xdr::xvector<Signer>& signers = mSigningAccount.mEntry.account().signers;
            if(mEnvelope.tx.body.setOptionsTx().signer->weight)
            { // add or change signer
                bool found = false;
                for(auto oldSigner : signers)
                {
                    if(oldSigner.pubKey == mEnvelope.tx.body.setOptionsTx().signer->pubKey)
                    {
                        oldSigner.weight = mEnvelope.tx.body.setOptionsTx().signer->weight;
                    }
                }
                if(!found)
                {
                    if( mSigningAccount.mEntry.account().balance < 
                        ledgerMaster.getMinBalance(mSigningAccount.mEntry.account().ownerCount + 1))
                    {
                        mResultCode = txBELOW_MIN_BALANCE;
                        return false;
                    }
                    mSigningAccount.mEntry.account().ownerCount++;
                    signers.push_back(*mEnvelope.tx.body.setOptionsTx().signer);
                }
            } else
            { // delete signer
                for(auto it = signers.begin(); it != signers.end(); it++)
                {
                    Signer& oldSigner = *it;
                    if(oldSigner.pubKey == mEnvelope.tx.body.setOptionsTx().signer->pubKey)
                    {
                        signers.erase(it);
                        mSigningAccount.mEntry.account().ownerCount--;
                    }
                }
            }
        }
        
        mResultCode = txSUCCESS;
        delta.setFinal(mSigningAccount);
        return true;
    }

    bool SetOptionsFrame::doCheckValid(Application& app)
    {
        // transfer rate can't be greater than 1
        if(mEnvelope.tx.body.setOptionsTx().transferRate)
        {
            if(*mEnvelope.tx.body.setOptionsTx().transferRate > TRANSFER_RATE_DIVISOR)
                return false;
        }
        return true;
    }
}
