#include "transactions/SetOptionsFrame.h"

// TODO.2 Handle all SQL exceptions
namespace stellar
{
    void SetOptionsFrame::doApply(TxDelta& delta, LedgerMaster& ledgerMaster)
    {
        if(mEnvelope.tx.body.setOptionsTx().creditAuthKey)
        {
            mSigningAccount.mEntry.account().creditAuthKey.activate()=*mEnvelope.tx.body.setOptionsTx().creditAuthKey;
        }
        if(mEnvelope.tx.body.setOptionsTx().pubKey)
        {
            mSigningAccount.mEntry.account().pubKey.activate() = *mEnvelope.tx.body.setOptionsTx().pubKey;
        }
        if(mEnvelope.tx.body.setOptionsTx().inflationDest)
        {
            mSigningAccount.mEntry.account().inflationDest.activate()=*mEnvelope.tx.body.setOptionsTx().inflationDest;
        }
        if(mEnvelope.tx.body.setOptionsTx().transferRate)
        {
            // make sure no one holds your credit
            int64_t b=0;
            std::string base58ID;
            toBase58(mSigningAccount.mEntry.account().accountID, base58ID);
            ledgerMaster.getDatabase().getSession() <<
                "SELECT balance from TrustLines where issuer=:v1 and balance>0 limit 1",
                soci::into(b), soci::use(base58ID);
            if(b)
            {
                mResultCode = txRATE_FIXED;
                return;
            }
            mSigningAccount.mEntry.account().transferRate = *mEnvelope.tx.body.setOptionsTx().transferRate;
        }
        if(mEnvelope.tx.body.setOptionsTx().flags)
        {   
            mSigningAccount.mEntry.account().flags = *mEnvelope.tx.body.setOptionsTx().flags;
        }
        
        mResultCode = txSUCCESS;
        delta.setFinal(mSigningAccount);
    }
}