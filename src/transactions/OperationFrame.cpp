// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/OperationFrame.h"
#include "transactions/AllowTrustOpFrame.h"
#include "transactions/BeginSponsoringFutureReservesOpFrame.h"
#include "transactions/BumpSequenceOpFrame.h"
#include "transactions/ChangeTrustOpFrame.h"
#include "transactions/ClaimClaimableBalanceOpFrame.h"
#include "transactions/ClawbackClaimableBalanceOpFrame.h"
#include "transactions/ClawbackOpFrame.h"
#include "transactions/CreateAccountOpFrame.h"
#include "transactions/CreateClaimableBalanceOpFrame.h"
#include "transactions/CreatePassiveSellOfferOpFrame.h"
#include "transactions/EndSponsoringFutureReservesOpFrame.h"
#include "transactions/ExtendFootprintTTLOpFrame.h"
#include "transactions/InflationOpFrame.h"
#include "transactions/InvokeHostFunctionOpFrame.h"
#include "transactions/LiquidityPoolDepositOpFrame.h"
#include "transactions/LiquidityPoolWithdrawOpFrame.h"
#include "transactions/ManageBuyOfferOpFrame.h"
#include "transactions/ManageDataOpFrame.h"
#include "transactions/ManageSellOfferOpFrame.h"
#include "transactions/MergeOpFrame.h"
#include "transactions/PathPaymentStrictReceiveOpFrame.h"
#include "transactions/PathPaymentStrictSendOpFrame.h"
#include "transactions/PaymentOpFrame.h"
#include "transactions/RestoreFootprintOpFrame.h"
#include "transactions/RevokeSponsorshipOpFrame.h"
#include "transactions/SetOptionsOpFrame.h"
#include "transactions/SetTrustLineFlagsOpFrame.h"
#include "transactions/TransactionFrame.h"
#include "transactions/TransactionUtils.h"
#include "util/Logging.h"
#include "util/ProtocolVersion.h"
#include "util/XDRCereal.h"
#include <Tracy.hpp>
#include <medida/metrics_registry.h>

