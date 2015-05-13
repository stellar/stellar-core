// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/PaymentOpFrame.h"
#include "transactions/PathPaymentOpFrame.h"
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

PaymentOpFrame::PaymentOpFrame(Operation const& op, OperationResult& res,
                               TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx), mPayment(mOperation.body.paymentOp())
{
}

bool
PaymentOpFrame::doApply(LedgerDelta& delta, LedgerManager& ledgerManager)
{
    AccountFrame destination;

    // if sending to self directly, just mark as success
    if (mPayment.destination == getSourceID())
    {
        innerResult().code(PAYMENT_SUCCESS);
        return true;
    }

    Database& db = ledgerManager.getDatabase();

    if (!AccountFrame::loadAccount(mPayment.destination, destination, db))
    {
        innerResult().code(PAYMENT_NO_DESTINATION);
        return false;
    }

    // build a pathPaymentOp
    Operation op;
    op.sourceAccount = mOperation.sourceAccount;
    op.body.type(PATH_PAYMENT);
    PathPaymentOp& ppOp = op.body.pathPaymentOp();
    ppOp.sendCurrency = mPayment.currency;
    ppOp.destCurrency = mPayment.currency;

    ppOp.destAmount = mPayment.amount;
    ppOp.sendMax = mPayment.amount;

    ppOp.destination = mPayment.destination;

    OperationResult opRes;
    opRes.code(opINNER);
    opRes.tr().type(PATH_PAYMENT);
    PathPaymentOpFrame ppayment(op, opRes, mParentTx);
    ppayment.setSourceAccountPtr(mSourceAccount);

    if (!ppayment.doCheckValid() || !ppayment.doApply(delta, ledgerManager))
    {
        if (ppayment.getResultCode() != opINNER)
        {
            throw std::runtime_error("Unexpected error code from pathPayment");
        }
        PaymentResultCode res;

        switch (PathPaymentOpFrame::getInnerCode(ppayment.getResult()))
        {
        case PATH_PAYMENT_UNDERFUNDED:
            res = PAYMENT_UNDERFUNDED;
            break;
        case PATH_PAYMENT_NO_DESTINATION:
            res = PAYMENT_NO_DESTINATION;
            break;
        case PATH_PAYMENT_NO_TRUST:
            res = PAYMENT_NO_TRUST;
            break;
        case PATH_PAYMENT_NOT_AUTHORIZED:
            res = PAYMENT_NOT_AUTHORIZED;
            break;
        case PATH_PAYMENT_LINE_FULL:
            res = PAYMENT_LINE_FULL;
            break;
        default:
            throw std::runtime_error("Unexpected error code from pathPayment");
        }
        innerResult().code(res);
        return false;
    }

    assert(PathPaymentOpFrame::getInnerCode(ppayment.getResult()) ==
           PATH_PAYMENT_SUCCESS);

    innerResult().code(PAYMENT_SUCCESS);

    return true;
}

bool
PaymentOpFrame::doCheckValid()
{
    if (mPayment.amount <= 0)
    {
        innerResult().code(PAYMENT_MALFORMED);
        return false;
    }
    if (!isCurrencyValid(mPayment.currency))
    {
        innerResult().code(PAYMENT_MALFORMED);
        return false;
    }
    return true;
}
}
