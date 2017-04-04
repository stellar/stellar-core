// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "OperationFrame.h"
#include "database/Database.h"
#include "ledger/LedgerDelta.h"
#include "main/Application.h"
#include "transactions/AllowTrustOpFrame.h"
#include "transactions/ChangeTrustOpFrame.h"
#include "transactions/CreateAccountOpFrame.h"
#include "transactions/CreatePassiveOfferOpFrame.h"
#include "transactions/InflationOpFrame.h"
#include "transactions/ManageDataOpFrame.h"
#include "transactions/ManageOfferOpFrame.h"
#include "transactions/MergeOpFrame.h"
#include "transactions/PathPaymentOpFrame.h"
#include "transactions/PaymentOpFrame.h"
#include "transactions/SetOptionsOpFrame.h"
#include "transactions/TransactionFrame.h"
#include "util/Logging.h"
#include "xdrpp/marshal.h"
#include <string>

#include "medida/meter.h"
#include "medida/metrics_registry.h"

namespace stellar
{

using namespace std;

bool
isSuccess(OperationResult const& result)
{
    if (result.code() != opINNER)
    {
        return false;
    }

    switch (result.tr().type())
    {
    case CREATE_ACCOUNT:
        return result.tr().createAccountResult().code() ==
               CREATE_ACCOUNT_SUCCESS;
    case PAYMENT:
        return result.tr().paymentResult().code() == PAYMENT_SUCCESS;
    case PATH_PAYMENT:
        return result.tr().pathPaymentResult().code() == PATH_PAYMENT_SUCCESS;
    case MANAGE_OFFER:
        return result.tr().manageOfferResult().code() == MANAGE_OFFER_SUCCESS;
    case CREATE_PASSIVE_OFFER:
        return result.tr().createPassiveOfferResult().code() ==
               MANAGE_OFFER_SUCCESS;
    case SET_OPTIONS:
        return result.tr().setOptionsResult().code() == SET_OPTIONS_SUCCESS;
    case CHANGE_TRUST:
        return result.tr().changeTrustResult().code() == CHANGE_TRUST_SUCCESS;
    case ALLOW_TRUST:
        return result.tr().allowTrustResult().code() == ALLOW_TRUST_SUCCESS;
    case ACCOUNT_MERGE:
        return result.tr().accountMergeResult().code() == ACCOUNT_MERGE_SUCCESS;
    case INFLATION:
        return result.tr().inflationResult().code() == INFLATION_SUCCESS;
    case MANAGE_DATA:
        return result.tr().manageDataResult().code() == MANAGE_DATA_SUCCESS;
    }
}

namespace
{

int32_t
getNeededThreshold(AccountFrame const& account, ThresholdLevel const level)
{
    switch (level)
    {
    case ThresholdLevel::LOW:
        return account.getLowThreshold();
    case ThresholdLevel::MEDIUM:
        return account.getMediumThreshold();
    case ThresholdLevel::HIGH:
        return account.getHighThreshold();
    default:
        assert(false);
    }
}

OperationResult
makeResult(OperationResultCode code)
{
    auto result = OperationResult{};
    result.code(code);
    return result;
}

}

shared_ptr<OperationFrame>
OperationFrame::makeHelper(Operation const& op, TransactionFrame& tx)
{
    switch (op.body.type())
    {
    case CREATE_ACCOUNT:
        return std::make_shared<CreateAccountOpFrame>(op, tx);
    case PAYMENT:
        return std::make_shared<PaymentOpFrame>(op, tx);
    case PATH_PAYMENT:
        return std::make_shared<PathPaymentOpFrame>(op, tx);
    case MANAGE_OFFER:
        return std::make_shared<ManageOfferOpFrame>(op, tx);
    case CREATE_PASSIVE_OFFER:
        return std::make_shared<CreatePassiveOfferOpFrame>(op, tx);
    case SET_OPTIONS:
        return std::make_shared<SetOptionsOpFrame>(op, tx);
    case CHANGE_TRUST:
        return std::make_shared<ChangeTrustOpFrame>(op, tx);
    case ALLOW_TRUST:
        return std::make_shared<AllowTrustOpFrame>(op, tx);
    case ACCOUNT_MERGE:
        return std::make_shared<MergeOpFrame>(op, tx);
    case INFLATION:
        return std::make_shared<InflationOpFrame>(op, tx);
    case MANAGE_DATA:
        return std::make_shared<ManageDataOpFrame>(op, tx);
    default:
        ostringstream err;
        err << "Unknown Tx type: " << op.body.type();
        throw std::invalid_argument(err.str());
    }
}

OperationFrame::OperationFrame(Operation const& op, TransactionFrame& parentTx)
    : mOperation(op), mParentTx(parentTx)
{
}

OperationResult
OperationFrame::apply(SignatureChecker& signatureChecker, LedgerDelta& delta,
                      Application& app)
{
    auto res = checkValid(signatureChecker, app, &delta);
    if (isSuccess(res))
    {
        return doApply(app, delta, app.getLedgerManager());
    }

    return res;
}

ThresholdLevel
OperationFrame::getThresholdLevel() const
{
    return ThresholdLevel::MEDIUM;
}

bool
OperationFrame::checkSignature(SignatureChecker& signatureChecker) const
{
    auto neededThreshold =
        getNeededThreshold(*mSourceAccount, getThresholdLevel());
    return mParentTx.checkSignature(signatureChecker, *mSourceAccount,
                                    neededThreshold);
}

AccountID const&
OperationFrame::getSourceID() const
{
    return mOperation.sourceAccount ? *mOperation.sourceAccount
                                    : mParentTx.getEnvelope().tx.sourceAccount;
}

bool
OperationFrame::loadAccount(int ledgerProtocolVersion, LedgerDelta* delta, Database& db)
{
    mSourceAccount = mParentTx.loadAccount(ledgerProtocolVersion, delta, db, getSourceID());
    return !!mSourceAccount;
}

// called when determining if we should accept this operation.
// called when determining if we should flood
// make sure sig is correct
// verifies that the operation is well formed (operation specific)
OperationResult
OperationFrame::checkValid(SignatureChecker& signatureChecker, Application& app,
                           LedgerDelta* delta)
{
    bool forApply = (delta != nullptr);
    if (!loadAccount(app.getLedgerManager().getCurrentLedgerVersion(), delta, app.getDatabase()))
    {
        if (forApply || !mOperation.sourceAccount)
        {
            app.getMetrics()
                .NewMeter({"operation", "invalid", "no-account"}, "operation")
                .Mark();
            return makeResult(opNO_ACCOUNT);
        }
        else
        {
            mSourceAccount =
                AccountFrame::makeAuthOnlyAccount(*mOperation.sourceAccount);
        }
    }

    if (app.getLedgerManager().getCurrentLedgerVersion() != 7 && !checkSignature(signatureChecker))
    {
        app.getMetrics()
            .NewMeter({"operation", "invalid", "bad-auth"}, "operation")
            .Mark();
        return makeResult(opBAD_AUTH);
    }

    if (!forApply)
    {
        // safety: operations should not rely on ledger state as
        // previous operations may change it (can even create the account)
        mSourceAccount.reset();
    }

    return doCheckValid(app);
}
}