namespace stellar
{

using namespace std;

static int32_t
getNeededThreshold(LedgerTxnEntry const& account, ThresholdLevel const level)
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
                           TransactionFrame& tx, uint32_t index)
{
    switch (op.body.type())
    {
    case CREATE_ACCOUNT:
        return std::make_shared<CreateAccountOpFrame>(op, res, tx);
    case PAYMENT:
        return std::make_shared<PaymentOpFrame>(op, res, tx);
    case PATH_PAYMENT_STRICT_RECEIVE:
        return std::make_shared<PathPaymentStrictReceiveOpFrame>(op, res, tx);
    case MANAGE_SELL_OFFER:
        return std::make_shared<ManageSellOfferOpFrame>(op, res, tx);
    case CREATE_PASSIVE_SELL_OFFER:
        return std::make_shared<CreatePassiveSellOfferOpFrame>(op, res, tx);
    case SET_OPTIONS:
        return std::make_shared<SetOptionsOpFrame>(op, res, tx);
    case CHANGE_TRUST:
        return std::make_shared<ChangeTrustOpFrame>(op, res, tx);
    case ALLOW_TRUST:
        return std::make_shared<AllowTrustOpFrame>(op, res, tx, index);
    case ACCOUNT_MERGE:
        return std::make_shared<MergeOpFrame>(op, res, tx);
    case INFLATION:
        return std::make_shared<InflationOpFrame>(op, res, tx);
    case MANAGE_DATA:
        return std::make_shared<ManageDataOpFrame>(op, res, tx);
    case BUMP_SEQUENCE:
        return std::make_shared<BumpSequenceOpFrame>(op, res, tx);
    case MANAGE_BUY_OFFER:
        return std::make_shared<ManageBuyOfferOpFrame>(op, res, tx);
    case PATH_PAYMENT_STRICT_SEND:
        return std::make_shared<PathPaymentStrictSendOpFrame>(op, res, tx);
    case CREATE_CLAIMABLE_BALANCE:
        return std::make_shared<CreateClaimableBalanceOpFrame>(op, res, tx,
                                                               index);
    case CLAIM_CLAIMABLE_BALANCE:
        return std::make_shared<ClaimClaimableBalanceOpFrame>(op, res, tx);
    case BEGIN_SPONSORING_FUTURE_RESERVES:
        return std::make_shared<BeginSponsoringFutureReservesOpFrame>(op, res,
                                                                      tx);
    case END_SPONSORING_FUTURE_RESERVES:
        return std::make_shared<EndSponsoringFutureReservesOpFrame>(op, res,
                                                                    tx);
    case REVOKE_SPONSORSHIP:
        return std::make_shared<RevokeSponsorshipOpFrame>(op, res, tx);
    case CLAWBACK:
        return std::make_shared<ClawbackOpFrame>(op, res, tx);
    case CLAWBACK_CLAIMABLE_BALANCE:
        return std::make_shared<ClawbackClaimableBalanceOpFrame>(op, res, tx);
    case SET_TRUST_LINE_FLAGS:
        return std::make_shared<SetTrustLineFlagsOpFrame>(op, res, tx, index);
    case LIQUIDITY_POOL_DEPOSIT:
        return std::make_shared<LiquidityPoolDepositOpFrame>(op, res, tx);
    case LIQUIDITY_POOL_WITHDRAW:
        return std::make_shared<LiquidityPoolWithdrawOpFrame>(op, res, tx);
    case INVOKE_HOST_FUNCTION:
        return std::make_shared<InvokeHostFunctionOpFrame>(op, res, tx);
    case EXTEND_FOOTPRINT_TTL:
        return std::make_shared<ExtendFootprintTTLOpFrame>(op, res, tx);
    case RESTORE_FOOTPRINT:
        return std::make_shared<RestoreFootprintOpFrame>(op, res, tx);
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
    resetResultSuccess();
}

bool
OperationFrame::apply(Application& app, SignatureChecker& signatureChecker,
                      AbstractLedgerTxn& ltx, Hash const& sorobanBasePrngSeed,
                      TransactionResultPayload& resPayload)
{
    ZoneScoped;
    bool res;
    CLOG_TRACE(Tx, "{}", xdrToCerealString(mOperation, "Operation"));
    res = checkValid(app, signatureChecker, ltx, true, resPayload);
    if (res)
    {
        res = doApply(app, ltx, sorobanBasePrngSeed, resPayload);
        CLOG_TRACE(Tx, "{}", xdrToCerealString(mResult, "OperationResult"));
    }

    return res;
}

bool
OperationFrame::doApply(Application& _app, AbstractLedgerTxn& ltx,
                        Hash const& sorobanBasePrngSeed,
                        TransactionResultPayload& resPayload)
{
    // By default we ignore the app and seed, but subclasses can override to
    // intercept and use them.
    return doApply(ltx);
}

ThresholdLevel
OperationFrame::getThresholdLevel() const
{
    return ThresholdLevel::MEDIUM;
}

bool
OperationFrame::isOpSupported(LedgerHeader const&) const
{
    return true;
}

bool
OperationFrame::checkSignature(SignatureChecker& signatureChecker,
                               AbstractLedgerTxn& ltx, bool forApply)
{
    ZoneScoped;
    auto header = ltx.loadHeader();
    auto sourceAccount = loadSourceAccount(ltx, header);
    if (sourceAccount)
    {
        auto neededThreshold =
            getNeededThreshold(sourceAccount, getThresholdLevel());
        if (!mParentTx.checkSignature(signatureChecker, sourceAccount,
                                      neededThreshold))
        {
            mResult.code(opBAD_AUTH);
            return false;
        }
    }
    else
    {
        if (forApply || !mOperation.sourceAccount)
        {
            mResult.code(opNO_ACCOUNT);
            return false;
        }

        if (!mParentTx.checkSignatureNoAccount(
                signatureChecker, toAccountID(*mOperation.sourceAccount)))
        {
            mResult.code(opBAD_AUTH);
            return false;
        }
    }

    return true;
}

AccountID
OperationFrame::getSourceID() const
{
    return mOperation.sourceAccount ? toAccountID(*mOperation.sourceAccount)
                                    : mParentTx.getSourceID();
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
OperationFrame::checkValid(Application& app, SignatureChecker& signatureChecker,
                           AbstractLedgerTxn& ltxOuter, bool forApply,
                           TransactionResultPayload& resPayload)
{
    ZoneScoped;
    // Note: ltx is always rolled back so checkValid never modifies the ledger
    LedgerTxn ltx(ltxOuter);
    if (!isOpSupported(ltx.loadHeader().current()))
    {
        mResult.code(opNOT_SUPPORTED);
        return false;
    }

    auto ledgerVersion = ltx.loadHeader().current().ledgerVersion;
    if (!forApply ||
        protocolVersionIsBefore(ledgerVersion, ProtocolVersion::V_10))
    {
        if (!checkSignature(signatureChecker, ltx, forApply))
        {
            return false;
        }
    }
    else
    {
        // for ledger versions >= 10 we need to load account here, as for
        // previous versions it is done in checkSignature call
        if (!loadSourceAccount(ltx, ltx.loadHeader()))
        {
            mResult.code(opNO_ACCOUNT);
            return false;
        }
    }

    resetResultSuccess();

    if (protocolVersionStartsFrom(ledgerVersion, SOROBAN_PROTOCOL_VERSION))
    {
        auto const& sorobanConfig =
            app.getLedgerManager().getSorobanNetworkConfig();

        return doCheckValid(sorobanConfig, app.getConfig(), ledgerVersion,
                            resPayload);
    }
    else
    {
        return doCheckValid(ledgerVersion);
    }
}

bool
OperationFrame::doCheckValid(SorobanNetworkConfig const& config,
                             Config const& appConfig, uint32_t ledgerVersion,
                             TransactionResultPayload& resPayload)
{
    return doCheckValid(ledgerVersion);
}

LedgerTxnEntry
OperationFrame::loadSourceAccount(AbstractLedgerTxn& ltx,
                                  LedgerTxnHeader const& header)
{
    ZoneScoped;
    return mParentTx.loadAccount(ltx, header, getSourceID());
}

void
OperationFrame::resetResultSuccess()
{
    mResult.code(opINNER);
    mResult.tr().type(mOperation.body.type());
}

bool
OperationFrame::isDexOperation() const
{
    return false;
}

bool
OperationFrame::isSoroban() const
{
    return false;
}

void
OperationFrame::insertLedgerKeysToPrefetch(UnorderedSet<LedgerKey>& keys) const
{
    // Do nothing by default
    return;
}
}
