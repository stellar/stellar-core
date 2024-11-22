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
#include "util/GlobalChecks.h"
#include "util/ProtocolVersion.h"
#include <Tracy.hpp>

namespace stellar
{

using namespace std;

PaymentOpFrame::PaymentOpFrame(Operation const& op,
                               TransactionFrame const& parentTx)
    : OperationFrame(op, parentTx), mPayment(mOperation.body.paymentOp())
{
}

bool
PaymentOpFrame::doApply(AppConnector& app, AbstractLedgerTxn& ltx,
                        Hash const& sorobanBasePrngSeed, OperationResult& res,
                        std::shared_ptr<SorobanTxData> sorobanData) const
{
    ZoneNamedN(applyZone, "PaymentOp apply", true);
    std::string payStr = assetToString(mPayment.asset);
    ZoneTextV(applyZone, payStr.c_str(), payStr.size());

    // if sending to self XLM directly, just mark as success, else we need at
    // least to check trustlines
    // in ledger version 2 it would work for any asset type
    auto ledgerVersion = ltx.loadHeader().current().ledgerVersion;
    auto destID = toAccountID(mPayment.destination);
    auto instantSuccess =
        protocolVersionStartsFrom(ledgerVersion, ProtocolVersion::V_3)
            ? destID == getSourceID() &&
                  mPayment.asset.type() == ASSET_TYPE_NATIVE
            : destID == getSourceID();
    if (instantSuccess)
    {
        innerResult(res).code(PAYMENT_SUCCESS);
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

    OperationResult ppRes;
    ppRes.code(opINNER);
    ppRes.tr().type(PATH_PAYMENT_STRICT_RECEIVE);
    PathPaymentStrictReceiveOpFrame ppayment(op, mParentTx);

    if (!ppayment.doCheckValid(ledgerVersion, ppRes) ||
        !ppayment.doApply(app, ltx, sorobanBasePrngSeed, ppRes, sorobanData))
    {
        if (ppRes.code() != opINNER)
        {
            throw std::runtime_error(
                "Unexpected error code from pathPaymentStrictReceive");
        }
        PaymentResultCode resCode;

        switch (PathPaymentStrictReceiveOpFrame::getInnerCode(ppRes))
        {
        case PATH_PAYMENT_STRICT_RECEIVE_UNDERFUNDED:
            resCode = PAYMENT_UNDERFUNDED;
            break;
        case PATH_PAYMENT_STRICT_RECEIVE_SRC_NOT_AUTHORIZED:
            resCode = PAYMENT_SRC_NOT_AUTHORIZED;
            break;
        case PATH_PAYMENT_STRICT_RECEIVE_SRC_NO_TRUST:
            resCode = PAYMENT_SRC_NO_TRUST;
            break;
        case PATH_PAYMENT_STRICT_RECEIVE_NO_DESTINATION:
            resCode = PAYMENT_NO_DESTINATION;
            break;
        case PATH_PAYMENT_STRICT_RECEIVE_NO_TRUST:
            resCode = PAYMENT_NO_TRUST;
            break;
        case PATH_PAYMENT_STRICT_RECEIVE_NOT_AUTHORIZED:
            resCode = PAYMENT_NOT_AUTHORIZED;
            break;
        case PATH_PAYMENT_STRICT_RECEIVE_LINE_FULL:
            resCode = PAYMENT_LINE_FULL;
            break;
        case PATH_PAYMENT_STRICT_RECEIVE_NO_ISSUER:
            resCode = PAYMENT_NO_ISSUER;
            break;
        default:
            throw std::runtime_error(
                "Unexpected error code from pathPaymentStrictReceive");
        }
        innerResult(res).code(resCode);
        return false;
    }

    releaseAssertOrThrow(PathPaymentStrictReceiveOpFrame::getInnerCode(ppRes) ==
                         PATH_PAYMENT_STRICT_RECEIVE_SUCCESS);

    innerResult(res).code(PAYMENT_SUCCESS);

    return true;
}

bool
PaymentOpFrame::doCheckValid(uint32_t ledgerVersion, OperationResult& res) const
{
    if (mPayment.amount <= 0)
    {
        innerResult(res).code(PAYMENT_MALFORMED);
        return false;
    }
    if (!isAssetValid(mPayment.asset, ledgerVersion))
    {
        innerResult(res).code(PAYMENT_MALFORMED);
        return false;
    }
    return true;
}

void
PaymentOpFrame::insertLedgerKeysToPrefetch(UnorderedSet<LedgerKey>& keys) const
{
    auto destID = toAccountID(mPayment.destination);
    keys.emplace(accountKey(destID));

    // Prefetch issuer for non-native assets
    if (mPayment.asset.type() != ASSET_TYPE_NATIVE)
    {
        // These are *maybe* needed; For now, we load everything
        keys.emplace(trustlineKey(destID, mPayment.asset));
        keys.emplace(trustlineKey(getSourceID(), mPayment.asset));
    }
}
}
