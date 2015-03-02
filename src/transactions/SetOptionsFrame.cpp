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
        SetOptionsTx const& options = mEnvelope.tx.body.setOptionsTx();
        // updating thresholds or signer requires high threshold
        if (options.thresholds || options.signer)
        {
            return mSigningAccount->getHighThreshold();
        }
        return mSigningAccount->getMidThreshold();
    }

    // make sure it doesn't allow us to add signers when we don't have the minbalance
    bool SetOptionsFrame::doApply(LedgerDelta& delta, LedgerMaster& ledgerMaster)
    {
        Database &db = ledgerMaster.getDatabase();
        SetOptionsTx const& options = mEnvelope.tx.body.setOptionsTx();
        if(options.inflationDest)
        {
            mSigningAccount->getAccount().inflationDest.activate()=*options.inflationDest;
        }

        if (options.clearFlags)
        {
            mSigningAccount->getAccount().flags = mSigningAccount->getAccount().flags & ~*options.clearFlags;
        }
        if(options.setFlags)
        {   
            mSigningAccount->getAccount().flags = mSigningAccount->getAccount().flags | *options.setFlags;
        }
        
        if(options.thresholds)
        {
            mSigningAccount->getAccount().thresholds = *options.thresholds;
        }
        
        if(options.signer)
        {
            xdr::xvector<Signer>& signers = mSigningAccount->getAccount().signers;
            if(options.signer->weight)
            { // add or change signer
                bool found = false;
                for(auto oldSigner : signers)
                {
                    if(oldSigner.pubKey == options.signer->pubKey)
                    {
                        oldSigner.weight = options.signer->weight;
                    }
                }
                if(!found)
                {
                    if( mSigningAccount->getAccount().balance < 
                        ledgerMaster.getMinBalance(mSigningAccount->getAccount().numSubEntries + 1))
                    {
                        innerResult().code(SetOptions::BELOW_MIN_BALANCE);
                        return false;
                    }
                    mSigningAccount->getAccount().numSubEntries++;
                    signers.push_back(*options.signer);
                }
            } else
            { // delete signer
                auto it = signers.begin();
                while (it != signers.end())
                {
                    Signer& oldSigner = *it;
                    if(oldSigner.pubKey == options.signer->pubKey)
                    {
                        it = signers.erase(it);
                        mSigningAccount->getAccount().numSubEntries--;
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
        SetOptionsTx const& options = mEnvelope.tx.body.setOptionsTx();
        if (options.setFlags && options.clearFlags)
        {
            if ((*options.setFlags & *options.clearFlags) != 0)
            {
                innerResult().code(SetOptions::MALFORMED);
                return false;
            }
        }
        return true;
    }
}
