// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "OperationFrame.h"
#include "database/Database.h"
#include "ledger/LedgerState.h"
#include "ledger/LedgerStateEntry.h"
#include "ledger/LedgerStateHeader.h"
#include "main/Application.h"
#include "transactions/AllowTrustOpFrame.h"
#include "transactions/BumpSequenceOpFrame.h"
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
#include "transactions/TransactionUtils.h"
#include "util/Logging.h"
#include "xdrpp/marshal.h"
#include <string>

#include "medida/meter.h"
#include "medida/metrics_registry.h"

namespace stellar
{

using namespace std;

static int32_t
getNeededThreshold(LedgerStateEntry const& account, ThresholdLevel const level)
{
    auto const& acc = account.current().data.account();
    switch (level)
    {
    case ThresholdLevel::LOW:
        return acc.thresholds[THRESHOLD_LOW];
    case ThresholdLevel::MEDIUM:
        return acc.thresholds[THRESHOLD_MED];
    case ThresholdLevel::HIGH:
        return acc.thresholds[THRESHOLD_HIGH];
    default:
        abort();
    }
}

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
    case BUMP_SEQUENCE:
        return std::make_shared<BumpSequenceOpFrame>(op, res, tx);
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
OperationFrame::apply(SignatureChecker& signatureChecker, Application& app,
                      AbstractLedgerState& ls)
{
    bool res;
    res = checkValid(signatureChecker, app, ls, true);
    if (res)
    {
        res = doApply(app, ls);
    }

    return res;
}

ThresholdLevel
OperationFrame::getThresholdLevel() const
{
    return ThresholdLevel::MEDIUM;
}

bool OperationFrame::isVersionSupported(uint32_t) const
{
    return true;
}

bool
OperationFrame::checkSignature(SignatureChecker& signatureChecker,
                               Application& app, AbstractLedgerState& ls,
                               bool forApply)
{
    auto header = ls.loadHeader();
    auto sourceAccount = loadSourceAccount(ls, header);
    if (sourceAccount)
    {
        auto neededThreshold =
            getNeededThreshold(sourceAccount, getThresholdLevel());
        if (!mParentTx.checkSignature(signatureChecker, sourceAccount,
                                      neededThreshold))
        {
            app.getMetrics()
                .NewMeter({"operation", "failure", "bad-auth"}, "operation")
                .Mark();
            mResult.code(opBAD_AUTH);
            return false;
        }
    }
    else
    {
        if (forApply || !mOperation.sourceAccount)
        {
            app.getMetrics()
                .NewMeter({"operation", "failure", "no-account"}, "operation")
                .Mark();
            mResult.code(opNO_ACCOUNT);
            return false;
        }

        if (!mParentTx.checkSignatureNoAccount(signatureChecker,
                                               *mOperation.sourceAccount))
        {
            app.getMetrics()
                .NewMeter({"operation", "failure", "bad-auth"}, "operation")
                .Mark();
            mResult.code(opBAD_AUTH);
            return false;
        }
    }

    return true;
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
                           AbstractLedgerState& lsOuter, bool forApply)
{
    // Note: ls is always rolled back so checkValid never modifies the ledger
    LedgerState ls(lsOuter);
    auto ledgerVersion = ls.loadHeader().current().ledgerVersion;
    if (!isVersionSupported(ledgerVersion))
    {
        app.getMetrics()
            .NewMeter({"operation", "failure", "not-supported"}, "operation")
            .Mark();
        mResult.code(opNOT_SUPPORTED);
        return false;
    }

    if (!forApply || ledgerVersion < 10)
    {
        if (!checkSignature(signatureChecker, app, ls, forApply))
        {
            return false;
        }
    }
    else
    {
        // for ledger versions >= 10 we need to load account here, as for
        // previous versions it is done in checkSignature call
        if (!loadSourceAccount(ls, ls.loadHeader()))
        {
            app.getMetrics()
                .NewMeter({"operation", "failure", "no-account"}, "operation")
                .Mark();
            mResult.code(opNO_ACCOUNT);
            return false;
        }
    }

    mResult.code(opINNER);
    mResult.tr().type(mOperation.body.type());

    return doCheckValid(app, ledgerVersion);
}

LedgerStateEntry
OperationFrame::loadSourceAccount(AbstractLedgerState& ls,
                                  LedgerStateHeader const& header)
{
    return mParentTx.loadAccount(ls, header, getSourceID());
}
}
