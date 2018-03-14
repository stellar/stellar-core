// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "transactions/CreateAccountOpFrame.h"
#include "OfferExchange.h"
#include "database/Database.h"
#include "ledger/LedgerHeaderReference.h"
#include "ledger/LedgerState.h"
#include "util/Logging.h"
#include "transactions/TransactionUtils.h"
#include <algorithm>

#include "main/Application.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"

#include "ledger/AccountReference.h"

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
CreateAccountOpFrame::doApply(Application& app, LedgerState& ls)
{
    auto destAccount = stellar::loadAccount(ls, mCreateAccount.destination);
    if (!destAccount)
    {
        auto header = ls.loadHeader();
        if (mCreateAccount.startingBalance < getCurrentMinBalance(header, 0))
        { // not over the minBalance to make an account
            app.getMetrics()
                .NewMeter({"op-create-account", "failure", "low-reserve"},
                          "operation")
                .Mark();
            innerResult().code(CREATE_ACCOUNT_LOW_RESERVE);
            return false;
        }
        else
        {
            auto sourceAccount = loadSourceAccount(ls, header);
            int64_t minBalance =
                sourceAccount.getMinimumBalance(header);

            if ((sourceAccount.account().balance - minBalance) <
                mCreateAccount.startingBalance)
            { // they don't have enough to send
                app.getMetrics()
                    .NewMeter({"op-create-account", "failure", "underfunded"},
                              "operation")
                    .Mark();
                innerResult().code(CREATE_ACCOUNT_UNDERFUNDED);
                return false;
            }

            if (getCurrentLedgerVersion(header) < 8)
            {
                sourceAccount.forget(ls);
                auto thisAccount = stellar::loadAccountRaw(ls, getSourceID());
                if (!thisAccount)
                {
                    throw std::runtime_error("modifying account that does not exist");
                }
                thisAccount->invalidate();
                sourceAccount = loadSourceAccount(ls, header);
            }

            auto ok =
                sourceAccount.addBalance(-mCreateAccount.startingBalance);
            assert(ok);

            LedgerEntry newAccount;
            newAccount.data.type(ACCOUNT);
            newAccount.data.account().thresholds[0] = 1;
            newAccount.data.account().accountID = mCreateAccount.destination;
            newAccount.data.account().seqNum =
                getStartingSequenceNumber(header);
            newAccount.data.account().balance = mCreateAccount.startingBalance;
            destAccount = ls.create(newAccount);

            header->invalidate();

            app.getMetrics()
                .NewMeter({"op-create-account", "success", "apply"},
                          "operation")
                .Mark();
            innerResult().code(CREATE_ACCOUNT_SUCCESS);
            return true;
        }
    }
    else
    {
        app.getMetrics()
            .NewMeter({"op-create-account", "failure", "already-exist"},
                      "operation")
            .Mark();
        innerResult().code(CREATE_ACCOUNT_ALREADY_EXIST);
        return false;
    }
}

bool
CreateAccountOpFrame::doCheckValid(Application& app, uint32_t ledgerVersion)
{
    if (mCreateAccount.startingBalance <= 0)
    {
        app.getMetrics()
            .NewMeter(
                {"op-create-account", "invalid", "malformed-negative-balance"},
                "operation")
            .Mark();
        innerResult().code(CREATE_ACCOUNT_MALFORMED);
        return false;
    }

    if (mCreateAccount.destination == getSourceID())
    {
        app.getMetrics()
            .NewMeter({"op-create-account", "invalid",
                       "malformed-destination-equals-source"},
                      "operation")
            .Mark();
        innerResult().code(CREATE_ACCOUNT_MALFORMED);
        return false;
    }

    return true;
}
}
