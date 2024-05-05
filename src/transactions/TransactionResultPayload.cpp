// Copyright 2034 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/TransactionResultPayload.h"
#include "transactions/TransactionFrame.h"
#include "transactions/TransactionUtils.h"

#include <Tracy.hpp>
namespace stellar
{
TransactionResultPayload::TransactionResultPayload(TransactionFrame& tx)
{
    auto const& envelope = tx.getEnvelope();
    auto const& ops = envelope.type() == ENVELOPE_TYPE_TX_V0
                          ? envelope.v0().tx.operations
                          : envelope.v1().tx.operations;
    mTxResult.result.code(txFAILED);
    mTxResult.result.results().resize(static_cast<uint32_t>(ops.size()));

    for (size_t i = 0; i < ops.size(); i++)
    {
        mOpFrames.push_back(
            tx.makeOperation(ops[i], mTxResult.result.results()[i], i));
    }
}

bool
TransactionResultPayload::isFeeBump() const
{
    return mOuterFeeBumpResult.has_value();
}

TransactionResult&
TransactionResultPayload::getInnerResult()
{
    return mTxResult;
}

TransactionResult&
TransactionResultPayload::getResult()
{
    if (mOuterFeeBumpResult)
    {
        return *mOuterFeeBumpResult;
    }

    return mTxResult;
}

TransactionResult const&
TransactionResultPayload::getResult() const
{
    if (mOuterFeeBumpResult)
    {
        return *mOuterFeeBumpResult;
    }

    return mTxResult;
}

TransactionResultCode
TransactionResultPayload::getResultCode() const
{
    return getResult().result.code();
}

bool
TransactionResultPayload::consumeRefundableSorobanResources(
    uint32_t contractEventSizeBytes, int64_t rentFee, uint32_t protocolVersion,
    SorobanNetworkConfig const& sorobanConfig, Config const& cfg,
    TransactionFrame& tx)
{
    ZoneScoped;
    releaseAssertOrThrow(tx.isSoroban());
    releaseAssertOrThrow(mSorobanExtension);
    auto& consumedContractEventsSizeBytes =
        mSorobanExtension->mConsumedContractEventsSizeBytes;
    consumedContractEventsSizeBytes += contractEventSizeBytes;

    auto& consumedRentFee = mSorobanExtension->mConsumedRentFee;
    auto& consumedRefundableFee = mSorobanExtension->mConsumedRefundableFee;
    consumedRentFee += rentFee;
    consumedRefundableFee += rentFee;

    // mFeeRefund was set in apply
    auto& feeRefund = mSorobanExtension->mFeeRefund;
    if (feeRefund < consumedRentFee)
    {
        pushApplyTimeDiagnosticError(
            cfg, SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
            "refundable resource fee was not sufficient to cover the ledger "
            "storage rent: {} > {}",
            {makeU64SCVal(consumedRentFee), makeU64SCVal(feeRefund)});
        return false;
    }
    feeRefund -= consumedRentFee;

    FeePair consumedFee = TransactionFrame::computeSorobanResourceFee(
        protocolVersion, tx.sorobanResources(),
        static_cast<uint32>(
            tx.getResources(false).getVal(Resource::Type::TX_BYTE_SIZE)),
        consumedContractEventsSizeBytes, sorobanConfig, cfg);
    consumedRefundableFee += consumedFee.refundable_fee;
    if (feeRefund < consumedFee.refundable_fee)
    {
        pushApplyTimeDiagnosticError(
            cfg, SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
            "refundable resource fee was not sufficient to cover the events "
            "fee after paying for ledger storage rent: {} > {}",
            {makeU64SCVal(consumedFee.refundable_fee),
             makeU64SCVal(feeRefund)});
        return false;
    }
    feeRefund -= consumedFee.refundable_fee;
    return true;
}

int64_t&
TransactionResultPayload::getSorobanConsumedNonRefundableFee()
{
    releaseAssert(mSorobanExtension);
    return mSorobanExtension->mConsumedNonRefundableFee;
}

int64_t&
TransactionResultPayload::getSorobanFeeRefund()
{
    releaseAssert(mSorobanExtension);
    return mSorobanExtension->mFeeRefund;
}

std::vector<std::shared_ptr<OperationFrame>> const&
TransactionResultPayload::getOpFrames() const
{
    return mOpFrames;
}

xdr::xvector<DiagnosticEvent> const&
TransactionResultPayload::getDiagnosticEvents() const
{
    static xdr::xvector<DiagnosticEvent> const empty;
    if (mSorobanExtension)
    {
        return mSorobanExtension->mDiagnosticEvents;
    }
    else
    {
        return empty;
    }
}

std::shared_ptr<InternalLedgerEntry const>&
TransactionResultPayload::getCachedAccountPtr()
{
    return mCachedAccount;
}

TransactionResultPayloadPtr
TransactionResultPayload::create(TransactionFrame& tx)
{
    return std::shared_ptr<TransactionResultPayload>(
        new TransactionResultPayload(tx));
}

void
TransactionResultPayload::initializeFeeBumpResult()
{
    mOuterFeeBumpResult = std::make_optional<TransactionResult>();
}

void
TransactionResultPayload::initializeSorobanExtension()
{
    mSorobanExtension = std::make_optional<SorobanData>();
}

void
TransactionResultPayload::pushContractEvents(
    xdr::xvector<ContractEvent> const& evts)
{
    releaseAssertOrThrow(mSorobanExtension);
    mSorobanExtension->mEvents = evts;
}

void
TransactionResultPayload::pushDiagnosticEvents(
    xdr::xvector<DiagnosticEvent> const& evts)
{
    releaseAssertOrThrow(mSorobanExtension);
    auto& des = mSorobanExtension->mDiagnosticEvents;
    des.insert(des.end(), evts.begin(), evts.end());
}

void
TransactionResultPayload::pushDiagnosticEvent(DiagnosticEvent const& evt)
{
    releaseAssertOrThrow(mSorobanExtension);
    mSorobanExtension->mDiagnosticEvents.emplace_back(evt);
}

void
TransactionResultPayload::pushSimpleDiagnosticError(Config const& cfg,
                                                    SCErrorType ty,
                                                    SCErrorCode code,
                                                    std::string&& message,
                                                    xdr::xvector<SCVal>&& args)
{
    releaseAssert(mSorobanExtension);

    ContractEvent ce;
    ce.type = DIAGNOSTIC;
    ce.body.v(0);

    SCVal sym = makeSymbolSCVal("error"), err;
    err.type(SCV_ERROR);
    err.error().type(ty);
    err.error().code() = code;
    ce.body.v0().topics.assign({std::move(sym), std::move(err)});

    if (args.empty())
    {
        ce.body.v0().data.type(SCV_STRING);
        ce.body.v0().data.str().assign(std::move(message));
    }
    else
    {
        ce.body.v0().data.type(SCV_VEC);
        ce.body.v0().data.vec().activate();
        ce.body.v0().data.vec()->reserve(args.size() + 1);
        ce.body.v0().data.vec()->emplace_back(
            makeStringSCVal(std::move(message)));
        std::move(std::begin(args), std::end(args),
                  std::back_inserter(*ce.body.v0().data.vec()));
    }
    DiagnosticEvent evt(false, std::move(ce));
    pushDiagnosticEvent(evt);
}

void
TransactionResultPayload::pushApplyTimeDiagnosticError(
    Config const& cfg, SCErrorType ty, SCErrorCode code, std::string&& message,
    xdr::xvector<SCVal>&& args)
{
    if (!cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS)
    {
        return;
    }
    pushSimpleDiagnosticError(cfg, ty, code, std::move(message),
                              std::move(args));
}

void
TransactionResultPayload::pushValidationTimeDiagnosticError(
    Config const& cfg, SCErrorType ty, SCErrorCode code, std::string&& message,
    xdr::xvector<SCVal>&& args)
{
    if (!cfg.ENABLE_DIAGNOSTICS_FOR_TX_SUBMISSION)
    {
        return;
    }
    pushSimpleDiagnosticError(cfg, ty, code, std::move(message),
                              std::move(args));
}

void
TransactionResultPayload::setReturnValue(SCVal const& returnValue)
{
    releaseAssertOrThrow(mSorobanExtension);
    mSorobanExtension->mReturnValue = returnValue;
}

void
TransactionResultPayload::publishSuccessDiagnosticsToMeta(
    TransactionMetaFrame& meta, Config const& cfg)
{
    releaseAssert(mSorobanExtension);
    meta.pushContractEvents(std::move(mSorobanExtension->mEvents));
    meta.pushDiagnosticEvents(std::move(mSorobanExtension->mDiagnosticEvents));
    meta.setReturnValue(std::move(mSorobanExtension->mReturnValue));
    if (cfg.EMIT_SOROBAN_TRANSACTION_META_EXT_V1)
    {
        meta.setSorobanFeeInfo(mSorobanExtension->mConsumedNonRefundableFee,
                               mSorobanExtension->mConsumedRefundableFee,
                               mSorobanExtension->mConsumedRentFee);
    }
}

void
TransactionResultPayload::publishFailureDiagnosticsToMeta(
    TransactionMetaFrame& meta, Config const& cfg)
{
    releaseAssert(mSorobanExtension);
    meta.pushDiagnosticEvents(std::move(mSorobanExtension->mDiagnosticEvents));
    if (cfg.EMIT_SOROBAN_TRANSACTION_META_EXT_V1)
    {
        meta.setSorobanFeeInfo(mSorobanExtension->mConsumedNonRefundableFee,
                               /* totalRefundableFeeSpent */ 0,
                               /* rentFeeCharged */ 0);
    }
}

void
TransactionResultPayload::reset(TransactionFrame& tx, int64_t feeCharged)
{
    auto const& envelope = tx.getEnvelope();
    auto const& ops = envelope.type() == ENVELOPE_TYPE_TX_V0
                          ? envelope.v0().tx.operations
                          : envelope.v1().tx.operations;

    // pre-allocates the results for all operations
    getInnerResult().result.code(txSUCCESS);
    getInnerResult().result.results().resize(static_cast<uint32_t>(ops.size()));

    mOpFrames.clear();

    // bind operations to the results
    for (size_t i = 0; i < ops.size(); i++)
    {
        auto op =
            tx.makeOperation(ops[i], getInnerResult().result.results()[i], i);
        mOpFrames.push_back(op);
    }

    getInnerResult().feeCharged = feeCharged;

    // resets Soroban related fields
    if (tx.isSoroban())
    {
        initializeSorobanExtension();
    }
}
}