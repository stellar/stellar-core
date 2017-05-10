// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "OperationFrame.h"
#include "database/Database.h"
#include "ledger/LedgerEntries.h"
#include "ledgerdelta/LedgerDelta.h"
#include "main/Application.h"
#include "signature/SigningAccount.h"
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

shared_ptr<OperationFrame>
OperationFrame::makeHelper(Operation const& op, OperationResult& res,
                           TransactionFrame& tx)
{
    switch (op.body.type())
    {
    case CREATE_ACCOUNT:
        return std::make_shared<CreateAccountOpFrame>(op, res, tx);
    case PAYMENT:
        return std::make_shared<PaymentOpFrame>(op, res, tx);
    case PATH_PAYMENT:
        return std::make_shared<PathPaymentOpFrame>(op, res, tx);
    case MANAGE_OFFER:
        return std::make_shared<ManageOfferOpFrame>(op, res, tx);
    case CREATE_PASSIVE_OFFER:
        return std::make_shared<CreatePassiveOfferOpFrame>(op, res, tx);
    case SET_OPTIONS:
        return std::make_shared<SetOptionsOpFrame>(op, res, tx);
    case CHANGE_TRUST:
        return std::make_shared<ChangeTrustOpFrame>(op, res, tx);
    case ALLOW_TRUST:
        return std::make_shared<AllowTrustOpFrame>(op, res, tx);
    case ACCOUNT_MERGE:
        return std::make_shared<MergeOpFrame>(op, res, tx);
    case INFLATION:
        return std::make_shared<InflationOpFrame>(op, res, tx);
    case MANAGE_DATA:
        return std::make_shared<ManageDataOpFrame>(op, res, tx);

    default:
        ostringstream err;
        err << "Unknown Tx type: " << op.body.type();
        throw std::invalid_argument(err.str());
    }
}

OperationFrame::OperationFrame(Operation const& op, OperationResult& res,
                               TransactionFrame& parentTx)
    : mOperation(op), mParentTx(parentTx), mResult(res)
{
}

bool
OperationFrame::apply(SignatureChecker& signatureChecker, LedgerDelta& ledgerDelta,
                      Application& app)
{
    bool res;
    res = checkValid(signatureChecker, app, &ledgerDelta);
    if (res)
    {
        res = doApply(app, ledgerDelta, app.getLedgerManager());
    }

    return res;
}

ThresholdLevel
OperationFrame::getThresholdLevel() const
{
    return ThresholdLevel::MEDIUM;
}

bool
OperationFrame::checkSignature(SigningAccount const& signingAccount, SignatureChecker& signatureChecker) const
{
    return mParentTx.checkSignature(signatureChecker, signingAccount,
                                    getThresholdLevel());
}

AccountID const&
OperationFrame::getSourceID() const
{
    return mOperation.sourceAccount ? *mOperation.sourceAccount
                                    : mParentTx.getEnvelope().tx.sourceAccount;
}

OperationResultCode
OperationFrame::getResultCode() const
{
    return mResult.code();
}

// called when determining if we should accept this operation.
// called when determining if we should flood
// make sure sig is correct
// verifies that the operation is well formed (operation specific)
bool
OperationFrame::checkValid(SignatureChecker& signatureChecker, Application& app,
                           LedgerDelta* ledgerDelta)
{
    SigningAccount signingAccount;

    bool forApply = (ledgerDelta != nullptr);

    auto sourceAccount = ledgerDelta
        ? ledgerDelta->loadAccount(getSourceID())
        : app.getLedgerEntries().load(accountKey(getSourceID()));
    if (!sourceAccount)
    {
        if (forApply)
        {
            app.getMetrics()
                .NewMeter({"operation", "invalid", "no-account"}, "operation")
                .Mark();
            mResult.code(opNO_ACCOUNT);
            return false;
        }
        else
        {
            signingAccount = SigningAccount{getSourceID()};
        }
    }
    else
    {
        signingAccount = SigningAccount{*sourceAccount};
    }

    if (!checkSignature(signingAccount, signatureChecker))
    {
        app.getMetrics()
            .NewMeter({"operation", "invalid", "bad-auth"}, "operation")
            .Mark();
        mResult.code(opBAD_AUTH);
        return false;
    }

    mResult.code(opINNER);
    mResult.tr().type(mOperation.body.type());

    return doCheckValid(app);
}
}
