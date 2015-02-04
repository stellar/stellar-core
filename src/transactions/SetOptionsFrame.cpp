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
            mSigningAccount->mEntry.account().inflationDest.activate()=*mEnvelope.tx.body.setOptionsTx().inflationDest;
        }

        if (mEnvelope.tx.body.setOptionsTx().clearFlags)
        {
            mSigningAccount->mEntry.account().flags = mSigningAccount->mEntry.account().flags & ~*mEnvelope.tx.body.setOptionsTx().clearFlags;
        }
        if(mEnvelope.tx.body.setOptionsTx().setFlags)
        {   
            mSigningAccount->mEntry.account().flags = mSigningAccount->mEntry.account().flags | *mEnvelope.tx.body.setOptionsTx().setFlags;
        }
        
        if(mEnvelope.tx.body.setOptionsTx().thresholds)
        {
            mSigningAccount->mEntry.account().thresholds = *mEnvelope.tx.body.setOptionsTx().thresholds;
        }
        
        if(mEnvelope.tx.body.setOptionsTx().signer)
        {
            xdr::xvector<Signer>& signers = mSigningAccount->mEntry.account().signers;
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
                    if( mSigningAccount->mEntry.account().balance < 
                        ledgerMaster.getMinBalance(mSigningAccount->mEntry.account().ownerCount + 1))
                    {
                        innerResult().result.code(SetOptions::BELOW_MIN_BALANCE);
                        return false;
                    }
                    mSigningAccount->mEntry.account().ownerCount++;
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
                        mSigningAccount->mEntry.account().ownerCount--;
                    }
                    else
                    {
                        it++;
                    }
                }
            }
            mSigningAccount->setUpdateSigners();
        }
        
        innerResult().result.code(SetOptions::SUCCESS);
        mSigningAccount->storeChange(delta, db);
        return true;
    }

    bool SetOptionsFrame::doCheckValid(Application& app)
    {
        if (mEnvelope.tx.body.setOptionsTx().setFlags && mEnvelope.tx.body.setOptionsTx().clearFlags)
        {
            if ((*mEnvelope.tx.body.setOptionsTx().setFlags & *mEnvelope.tx.body.setOptionsTx().clearFlags) != 0)
            {
                innerResult().result.code(SetOptions::MALFORMED);
                return false;
            }
        }
        return true;
    }
}
