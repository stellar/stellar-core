// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "OperationFrame.h"
#include "main/Application.h"
#include "xdrpp/marshal.h"
#include <string>
#include "util/Logging.h"
#include "ledger/LedgerDelta.h"
#include "transactions/AllowTrustOpFrame.h"
#include "transactions/CreateOfferOpFrame.h"
#include "transactions/ChangeTrustOpFrame.h"
#include "transactions/InflationOpFrame.h"
#include "transactions/MergeOpFrame.h"
#include "transactions/PaymentOpFrame.h"
#include "transactions/SetOptionsOpFrame.h"
#include "database/Database.h"

namespace stellar
{

using namespace std;

shared_ptr<OperationFrame>
OperationFrame::makeHelper(Operation const& op, OperationResult& res,
                           TransactionFrame& tx)
{
    switch (op.body.type())
    {
    case PAYMENT:
        return shared_ptr<OperationFrame>(new PaymentOpFrame(op, res, tx));
    case CREATE_OFFER:
        return shared_ptr<OperationFrame>(new CreateOfferOpFrame(op, res, tx));
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
    res = checkValid(app);
    if (res)
    {
        res = doApply(delta, app.getLedgerManager());
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
OperationFrame::loadAccount(Application& app)
{
    mSourceAccount = mParentTx.loadAccount(app, getSourceID());
    return !!mSourceAccount;
}

OperationResultCode
OperationFrame::getResultCode() const
{
    return mResult.code();
}

// called when determining if we should accept this tx.
// called when determining if we should flood
// make sure sig is correct
// make sure maxFee is above the current fee
// make sure it is in the correct ledger bounds
// don't consider minBalance since you want to allow them to still send
// around credit etc
bool
OperationFrame::checkValid(Application& app)
{
    if (!loadAccount(app))
    {
        mResult.code(opNO_ACCOUNT);
        return false;
    }

    if (!checkSignature())
    {
        mResult.code(opBAD_AUTH);
        return false;
    }

    mResult.code(opINNER);
    mResult.tr().type(mOperation.body.type());

    return doCheckValid(app);
}
}
