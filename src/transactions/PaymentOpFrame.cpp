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

#include "medida/meter.h"
#include "medida/metrics_registry.h"

namespace stellar
{

using namespace std;

PaymentOpFrame::PaymentOpFrame(Operation const& op, OperationResult& res,
                               TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx), mPayment(mOperation.body.paymentOp())
{
}

bool
PaymentOpFrame::doApply(medida::MetricsRegistry& metrics,
                        LedgerDelta& delta, LedgerManager& ledgerManager)
{
    // if sending to self directly, just mark as success
    if (mPayment.destination == getSourceID())
    {
        metrics.NewMeter({"op-payment", "success", "apply"},
                         "operation").Mark();
        innerResult().code(PAYMENT_SUCCESS);
        return true;
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

    if (!ppayment.doCheckValid(metrics) ||
        !ppayment.doApply(metrics, delta, ledgerManager))
    {
        if (ppayment.getResultCode() != opINNER)
        {
            throw std::runtime_error("Unexpected error code from pathPayment");
        }
        PaymentResultCode res;

        switch (PathPaymentOpFrame::getInnerCode(ppayment.getResult()))
        {
        case PATH_PAYMENT_UNDERFUNDED:
            metrics.NewMeter({"op-payment", "failure", "underfunded"},
                             "operation").Mark();
            res = PAYMENT_UNDERFUNDED;
            break;
        case PATH_PAYMENT_NO_DESTINATION:
            metrics.NewMeter({"op-payment", "failure", "no-destination"},
                             "operation").Mark();
            res = PAYMENT_NO_DESTINATION;
            break;
        case PATH_PAYMENT_NO_TRUST:
            metrics.NewMeter({"op-payment", "failure", "no-trust"},
                             "operation").Mark();
            res = PAYMENT_NO_TRUST;
            break;
        case PATH_PAYMENT_NOT_AUTHORIZED:
            metrics.NewMeter({"op-payment", "failure", "not-authorized"},
                             "operation").Mark();
            res = PAYMENT_NOT_AUTHORIZED;
            break;
        case PATH_PAYMENT_LINE_FULL:
            metrics.NewMeter({"op-payment", "failure", "line-full"},
                             "operation").Mark();
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

    metrics.NewMeter({"op-payment", "success", "apply"},
                     "operation").Mark();
    innerResult().code(PAYMENT_SUCCESS);

    return true;
}

bool
PaymentOpFrame::doCheckValid(medida::MetricsRegistry& metrics)
{
    if (mPayment.amount <= 0)
    {
        metrics.NewMeter({"op-payment", "invalid",
                          "malformed-negative-amount"},
                         "operation").Mark();
        innerResult().code(PAYMENT_MALFORMED);
        return false;
    }
    if (!isCurrencyValid(mPayment.currency))
    {
        metrics.NewMeter({"op-payment", "invalid",
                          "malformed-invalid-currency"},
                         "operation").Mark();
        innerResult().code(PAYMENT_MALFORMED);
        return false;
    }
    return true;
}
}
