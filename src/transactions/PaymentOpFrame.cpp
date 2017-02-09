// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "transactions/PaymentOpFrame.h"
#include "OfferExchange.h"
#include "database/Database.h"
#include "ledger/LedgerDelta.h"
#include "ledger/OfferFrame.h"
#include "ledger/TrustFrame.h"
#include "main/Application.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "transactions/PathPaymentOpFrame.h"
#include "util/Logging.h"
#include <algorithm>

namespace stellar
{

using namespace std;
using xdr::operator==;

PaymentOpFrame::PaymentOpFrame(Operation const& op, OperationResult& res,
                               TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx), mPayment(mOperation.body.paymentOp())
{
}

bool
PaymentOpFrame::doApply(Application& app, LedgerDelta& delta,
                        LedgerManager& ledgerManager)
{
    // if sending to self XLM directly, just mark as success, else we need at
    // least to check trustlines
    // in ledger version 2 it would work for any asset type
    auto instantSuccess = app.getLedgerManager().getCurrentLedgerVersion() > 2
                              ? mPayment.destination == getSourceID() &&
                                    mPayment.asset.type() == ASSET_TYPE_NATIVE
                              : mPayment.destination == getSourceID();
    if (instantSuccess)
    {
        app.getMetrics()
            .NewMeter({"op-payment", "success", "apply"}, "operation")
            .Mark();
        innerResult().code(PAYMENT_SUCCESS);
        return true;
    }

    // build a pathPaymentOp
    Operation op;
    op.sourceAccount = mOperation.sourceAccount;
    op.body.type(PATH_PAYMENT);
    PathPaymentOp& ppOp = op.body.pathPaymentOp();
    ppOp.sendAsset = mPayment.asset;
    ppOp.destAsset = mPayment.asset;

    ppOp.destAmount = mPayment.amount;
    ppOp.sendMax = mPayment.amount;

    ppOp.destination = mPayment.destination;

    OperationResult opRes;
    opRes.code(opINNER);
    opRes.tr().type(PATH_PAYMENT);
    PathPaymentOpFrame ppayment(op, opRes, mParentTx);
    ppayment.setSourceAccountPtr(mSourceAccount);

    if (!ppayment.doCheckValid(app) ||
        !ppayment.doApply(app, delta, ledgerManager))
    {
        if (ppayment.getResultCode() != opINNER)
        {
            throw std::runtime_error("Unexpected error code from pathPayment");
        }
        PaymentResultCode res;

        switch (PathPaymentOpFrame::getInnerCode(ppayment.getResult()))
        {
        case PATH_PAYMENT_UNDERFUNDED:
            app.getMetrics()
                .NewMeter({"op-payment", "failure", "underfunded"}, "operation")
                .Mark();
            res = PAYMENT_UNDERFUNDED;
            break;
        case PATH_PAYMENT_SRC_NOT_AUTHORIZED:
            app.getMetrics()
                .NewMeter({"op-payment", "failure", "src-not-authorized"},
                          "operation")
                .Mark();
            res = PAYMENT_SRC_NOT_AUTHORIZED;
            break;
        case PATH_PAYMENT_SRC_NO_TRUST:
            app.getMetrics()
                .NewMeter({"op-payment", "failure", "src-no-trust"},
                          "operation")
                .Mark();
            res = PAYMENT_SRC_NO_TRUST;
            break;
        case PATH_PAYMENT_NO_DESTINATION:
            app.getMetrics()
                .NewMeter({"op-payment", "failure", "no-destination"},
                          "operation")
                .Mark();
            res = PAYMENT_NO_DESTINATION;
            break;
        case PATH_PAYMENT_NO_TRUST:
            app.getMetrics()
                .NewMeter({"op-payment", "failure", "no-trust"}, "operation")
                .Mark();
            res = PAYMENT_NO_TRUST;
            break;
        case PATH_PAYMENT_NOT_AUTHORIZED:
            app.getMetrics()
                .NewMeter({"op-payment", "failure", "not-authorized"},
                          "operation")
                .Mark();
            res = PAYMENT_NOT_AUTHORIZED;
            break;
        case PATH_PAYMENT_LINE_FULL:
            app.getMetrics()
                .NewMeter({"op-payment", "failure", "line-full"}, "operation")
                .Mark();
            res = PAYMENT_LINE_FULL;
            break;
        case PATH_PAYMENT_NO_ISSUER:
            app.getMetrics()
                .NewMeter({"op-payment", "failure", "no-issuer"}, "operation")
                .Mark();
            res = PAYMENT_NO_ISSUER;
            break;
        default:
            throw std::runtime_error("Unexpected error code from pathPayment");
        }
        innerResult().code(res);
        return false;
    }

    assert(PathPaymentOpFrame::getInnerCode(ppayment.getResult()) ==
           PATH_PAYMENT_SUCCESS);

    app.getMetrics()
        .NewMeter({"op-payment", "success", "apply"}, "operation")
        .Mark();
    innerResult().code(PAYMENT_SUCCESS);

    return true;
}

bool
PaymentOpFrame::doCheckValid(Application& app)
{
    if (mPayment.amount <= 0)
    {
        app.getMetrics()
            .NewMeter({"op-payment", "invalid", "malformed-negative-amount"},
                      "operation")
            .Mark();
        innerResult().code(PAYMENT_MALFORMED);
        return false;
    }
    if (!isAssetValid(mPayment.asset))
    {
        app.getMetrics()
            .NewMeter({"op-payment", "invalid", "malformed-invalid-asset"},
                      "operation")
            .Mark();
        innerResult().code(PAYMENT_MALFORMED);
        return false;
    }
    return true;
}
}
