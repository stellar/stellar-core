// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "OperationFrame.h"
#include "main/Application.h"
#include "xdrpp/marshal.h"
#include <string>
#include "util/Logging.h"
#include "ledger/LedgerDelta.h"
#include "transactions/AllowTrustTxFrame.h"
#include "transactions/CancelOfferFrame.h"
#include "transactions/CreateOfferFrame.h"
#include "transactions/ChangeTrustTxFrame.h"
#include "transactions/InflationFrame.h"
#include "transactions/MergeFrame.h"
#include "transactions/PaymentFrame.h"
#include "transactions/SetOptionsFrame.h"
#include "database/Database.h"

namespace stellar
{

    using namespace std;

    shared_ptr<OperationFrame> OperationFrame::makeHelper(Operation const& op,
        OperationResult &res, TransactionFrame &tx)
    {
        switch (op.body.type())
        {
        case PAYMENT:
            return shared_ptr<OperationFrame>(new PaymentOpFrame(op, res, tx));
        case CREATE_OFFER:
            return shared_ptr<OperationFrame>(new CreateOfferOpFrame(op, res, tx));
        case CANCEL_OFFER:
            return shared_ptr<OperationFrame>(new CancelOfferOpFrame(op, res, tx));
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

    OperationFrame::OperationFrame(Operation const& op, OperationResult &res, TransactionFrame & parentTx)
        : mOperation(op), mParentTx(parentTx), mResult(res)
    {
    }

    bool OperationFrame::apply(LedgerDelta& delta, Application& app)
    {
        bool res;
        res = checkValid(app);
        if (res)
        {
            res = doApply(delta, app.getLedgerMaster());
        }

        return res;
    }

    int32_t OperationFrame::getNeededThreshold()
    {
        return mSourceAccount->getMidThreshold();
    }

    bool OperationFrame::checkSignature()
    {
        return mParentTx.checkSignature(*mSourceAccount, getNeededThreshold());
    }

    uint256 const& OperationFrame::getSourceID()
    {
        return mOperation.sourceAccount ?
            *mOperation.sourceAccount : mParentTx.getEnvelope().tx.account;
    }

    bool OperationFrame::loadAccount(Application& app)
    {
        mSourceAccount = mParentTx.loadAccount(app, getSourceID());
        return !!mSourceAccount;
    }

    OperationResultCode OperationFrame::getResultCode()
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
    bool OperationFrame::checkValid(Application& app)
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
        mResult.tr().type(
            mOperation.body.type());

        return doCheckValid(app);
    }

}
