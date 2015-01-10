#include "ledger/LedgerMaster.h"
#include "transactions/AllowTrustTxFrame.h"
#include "ledger/TrustFrame.h"
#include "database/Database.h"

namespace stellar
{
    AllowTrustTxFrame::AllowTrustTxFrame(const TransactionEnvelope& envelope) : TransactionFrame(envelope)
    {

    }

    int32_t AllowTrustTxFrame::getNeededThreshold()
    {
        return mSigningAccount.getLowThreshold();
    }

    bool AllowTrustTxFrame::doApply(TxDelta& delta, LedgerMaster& ledgerMaster)
    {
        if(!(mSigningAccount.mEntry.account().flags & AccountFrame::AUTH_REQUIRED_FLAG))
        {   // this account doesn't require authorization to hold credit
            mResultCode = txNOT_AUTHORIZED;
            return false;
        }

        Currency ci;
        ci.type(ISO4217);
        ci.isoCI().currencyCode = mEnvelope.tx.body.allowTrustTx().code.currencyCode();
        ci.isoCI().issuer=mEnvelope.tx.account;

        TrustFrame trustLine;
        if(!ledgerMaster.getDatabase().loadTrustLine(mEnvelope.tx.body.allowTrustTx().trustor, ci, trustLine))
        {
            mResultCode = txNOTRUST;
            return false;
        }

        mResultCode = txSUCCESS;
        delta.setStart(trustLine);
        trustLine.mEntry.trustLine().authorized = mEnvelope.tx.body.allowTrustTx().authorize;
        delta.setFinal(trustLine);
        return true;
    }

    bool AllowTrustTxFrame::doCheckValid(Application& app)
    {
        if(mEnvelope.tx.body.allowTrustTx().code.type() != ISO4217) return false;
        return true;
    }
}