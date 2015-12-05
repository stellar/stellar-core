// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "transactions/CreateAccountOpFrame.h"
#include "util/Logging.h"
#include "ledger/LedgerDelta.h"
#include "ledger/TrustFrame.h"
#include "ledger/OfferFrame.h"
#include "database/Database.h"
#include "OfferExchange.h"
#include <algorithm>

#include "medida/meter.h"
#include "medida/metrics_registry.h"

namespace stellar
{

using namespace std;
using xdr::operator==;

CreateAccountOpFrame::CreateAccountOpFrame(Operation const& op,
                                           OperationResult& res,
                                           TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mCreateAccount(mOperation.body.createAccountOp())
{
}

bool
CreateAccountOpFrame::doApply(medida::MetricsRegistry& metrics,
                              LedgerDelta& delta, LedgerManager& ledgerManager)
{
    AccountFrame::pointer destAccount;

    Database& db = ledgerManager.getDatabase();

    destAccount =
        AccountFrame::loadAccount(delta, mCreateAccount.destination, db);
    if (!destAccount)
    {
        if (mCreateAccount.startingBalance < ledgerManager.getMinBalance(0))
        { // not over the minBalance to make an account
            metrics.NewMeter({"op-create-account", "failure", "low-reserve"},
                             "operation").Mark();
            innerResult().code(CREATE_ACCOUNT_LOW_RESERVE);
            return false;
        }
        else
        {
            int64_t minBalance =
                mSourceAccount->getMinimumBalance(ledgerManager);

            if ((mSourceAccount->getAccount().balance - minBalance) <
                mCreateAccount.startingBalance)
            { // they don't have enough to send
                metrics.NewMeter(
                            {"op-create-account", "failure", "underfunded"},
                            "operation").Mark();
                innerResult().code(CREATE_ACCOUNT_UNDERFUNDED);
                return false;
            }

            mSourceAccount->getAccount().balance -=
                mCreateAccount.startingBalance;
            mSourceAccount->storeChange(delta, db);

            destAccount = make_shared<AccountFrame>(mCreateAccount.destination);
            destAccount->getAccount().seqNum =
                delta.getHeaderFrame().getStartingSequenceNumber();
            destAccount->getAccount().balance = mCreateAccount.startingBalance;

            destAccount->storeAdd(delta, db);

            metrics.NewMeter({"op-create-account", "success", "apply"},
                             "operation").Mark();
            innerResult().code(CREATE_ACCOUNT_SUCCESS);
            return true;
        }
    }
    else
    {
        metrics.NewMeter({"op-create-account", "failure", "already-exist"},
                         "operation").Mark();
        innerResult().code(CREATE_ACCOUNT_ALREADY_EXIST);
        return false;
    }
}

bool
CreateAccountOpFrame::doCheckValid(medida::MetricsRegistry& metrics)
{
    if (mCreateAccount.startingBalance <= 0)
    {
        metrics.NewMeter({"op-create-account", "invalid",
                          "malformed-negative-balance"},
                         "operation").Mark();
        innerResult().code(CREATE_ACCOUNT_MALFORMED);
        return false;
    }

    if (mCreateAccount.destination == getSourceID())
    {
        metrics.NewMeter({"op-create-account", "invalid",
                          "malformed-destination-equals-source"},
                         "operation").Mark();
        innerResult().code(CREATE_ACCOUNT_MALFORMED);
        return false;
    }

    return true;
}
}
