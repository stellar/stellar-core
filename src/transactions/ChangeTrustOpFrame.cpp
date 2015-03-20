// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "ChangeTrustOpFrame.h"
#include "ledger/TrustFrame.h"
#include "ledger/LedgerMaster.h"
#include "database/Database.h"

namespace stellar
{

ChangeTrustOpFrame::ChangeTrustOpFrame(Operation const& op,
                                       OperationResult& res,
                                       TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mChangeTrust(mOperation.body.changeTrustOp())
{
}
bool
ChangeTrustOpFrame::doApply(LedgerDelta& delta, LedgerMaster& ledgerMaster)
{
    TrustFrame trustLine;
    Database& db = ledgerMaster.getDatabase();

    if (TrustFrame::loadTrustLine(getSourceID(), mChangeTrust.line, trustLine,
                                  db))
    { // we are modifying an old trustline
        
        if( mChangeTrust.limit < 0 || 
            mChangeTrust.limit < trustLine.getTrustLine().balance)
        { // Can't drop the limit below the balance you are holding with them
            innerResult().code(ChangeTrust::INVALID_LIMIT);
            return false;
        }

        trustLine.getTrustLine().limit = mChangeTrust.limit;
        if (trustLine.getTrustLine().limit == 0 &&
            trustLine.getTrustLine().balance == 0)
        {
            // line gets deleted
            mSourceAccount->getAccount().numSubEntries--;
            trustLine.storeDelete(delta, db);
            mSourceAccount->storeChange(delta, db);
        }
        else
        {
            trustLine.storeChange(delta, db);
        }
        innerResult().code(ChangeTrust::SUCCESS);
        return true;
    }
    else
    { // new trust line
        AccountFrame issuer;
        if (!AccountFrame::loadAccount(mChangeTrust.line.isoCI().issuer, issuer,
                                       db))
        {
            innerResult().code(ChangeTrust::NO_ACCOUNT);
            return false;
        }

        trustLine.getTrustLine().accountID = getSourceID();
        trustLine.getTrustLine().currency = mChangeTrust.line;
        trustLine.getTrustLine().limit = mChangeTrust.limit;
        trustLine.getTrustLine().balance = 0;
        trustLine.getTrustLine().authorized = !issuer.isAuthRequired();

        mSourceAccount->getAccount().numSubEntries++;

        mSourceAccount->storeChange(delta, db);
        trustLine.storeAdd(delta, db);

        innerResult().code(ChangeTrust::SUCCESS);
        return true;
    }
}

bool
ChangeTrustOpFrame::doCheckValid(Application& app)
{
    return true;
}
}
