// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "transactions/TxResultCode.h"
#include "ChangeTrustTxFrame.h"
#include "ledger/TrustFrame.h"
#include "ledger/LedgerMaster.h"

namespace stellar
{ 

ChangeTrustTxFrame::ChangeTrustTxFrame(const TransactionEnvelope& envelope) : TransactionFrame(envelope)
{

}
bool ChangeTrustTxFrame::doApply(TxDelta& delta, LedgerMaster& ledgerMaster)
{
    TrustFrame trustLine;
        
    if(ledgerMaster.getDatabase().loadTrustLine(mSigningAccount.mEntry.account().accountID,
        mEnvelope.tx.body.changeTrustTx().line, trustLine))
    { // we are modifying an old trustline
        delta.setStart(trustLine);

        trustLine.mEntry.trustLine().limit= mEnvelope.tx.body.changeTrustTx().limit;
        if(trustLine.mEntry.trustLine().limit == 0 &&
            trustLine.mEntry.trustLine().balance == 0)
        {
            mSigningAccount.mEntry.account().ownerCount--;
            delta.setFinal(mSigningAccount);
            //delete this line by not adding it to setFinal
        }else delta.setFinal(trustLine);
        mResultCode = txSUCCESS;
        return true;
    } else
    { // new trust line
        AccountFrame issuer;
        if(!ledgerMaster.getDatabase().loadAccount(mEnvelope.tx.body.changeTrustTx().line.isoCI().issuer, issuer))
        {
            mResultCode = txNOACCOUNT;
            return false;
        }
            
        trustLine.mEntry.type(TRUSTLINE);
        trustLine.mEntry.trustLine().accountID = mSigningAccount.mEntry.account().accountID;
        trustLine.mEntry.trustLine().currency = mEnvelope.tx.body.changeTrustTx().line;
        trustLine.mEntry.trustLine().limit = mEnvelope.tx.body.changeTrustTx().limit;
        trustLine.mEntry.trustLine().balance = 0;
        trustLine.mEntry.trustLine().authorized = !issuer.isAuthRequired();

        mSigningAccount.mEntry.account().ownerCount++;
        delta.setFinal(mSigningAccount);
        delta.setFinal(trustLine);
        mResultCode = txSUCCESS;
        return true;
    }
}

bool ChangeTrustTxFrame::doCheckValid(Application& app)
{
    return true;
}

   
}

