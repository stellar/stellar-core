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
#include "transactions/MutableTransactionResult.h"
#include "transactions/PathPaymentStrictReceiveOpFrame.h"
#include "transactions/PathPaymentStrictSendOpFrame.h"
#include "transactions/PaymentOpFrame.h"
#include "transactions/RestoreFootprintOpFrame.h"
#include "transactions/RevokeSponsorshipOpFrame.h"
#include "transactions/SetOptionsOpFrame.h"
#include "transactions/SetTrustLineFlagsOpFrame.h"
#include "transactions/TransactionFrame.h"
#include "transactions/TransactionUtils.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/ProtocolVersion.h"
#include "util/XDRCereal.h"
#include <Tracy.hpp>

namespace stellar
{

using namespace std;

static int32_t
getNeededThreshold(LedgerEntryWrapper const& account,
                   ThresholdLevel const level)
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
OperationFrame::makeHelper(Operation const& op, TransactionFrame const& tx,
                           uint32_t index)
{
    switch (op.body.type())
    {
    case CREATE_ACCOUNT:
        return std::make_shared<CreateAccountOpFrame>(op, tx);
    case PAYMENT:
        return std::make_shared<PaymentOpFrame>(op, tx);
    case PATH_PAYMENT_STRICT_RECEIVE:
        return std::make_shared<PathPaymentStrictReceiveOpFrame>(op, tx);
    case MANAGE_SELL_OFFER:
        return std::make_shared<ManageSellOfferOpFrame>(op, tx);
    case CREATE_PASSIVE_SELL_OFFER:
        return std::make_shared<CreatePassiveSellOfferOpFrame>(op, tx);
    case SET_OPTIONS:
        return std::make_shared<SetOptionsOpFrame>(op, tx);
    case CHANGE_TRUST:
        return std::make_shared<ChangeTrustOpFrame>(op, tx);
    case ALLOW_TRUST:
        return std::make_shared<AllowTrustOpFrame>(op, tx, index);
    case ACCOUNT_MERGE:
        return std::make_shared<MergeOpFrame>(op, tx);
    case INFLATION:
        return std::make_shared<InflationOpFrame>(op, tx);
    case MANAGE_DATA:
        return std::make_shared<ManageDataOpFrame>(op, tx);
    case BUMP_SEQUENCE:
        return std::make_shared<BumpSequenceOpFrame>(op, tx);
    case MANAGE_BUY_OFFER:
        return std::make_shared<ManageBuyOfferOpFrame>(op, tx);
    case PATH_PAYMENT_STRICT_SEND:
        return std::make_shared<PathPaymentStrictSendOpFrame>(op, tx);
    case CREATE_CLAIMABLE_BALANCE:
        return std::make_shared<CreateClaimableBalanceOpFrame>(op, tx, index);
    case CLAIM_CLAIMABLE_BALANCE:
        return std::make_shared<ClaimClaimableBalanceOpFrame>(op, tx);
    case BEGIN_SPONSORING_FUTURE_RESERVES:
        return std::make_shared<BeginSponsoringFutureReservesOpFrame>(op, tx);
    case END_SPONSORING_FUTURE_RESERVES:
        return std::make_shared<EndSponsoringFutureReservesOpFrame>(op, tx);
    case REVOKE_SPONSORSHIP:
        return std::make_shared<RevokeSponsorshipOpFrame>(op, tx);
    case CLAWBACK:
        return std::make_shared<ClawbackOpFrame>(op, tx);
    case CLAWBACK_CLAIMABLE_BALANCE:
        return std::make_shared<ClawbackClaimableBalanceOpFrame>(op, tx);
    case SET_TRUST_LINE_FLAGS:
        return std::make_shared<SetTrustLineFlagsOpFrame>(op, tx, index);
    case LIQUIDITY_POOL_DEPOSIT:
        return std::make_shared<LiquidityPoolDepositOpFrame>(op, tx);
    case LIQUIDITY_POOL_WITHDRAW:
        return std::make_shared<LiquidityPoolWithdrawOpFrame>(op, tx);
    case INVOKE_HOST_FUNCTION:
        return std::make_shared<InvokeHostFunctionOpFrame>(op, tx);
    case EXTEND_FOOTPRINT_TTL:
        return std::make_shared<ExtendFootprintTTLOpFrame>(op, tx);
    case RESTORE_FOOTPRINT:
        return std::make_shared<RestoreFootprintOpFrame>(op, tx);
    default:
        ostringstream err;
        err << "Unknown Tx type: " << op.body.type();
        throw std::invalid_argument(err.str());
    }
}

OperationFrame::OperationFrame(Operation const& op,
                               TransactionFrame const& parentTx)
    : mOperation(op), mParentTx(parentTx)
{
}

bool
OperationFrame::apply(AppConnector& app, SignatureChecker& signatureChecker,
                      AbstractLedgerTxn& ltx, Hash const& sorobanBasePrngSeed,
                      OperationResult& res,
                      std::shared_ptr<SorobanTxData> sorobanData) const
{
    ZoneScoped;
    CLOG_TRACE(Tx, "{}", xdrToCerealString(mOperation, "Operation"));

    LedgerSnapshot ltxState(ltx);
    std::optional<SorobanNetworkConfig> cfg =
        isSoroban() ? std::make_optional(app.getSorobanNetworkConfigForApply())
                    : std::nullopt;
    bool applyRes = checkValid(app, signatureChecker, cfg, ltxState, true, res,
                               sorobanData);
    if (applyRes)
    {
        applyRes = doApply(app, ltx, sorobanBasePrngSeed, res, sorobanData);
        CLOG_TRACE(Tx, "{}", xdrToCerealString(res, "OperationResult"));
    }

    return applyRes;
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
                               LedgerSnapshot const& ls, OperationResult& res,
                               bool forApply) const
{
    ZoneScoped;
    auto header = ls.getLedgerHeader();
    auto sourceAccount = ls.getAccount(header, mParentTx, getSourceID());
    if (sourceAccount)
    {
        auto neededThreshold =
            getNeededThreshold(sourceAccount, getThresholdLevel());
        if (!mParentTx.checkSignature(signatureChecker, sourceAccount,
                                      neededThreshold))
        {
            res.code(opBAD_AUTH);
            return false;
        }
    }
    else
    {
        if (forApply || !mOperation.sourceAccount)
        {
            res.code(opNO_ACCOUNT);
            return false;
        }

        if (!mParentTx.checkSignatureNoAccount(
                signatureChecker, toAccountID(*mOperation.sourceAccount)))
        {
            res.code(opBAD_AUTH);
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

// called when determining if we should accept this operation.
// called when determining if we should flood
// make sure sig is correct
// verifies that the operation is well formed (operation specific)
bool
OperationFrame::checkValid(AppConnector& app,
                           SignatureChecker& signatureChecker,
                           std::optional<SorobanNetworkConfig> const& cfg,
                           LedgerSnapshot const& ls, bool forApply,
                           OperationResult& res,
                           std::shared_ptr<SorobanTxData> sorobanData) const
{
    ZoneScoped;
    bool validationResult = false;
    auto validate = [this, &res, forApply, &signatureChecker, &app,
                     &sorobanData, &validationResult,
                     &cfg](LedgerSnapshot const& ls) {
        if (!isOpSupported(ls.getLedgerHeader().current()))
        {
            res.code(opNOT_SUPPORTED);
            validationResult = false;
            return;
        }

        auto ledgerVersion = ls.getLedgerHeader().current().ledgerVersion;
        if (!forApply ||
            protocolVersionIsBefore(ledgerVersion, ProtocolVersion::V_10))
        {
            if (!checkSignature(signatureChecker, ls, res, forApply))
            {
                validationResult = false;
                return;
            }
        }
        else
        {
            // for ledger versions >= 10 we need to load account here, as for
            // previous versions it is done in checkSignature call
            if (!ls.getAccount(ls.getLedgerHeader(), mParentTx, getSourceID()))
            {
                res.code(opNO_ACCOUNT);
                validationResult = false;
                return;
            }
        }

        if (protocolVersionStartsFrom(ledgerVersion,
                                      SOROBAN_PROTOCOL_VERSION) &&
            isSoroban())
        {
            releaseAssertOrThrow(sorobanData);
            releaseAssertOrThrow(cfg);
            validationResult = doCheckValidForSoroban(
                cfg.value(), app.getConfig(), ledgerVersion, res, *sorobanData);
        }
        else
        {
            validationResult = doCheckValid(ledgerVersion, res);
        }
    };

    // Older protocol versions contain buggy account loading code,
    // so preserve nested LedgerTxn to avoid writing to the ledger
    if (protocolVersionIsBefore(ls.getLedgerHeader().current().ledgerVersion,
                                ProtocolVersion::V_8) &&
        forApply)
    {
        ls.executeWithMaybeInnerSnapshot(validate);
    }
    else
    {
        // Validate using read-only snapshot
        validate(ls);
    }

    return validationResult;
}

bool
OperationFrame::doCheckValidForSoroban(SorobanNetworkConfig const& config,
                                       Config const& appConfig,
                                       uint32_t ledgerVersion,
                                       OperationResult& res,
                                       SorobanTxData& sorobanData) const
{
    return doCheckValid(ledgerVersion, res);
}

LedgerTxnEntry
OperationFrame::loadSourceAccount(AbstractLedgerTxn& ltx,
                                  LedgerTxnHeader const& header) const
{
    ZoneScoped;
    return mParentTx.loadAccount(ltx, header, getSourceID());
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

SorobanResources const&
OperationFrame::getSorobanResources() const
{
    releaseAssertOrThrow(isSoroban());
    return mParentTx.sorobanResources();
}
}
