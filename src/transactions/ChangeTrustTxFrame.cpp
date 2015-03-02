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

    ChangeTrustTx const& changeTrust = mEnvelope.tx.body.changeTrustTx();

    if(TrustFrame::loadTrustLine(mSigningAccount->getAccount().accountID,
        changeTrust.line, trustLine, db))
    { // we are modifying an old trustline
        trustLine.getTrustLine().limit= changeTrust.limit;
        if(trustLine.getTrustLine().limit == 0 &&
            trustLine.getTrustLine().balance == 0)
        {
            // line gets deleted
            mSigningAccount->getAccount().numSubEntries--;
            trustLine.storeDelete(delta, db);
            mSigningAccount->storeChange(delta, db);
        }
        else
        {
            trustLine.storeChange(delta, db);
        }
        innerResult().code(ChangeTrust::SUCCESS);
        return true;
    } else
    { // new trust line
        AccountFrame issuer;
        if(!AccountFrame::loadAccount(changeTrust.line.isoCI().issuer, issuer, db))
        {
            innerResult().code(ChangeTrust::NO_ACCOUNT);
            return false;
        }
            
        trustLine.getTrustLine().accountID = mSigningAccount->getAccount().accountID;
        trustLine.getTrustLine().currency = changeTrust.line;
        trustLine.getTrustLine().limit = changeTrust.limit;
        trustLine.getTrustLine().balance = 0;
        trustLine.getTrustLine().authorized = !issuer.isAuthRequired();

        mSigningAccount->getAccount().numSubEntries++;

        mSigningAccount->storeChange(delta, db);
        trustLine.storeAdd(delta, db);

        innerResult().code(ChangeTrust::SUCCESS);
        return true;
    }
}

bool ChangeTrustTxFrame::doCheckValid(Application& app)
{
    return true;
}

}

