#include "transactions/SetOptionsFrame.h"
#include "crypto/Base58.h"
#include "database/Database.h"

// TODO.2 Handle all SQL exceptions
namespace stellar
{
    SetOptionsFrame::SetOptionsFrame(const TransactionEnvelope& envelope) :
        TransactionFrame(envelope)
    {

    }

    int32_t SetOptionsFrame::getNeededThreshold()
    {
        // updating thresholds or signer requires high threshold
        if(mEnvelope.tx.body.setOptionsTx().thresholds ||
            mEnvelope.tx.body.setOptionsTx().signer)
            return mSigningAccount->getHighThreshold();
        return mSigningAccount->getMidThreshold();
    }

    // make sure it doesn't allow us to add signers when we don't have the minbalance
    bool SetOptionsFrame::doApply(LedgerDelta& delta, LedgerMaster& ledgerMaster)
    {
        Database &db = ledgerMaster.getDatabase();
        if(mEnvelope.tx.body.setOptionsTx().inflationDest)
        {
            mSigningAccount->getAccount().inflationDest.activate()=*mEnvelope.tx.body.setOptionsTx().inflationDest;
        }

        if (mEnvelope.tx.body.setOptionsTx().clearFlags)
        {
            mSigningAccount->getAccount().flags = mSigningAccount->getAccount().flags & ~*mEnvelope.tx.body.setOptionsTx().clearFlags;
        }
        if(mEnvelope.tx.body.setOptionsTx().setFlags)
        {   
            mSigningAccount->getAccount().flags = mSigningAccount->getAccount().flags | *mEnvelope.tx.body.setOptionsTx().setFlags;
        }
        
        if(mEnvelope.tx.body.setOptionsTx().thresholds)
        {
            mSigningAccount->getAccount().thresholds = *mEnvelope.tx.body.setOptionsTx().thresholds;
        }
        
        if(mEnvelope.tx.body.setOptionsTx().signer)
        {
            xdr::xvector<Signer>& signers = mSigningAccount->getAccount().signers;
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
                    if( mSigningAccount->getAccount().balance < 
                        ledgerMaster.getMinBalance(mSigningAccount->getAccount().ownerCount + 1))
                    {
                        innerResult().code(SetOptions::BELOW_MIN_BALANCE);
                        return false;
                    }
                    mSigningAccount->getAccount().ownerCount++;
                    signers.push_back(*mEnvelope.tx.body.setOptionsTx().signer);
                }
            } else
            { // delete signer
                auto it = signers.begin();
                while (it != signers.end())
                {
                    Signer& oldSigner = *it;
                    if(oldSigner.pubKey == mEnvelope.tx.body.setOptionsTx().signer->pubKey)
                    {
                        it = signers.erase(it);
                        mSigningAccount->getAccount().ownerCount--;
                    }
                    else
                    {
                        it++;
                    }
                }
            }
            mSigningAccount->setUpdateSigners();
        }
        
        innerResult().code(SetOptions::SUCCESS);
        mSigningAccount->storeChange(delta, db);
        return true;
    }

    bool SetOptionsFrame::doCheckValid(Application& app)
    {
        if (mEnvelope.tx.body.setOptionsTx().setFlags && mEnvelope.tx.body.setOptionsTx().clearFlags)
        {
            if ((*mEnvelope.tx.body.setOptionsTx().setFlags & *mEnvelope.tx.body.setOptionsTx().clearFlags) != 0)
            {
                innerResult().code(SetOptions::MALFORMED);
                return false;
            }
        }
        return true;
    }
}
