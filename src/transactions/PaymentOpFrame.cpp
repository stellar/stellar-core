// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "transactions/PaymentOpFrame.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "transactions/PathPaymentStrictReceiveOpFrame.h"
#include "transactions/TransactionUtils.h"

namespace stellar
{

using namespace std;

PaymentOpFrame::PaymentOpFrame(Operation const& op, OperationResult& res,
                               TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx), mPayment(mOperation.body.paymentOp())
{
}

bool
PaymentOpFrame::doApply(AbstractLedgerTxn& ltx)
{
    // if sending to self XLM directly, just mark as success, else we need at
    // least to check trustlines
    // in ledger version 2 it would work for any asset type
    auto ledgerVersion = ltx.loadHeader().current().ledgerVersion;
    auto instantSuccess =
        ledgerVersion > 2
            ? toAccountID(mPayment.destination) == getSourceID() &&
                  mPayment.asset.type() == ASSET_TYPE_NATIVE
            : toAccountID(mPayment.destination) == getSourceID();
    if (instantSuccess)
    {
        innerResult().code(PAYMENT_SUCCESS);
        return true;
    }

    // build a pathPaymentOp
    Operation op;
    op.sourceAccount = mOperation.sourceAccount;
    op.body.type(PATH_PAYMENT_STRICT_RECEIVE);
    PathPaymentStrictReceiveOp& ppOp = op.body.pathPaymentStrictReceiveOp();
    ppOp.sendAsset = mPayment.asset;
    ppOp.destAsset = mPayment.asset;

    ppOp.destAmount = mPayment.amount;
    ppOp.sendMax = mPayment.amount;

    ppOp.destination = mPayment.destination;

    OperationResult opRes;
    opRes.code(opINNER);
    opRes.tr().type(PATH_PAYMENT_STRICT_RECEIVE);
    PathPaymentStrictReceiveOpFrame ppayment(op, opRes, mParentTx);

    if (!ppayment.doCheckValid(ledgerVersion) || !ppayment.doApply(ltx))
    {
        if (ppayment.getResultCode() != opINNER)
        {
            throw std::runtime_error(
                "Unexpected error code from pathPaymentStrictReceive");
        }
        PaymentResultCode res;

        switch (
            PathPaymentStrictReceiveOpFrame::getInnerCode(ppayment.getResult()))
        {
        case PATH_PAYMENT_STRICT_RECEIVE_UNDERFUNDED:
            res = PAYMENT_UNDERFUNDED;
            break;
        case PATH_PAYMENT_STRICT_RECEIVE_SRC_NOT_AUTHORIZED:
            res = PAYMENT_SRC_NOT_AUTHORIZED;
            break;
        case PATH_PAYMENT_STRICT_RECEIVE_SRC_NO_TRUST:
            res = PAYMENT_SRC_NO_TRUST;
            break;
        case PATH_PAYMENT_STRICT_RECEIVE_NO_DESTINATION:
            res = PAYMENT_NO_DESTINATION;
            break;
        case PATH_PAYMENT_STRICT_RECEIVE_NO_TRUST:
            res = PAYMENT_NO_TRUST;
            break;
        case PATH_PAYMENT_STRICT_RECEIVE_NOT_AUTHORIZED:
            res = PAYMENT_NOT_AUTHORIZED;
            break;
        case PATH_PAYMENT_STRICT_RECEIVE_LINE_FULL:
            res = PAYMENT_LINE_FULL;
            break;
        case PATH_PAYMENT_STRICT_RECEIVE_NO_ISSUER:
            res = PAYMENT_NO_ISSUER;
            break;
        default:
            throw std::runtime_error(
                "Unexpected error code from pathPaymentStrictReceive");
        }
        innerResult().code(res);
        return false;
    }

    assert(PathPaymentStrictReceiveOpFrame::getInnerCode(
               ppayment.getResult()) == PATH_PAYMENT_STRICT_RECEIVE_SUCCESS);

    innerResult().code(PAYMENT_SUCCESS);

    return true;
}

bool
PaymentOpFrame::doCheckValid(uint32_t ledgerVersion)
{
    if (mPayment.amount <= 0)
    {
        innerResult().code(PAYMENT_MALFORMED);
        return false;
    }
    if (!isAssetValid(mPayment.asset))
    {
        innerResult().code(PAYMENT_MALFORMED);
        return false;
    }
    return true;
}

void
PaymentOpFrame::insertLedgerKeysToPrefetch(
    std::unordered_set<LedgerKey>& keys) const
{
    auto dest = toAccountID(mPayment.destination);
    keys.emplace(accountKey(dest));

    // Prefetch issuer for non-native assets
    if (mPayment.asset.type() != ASSET_TYPE_NATIVE)
    {
        // These are *maybe* needed; For now, we load everything
        keys.emplace(trustlineKey(dest, mPayment.asset));
        keys.emplace(trustlineKey(getSourceID(), mPayment.asset));
    }
}
}
