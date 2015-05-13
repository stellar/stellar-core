// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/CreateAccountOpFrame.h"
#include "util/Logging.h"
#include "ledger/LedgerDelta.h"
#include "ledger/TrustFrame.h"
#include "ledger/OfferFrame.h"
#include "database/Database.h"
#include "OfferExchange.h"
#include <algorithm>

namespace stellar
{

using namespace std;

CreateAccountOpFrame::CreateAccountOpFrame(Operation const& op,
                                           OperationResult& res,
                                           TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mCreateAccount(mOperation.body.createAccountOp())
{
}

bool
CreateAccountOpFrame::doApply(LedgerDelta& delta, LedgerManager& ledgerManager)
{
    AccountFrame destAccount;

    Database& db = ledgerManager.getDatabase();

    if (!AccountFrame::loadAccount(mCreateAccount.destination, destAccount, db))
    {
        if (mCreateAccount.startingBalance < ledgerManager.getMinBalance(0))
        { // not over the minBalance to make an account
            innerResult().code(CREATE_ACCOUNT_LOW_RESERVE);
            return false;
        }
        else
        {
            int64_t minBalance =
                mSourceAccount->getMinimumBalance(ledgerManager);

            if (mSourceAccount->getAccount().balance <
                (minBalance + mCreateAccount.startingBalance))
            { // they don't have enough to send
                innerResult().code(CREATE_ACCOUNT_UNDERFUNDED);
                return false;
            }

            mSourceAccount->getAccount().balance -=
                mCreateAccount.startingBalance;
            mSourceAccount->storeChange(delta, db);

            destAccount.getAccount().accountID = mCreateAccount.destination;
            destAccount.getAccount().seqNum =
                delta.getHeaderFrame().getStartingSequenceNumber();
            destAccount.getAccount().balance = mCreateAccount.startingBalance;

            destAccount.storeAdd(delta, db);

            innerResult().code(CREATE_ACCOUNT_SUCCESS);
            return true;
        }
    }
    else
    {
        innerResult().code(CREATE_ACCOUNT_ALREADY_EXIST);
        return false;
    }
}

bool
CreateAccountOpFrame::doCheckValid()
{
    if (mCreateAccount.startingBalance <= 0)
    {
        innerResult().code(CREATE_ACCOUNT_MALFORMED);
        return false;
    }

    if (mCreateAccount.destination == getSourceID())
    {
        innerResult().code(CREATE_ACCOUNT_MALFORMED);
        return false;
    }

    return true;
}
}
