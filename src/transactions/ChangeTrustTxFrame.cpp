// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC


#include "ChangeTrustTxFrame.h"
#include "ledger/TrustFrame.h"
#include "ledger/LedgerMaster.h"
#include "database/Database.h"

namespace stellar
{ 

ChangeTrustTxFrame::ChangeTrustTxFrame(const TransactionEnvelope& envelope) : TransactionFrame(envelope)
{

}
bool ChangeTrustTxFrame::doApply(LedgerDelta& delta, LedgerMaster& ledgerMaster)
{
    TrustFrame trustLine;
    Database &db = ledgerMaster.getDatabase();

    if(TrustFrame::loadTrustLine(mSigningAccount->mEntry.account().accountID,
        mEnvelope.tx.body.changeTrustTx().line, trustLine, db))
    { // we are modifying an old trustline
        trustLine.mEntry.trustLine().limit= mEnvelope.tx.body.changeTrustTx().limit;
        if(trustLine.mEntry.trustLine().limit == 0 &&
            trustLine.mEntry.trustLine().balance == 0)
        {
            // line gets deleted
            mSigningAccount->mEntry.account().ownerCount--;
            trustLine.storeDelete(delta, db);
            mSigningAccount->storeChange(delta, db);
        }
        else
        {
            trustLine.storeChange(delta, db);
        }
        innerResult().result.code(ChangeTrust::SUCCESS);
        return true;
    } else
    { // new trust line
        AccountFrame issuer;
        if(!AccountFrame::loadAccount(mEnvelope.tx.body.changeTrustTx().line.isoCI().issuer,
            issuer, db))
        {
            innerResult().result.code(ChangeTrust::NO_ACCOUNT);
            return false;
        }
            
        trustLine.mEntry.type(TRUSTLINE);
        trustLine.mEntry.trustLine().accountID = mSigningAccount->mEntry.account().accountID;
        trustLine.mEntry.trustLine().currency = mEnvelope.tx.body.changeTrustTx().line;
        trustLine.mEntry.trustLine().limit = mEnvelope.tx.body.changeTrustTx().limit;
        trustLine.mEntry.trustLine().balance = 0;
        trustLine.mEntry.trustLine().authorized = !issuer.isAuthRequired();

        mSigningAccount->mEntry.account().ownerCount++;

        mSigningAccount->storeChange(delta, db);
        trustLine.storeAdd(delta, db);

        innerResult().result.code(ChangeTrust::SUCCESS);
        return true;
    }
}

bool ChangeTrustTxFrame::doCheckValid(Application& app)
{
    return true;
}

}

