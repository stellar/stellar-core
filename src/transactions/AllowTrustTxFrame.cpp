#include "ledger/LedgerMaster.h"
#include "transactions/AllowTrustTxFrame.h"

namespace stellar
{
    AllowTrustTxFrame::AllowTrustTxFrame(const TransactionEnvelope& envelope) : TransactionFrame(envelope)
    {

    }

    void AllowTrustTxFrame::doApply(TxDelta& delta, LedgerMaster& ledgerMaster)
    {
        if(!(mSigningAccount.mEntry.account().flags & AccountFrame::AUTH_REQUIRED_FLAG))
        {   // this account doesn't require authorization to hold credit
            mResultCode = txNOT_AUTHORIZED;
            return;
        }

        CurrencyIssuer ci;
        ci.currencyCode = mEnvelope.tx.body.allowTrustTx().currencyCode;
        ci.issuer = mEnvelope.tx.account;

        TrustFrame trustLine;
        if(!ledgerMaster.getDatabase().loadTrustLine(mEnvelope.tx.body.allowTrustTx().trustor, ci, trustLine))
        {
            mResultCode = txNOTRUST;
            return;
        }

        mResultCode = txSUCCESS;
        delta.setStart(trustLine);
        trustLine.mEntry.trustLine().authorized = mEnvelope.tx.body.allowTrustTx().authorize;
        delta.setFinal(trustLine);
    }

    bool AllowTrustTxFrame::doCheckValid(Application& app)
    {
        return false;
    }
}