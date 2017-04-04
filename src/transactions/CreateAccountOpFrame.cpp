// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "transactions/CreateAccountOpFrame.h"
#include "OfferExchange.h"
#include "database/Database.h"
#include "ledger/LedgerDelta.h"
#include "ledger/OfferFrame.h"
#include "ledger/TrustFrame.h"
#include "util/Logging.h"
#include <algorithm>

#include "main/Application.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"

namespace stellar
{

using namespace std;
using xdr::operator==;

namespace
{

OperationResult
makeResult(CreateAccountResultCode code)
{
    auto result = OperationResult{};
    result.code(opINNER);
    result.tr().type(CREATE_ACCOUNT);
    result.tr().createAccountResult().code(code);
    return result;
}
}

CreateAccountOpFrame::CreateAccountOpFrame(Operation const& op,
                                           TransactionFrame& parentTx)
    : OperationFrame(op, parentTx)
    , mCreateAccount(mOperation.body.createAccountOp())
{
}

OperationResult
CreateAccountOpFrame::doApply(Application& app, LedgerDelta& delta,
                              LedgerManager& ledgerManager)
{
    AccountFrame::pointer destAccount;

    Database& db = ledgerManager.getDatabase();

    destAccount =
        AccountFrame::loadAccount(delta, mCreateAccount.destination, db);
    if (!destAccount)
    {
        if (mCreateAccount.startingBalance < ledgerManager.getMinBalance(0))
        { // not over the minBalance to make an account
            app.getMetrics()
                .NewMeter({"op-create-account", "failure", "low-reserve"},
                          "operation")
                .Mark();
            return makeResult(CREATE_ACCOUNT_LOW_RESERVE);
        }
        else
        {
            int64_t minBalance =
                mSourceAccount->getMinimumBalance(ledgerManager);

            if ((mSourceAccount->getAccount().balance - minBalance) <
                mCreateAccount.startingBalance)
            { // they don't have enough to send
                app.getMetrics()
                    .NewMeter({"op-create-account", "failure", "underfunded"},
                              "operation")
                    .Mark();
                return makeResult(CREATE_ACCOUNT_UNDERFUNDED);
            }

            auto ok =
                mSourceAccount->addBalance(-mCreateAccount.startingBalance);
            assert(ok);

            mSourceAccount->storeChange(delta, db);

            destAccount = make_shared<AccountFrame>(mCreateAccount.destination);
            destAccount->getAccount().seqNum =
                delta.getHeaderFrame().getStartingSequenceNumber();
            destAccount->getAccount().balance = mCreateAccount.startingBalance;

            destAccount->storeAdd(delta, db);

            app.getMetrics()
                .NewMeter({"op-create-account", "success", "apply"},
                          "operation")
                .Mark();
            return makeResult(CREATE_ACCOUNT_SUCCESS);
        }
    }
    else
    {
        app.getMetrics()
            .NewMeter({"op-create-account", "failure", "already-exist"},
                      "operation")
            .Mark();
        return makeResult(CREATE_ACCOUNT_ALREADY_EXIST);
    }
}

OperationResult
CreateAccountOpFrame::doCheckValid(Application& app)
{
    if (mCreateAccount.startingBalance <= 0)
    {
        app.getMetrics()
            .NewMeter(
                {"op-create-account", "invalid", "malformed-negative-balance"},
                "operation")
            .Mark();
        return makeResult(CREATE_ACCOUNT_MALFORMED);
    }

    if (mCreateAccount.destination == getSourceID())
    {
        app.getMetrics()
            .NewMeter({"op-create-account", "invalid",
                       "malformed-destination-equals-source"},
                      "operation")
            .Mark();
        return makeResult(CREATE_ACCOUNT_MALFORMED);
    }

    return makeResult(CREATE_ACCOUNT_SUCCESS);
}
}
