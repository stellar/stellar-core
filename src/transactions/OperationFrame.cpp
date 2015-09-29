// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "OperationFrame.h"
#include "main/Application.h"
#include "xdrpp/marshal.h"
#include <string>
#include "util/Logging.h"
#include "ledger/LedgerDelta.h"
#include "transactions/TransactionFrame.h"
#include "transactions/AllowTrustOpFrame.h"
#include "transactions/CreateAccountOpFrame.h"
#include "transactions/ManageOfferOpFrame.h"
#include "transactions/CreatePassiveOfferOpFrame.h"
#include "transactions/ChangeTrustOpFrame.h"
#include "transactions/InflationOpFrame.h"
#include "transactions/MergeOpFrame.h"
#include "transactions/PathPaymentOpFrame.h"
#include "transactions/PaymentOpFrame.h"
#include "transactions/SetOptionsOpFrame.h"
#include "database/Database.h"

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
        return shared_ptr<OperationFrame>(
            new CreateAccountOpFrame(op, res, tx));
    case PAYMENT:
        return shared_ptr<OperationFrame>(new PaymentOpFrame(op, res, tx));
    case PATH_PAYMENT:
        return shared_ptr<OperationFrame>(new PathPaymentOpFrame(op, res, tx));
    case MANAGE_OFFER:
        return shared_ptr<OperationFrame>(new ManageOfferOpFrame(op, res, tx));
    case CREATE_PASSIVE_OFFER:
        return shared_ptr<OperationFrame>(
            new CreatePassiveOfferOpFrame(op, res, tx));
    case SET_OPTIONS:
        return shared_ptr<OperationFrame>(new SetOptionsOpFrame(op, res, tx));
    case CHANGE_TRUST:
        return shared_ptr<OperationFrame>(new ChangeTrustOpFrame(op, res, tx));
    case ALLOW_TRUST:
        return shared_ptr<OperationFrame>(new AllowTrustOpFrame(op, res, tx));
    case ACCOUNT_MERGE:
        return shared_ptr<OperationFrame>(new MergeOpFrame(op, res, tx));
    case INFLATION:
        return shared_ptr<OperationFrame>(new InflationOpFrame(op, res, tx));

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
OperationFrame::apply(LedgerDelta& delta, Application& app)
{
    bool res;
    res = checkValid(app, true);
    if (res)
    {
        res = doApply(app.getMetrics(), delta, app.getLedgerManager());
    }

    return res;
}

int32_t
OperationFrame::getNeededThreshold() const
{
    return mSourceAccount->getMediumThreshold();
}

bool
OperationFrame::checkSignature() const
{
    return mParentTx.checkSignature(*mSourceAccount, getNeededThreshold());
}

AccountID const&
OperationFrame::getSourceID() const
{
    return mOperation.sourceAccount ? *mOperation.sourceAccount
                                    : mParentTx.getEnvelope().tx.sourceAccount;
}

bool
OperationFrame::loadAccount(Database& db)
{
    mSourceAccount = mParentTx.loadAccount(db, getSourceID());
    return !!mSourceAccount;
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
OperationFrame::checkValid(Application& app, bool forApply)
{
    if (!loadAccount(app.getDatabase()))
    {
        if (forApply || !mOperation.sourceAccount)
        {
            app.getMetrics()
                .NewMeter({"operation", "invalid", "no-account"}, "operation")
                .Mark();
            mResult.code(opNO_ACCOUNT);
            return false;
        }
        else
        {
            mSourceAccount =
                AccountFrame::makeAuthOnlyAccount(*mOperation.sourceAccount);
        }
    }

    if (!checkSignature())
    {
        app.getMetrics()
            .NewMeter({"operation", "invalid", "bad-auth"}, "operation")
            .Mark();
        mResult.code(opBAD_AUTH);
        return false;
    }

    if (!forApply)
    {
        // safety: operations should not rely on ledger state as
        // previous operations may change it (can even create the account)
        mSourceAccount.reset();
    }

    mResult.code(opINNER);
    mResult.tr().type(mOperation.body.type());

    return doCheckValid(app.getMetrics());
}
}
