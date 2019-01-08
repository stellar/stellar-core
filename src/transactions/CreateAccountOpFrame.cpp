// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "transactions/CreateAccountOpFrame.h"
#include "OfferExchange.h"
#include "database/Database.h"
#include "ledger/LedgerState.h"
#include "ledger/LedgerStateEntry.h"
#include "ledger/LedgerStateHeader.h"
#include "transactions/TransactionUtils.h"
#include "util/Logging.h"
#include "util/XDROperators.h"
#include <algorithm>

#include "main/Application.h"

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
CreateAccountOpFrame::doApply(Application& app, AbstractLedgerState& ls)
{
    if (!stellar::loadAccount(ls, mCreateAccount.destination))
    {
        auto header = ls.loadHeader();
        if (mCreateAccount.startingBalance < getMinBalance(header, 0))
        { // not over the minBalance to make an account
            innerResult().code(CREATE_ACCOUNT_LOW_RESERVE);
            return false;
        }
        else
        {
            bool doesAccountExist = true;
            if (header.current().ledgerVersion < 8)
            {
                LedgerKey key(ACCOUNT);
                key.account().accountID = getSourceID();
                doesAccountExist = (bool)ls.loadWithoutRecord(key);
            }

            auto sourceAccount = loadSourceAccount(ls, header);
            if (getAvailableBalance(header, sourceAccount) <
                mCreateAccount.startingBalance)
            { // they don't have enough to send
                innerResult().code(CREATE_ACCOUNT_UNDERFUNDED);
                return false;
            }

            if (!doesAccountExist)
            {
                throw std::runtime_error(
                    "modifying account that does not exist");
            }

            auto ok = addBalance(header, sourceAccount,
                                 -mCreateAccount.startingBalance);
            assert(ok);

            LedgerEntry newAccountEntry;
            newAccountEntry.data.type(ACCOUNT);
            auto& newAccount = newAccountEntry.data.account();
            newAccount.thresholds[0] = 1;
            newAccount.accountID = mCreateAccount.destination;
            newAccount.seqNum = getStartingSequenceNumber(header);
            newAccount.balance = mCreateAccount.startingBalance;
            ls.create(newAccountEntry);

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
CreateAccountOpFrame::doCheckValid(Application& app, uint32_t ledgerVersion)
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
