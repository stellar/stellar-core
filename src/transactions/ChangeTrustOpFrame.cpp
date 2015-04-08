// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ChangeTrustOpFrame.h"
#include "ledger/TrustFrame.h"
#include "ledger/LedgerManager.h"
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
ChangeTrustOpFrame::doApply(LedgerDelta& delta, LedgerManager& ledgerManager)
{
    TrustFrame trustLine;
    Database& db = ledgerManager.getDatabase();

    if (TrustFrame::loadTrustLine(getSourceID(), mChangeTrust.line, trustLine,
                                  db))
    { // we are modifying an old trustline
        
        if (mChangeTrust.limit < 0 ||
            mChangeTrust.limit < trustLine.getBalance())
        { // Can't drop the limit below the balance you are holding with them
            innerResult().code(CHANGE_TRUST_INVALID_LIMIT);
            return false;
        }

        trustLine.getTrustLine().limit = mChangeTrust.limit;
        if (trustLine.getTrustLine().limit == 0 &&
            trustLine.getBalance() == 0)
        {
            // line gets deleted
            mSourceAccount->addNumEntries(-1, ledgerManager);
            trustLine.storeDelete(delta, db);
            mSourceAccount->storeChange(delta, db);
        }
        else
        {
            trustLine.storeChange(delta, db);
        }
        innerResult().code(CHANGE_TRUST_SUCCESS);
        return true;
    }
    else
    { // new trust line
        AccountFrame issuer;
        if (!AccountFrame::loadAccount(mChangeTrust.line.isoCI().issuer, issuer,
                                       db))
        {
            innerResult().code(CHANGE_TRUST_NO_ISSUER);
            return false;
        }

        trustLine.getTrustLine().accountID = getSourceID();
        trustLine.getTrustLine().currency = mChangeTrust.line;
        trustLine.getTrustLine().limit = mChangeTrust.limit;
        trustLine.getTrustLine().balance = 0;
        trustLine.getTrustLine().authorized = !issuer.isAuthRequired();

        if (!mSourceAccount->addNumEntries(1, ledgerManager))
        {
            innerResult().code(CHANGE_TRUST_LOW_RESERVE);
        }

        mSourceAccount->storeChange(delta, db);
        trustLine.storeAdd(delta, db);

        innerResult().code(CHANGE_TRUST_SUCCESS);
        return true;
    }
}

bool
ChangeTrustOpFrame::doCheckValid(Application& app)
{
    if (mChangeTrust.limit < 0)
    {
        innerResult().code(CHANGE_TRUST_MALFORMED);
        return false;
    }
    return true;
}
}
