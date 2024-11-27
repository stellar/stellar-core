// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/MutableTransactionResult.h"
#include "transactions/OperationFrame.h"
#include "transactions/TransactionFrame.h"
#include "transactions/TransactionUtils.h"
#include "xdr/Stellar-transaction.h"

#include <Tracy.hpp>

namespace stellar
{

MutableTransactionResultBase::MutableTransactionResultBase()
    : mTxResult(std::make_unique<TransactionResult>())
{
}

MutableTransactionResultBase::MutableTransactionResultBase(
    MutableTransactionResultBase&& rhs)
    : mTxResult(std::move(rhs.mTxResult))
{
    releaseAssertOrThrow(mTxResult);
}

void
SorobanTxData::setSorobanConsumedNonRefundableFee(int64_t fee)
{
    mConsumedNonRefundableFee = fee;
}

int64_t
SorobanTxData::getSorobanFeeRefund() const
{
    return mFeeRefund;
}

void
SorobanTxData::setSorobanFeeRefund(int64_t fee)
{
    mFeeRefund = fee;
}

xdr::xvector<DiagnosticEvent> const&
SorobanTxData::getDiagnosticEvents() const
{
    return mDiagnosticEvents;
}

void
SorobanTxData::pushContractEvents(xdr::xvector<ContractEvent> const& evts)
{
    mEvents = evts;
}

void
SorobanTxData::pushDiagnosticEvents(xdr::xvector<DiagnosticEvent> const& evts)
{
    auto& des = mDiagnosticEvents;
    des.insert(des.end(), evts.begin(), evts.end());
}

void
SorobanTxData::pushDiagnosticEvent(DiagnosticEvent const& evt)
{
    mDiagnosticEvents.emplace_back(evt);
}

void
SorobanTxData::pushSimpleDiagnosticError(Config const& cfg, SCErrorType ty,
                                         SCErrorCode code,
                                         std::string&& message,
                                         xdr::xvector<SCVal>&& args)
{
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
SorobanTxData::pushApplyTimeDiagnosticError(Config const& cfg, SCErrorType ty,
                                            SCErrorCode code,
                                            std::string&& message,
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
SorobanTxData::pushValidationTimeDiagnosticError(Config const& cfg,
                                                 SCErrorType ty,
                                                 SCErrorCode code,
                                                 std::string&& message,
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
SorobanTxData::setReturnValue(SCVal const& returnValue)
{
    mReturnValue = returnValue;
}

void
SorobanTxData::publishSuccessDiagnosticsToMeta(TransactionMetaFrame& meta,
                                               Config const& cfg)
{
    meta.pushContractEvents(std::move(mEvents));
    meta.pushDiagnosticEvents(std::move(mDiagnosticEvents));
    meta.setReturnValue(std::move(mReturnValue));
    if (cfg.EMIT_SOROBAN_TRANSACTION_META_EXT_V1)
    {
        meta.setSorobanFeeInfo(mConsumedNonRefundableFee,
                               mConsumedRefundableFee, mConsumedRentFee);
    }
}

void
SorobanTxData::publishFailureDiagnosticsToMeta(TransactionMetaFrame& meta,
                                               Config const& cfg)
{
    meta.pushDiagnosticEvents(std::move(mDiagnosticEvents));
    if (cfg.EMIT_SOROBAN_TRANSACTION_META_EXT_V1)
    {
        meta.setSorobanFeeInfo(mConsumedNonRefundableFee,
                               /* totalRefundableFeeSpent */ 0,
                               /* rentFeeCharged */ 0);
    }
}

bool
SorobanTxData::consumeRefundableSorobanResources(
    uint32_t contractEventSizeBytes, int64_t rentFee, uint32_t protocolVersion,
    SorobanNetworkConfig const& sorobanConfig, Config const& cfg,
    TransactionFrame const& tx)
{
    ZoneScoped;
    releaseAssertOrThrow(tx.isSoroban());
    mConsumedContractEventsSizeBytes += contractEventSizeBytes;

    mConsumedRentFee += rentFee;
    mConsumedRefundableFee += rentFee;

    // mFeeRefund was set in apply
    if (mFeeRefund < mConsumedRentFee)
    {
        pushApplyTimeDiagnosticError(
            cfg, SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
            "refundable resource fee was not sufficient to cover the ledger "
            "storage rent: {} > {}",
            {makeU64SCVal(mConsumedRentFee), makeU64SCVal(mFeeRefund)});
        return false;
    }
    mFeeRefund -= mConsumedRentFee;

    FeePair consumedFee = TransactionFrame::computeSorobanResourceFee(
        protocolVersion, tx.sorobanResources(),
        static_cast<uint32>(
            tx.getResources(false).getVal(Resource::Type::TX_BYTE_SIZE)),
        mConsumedContractEventsSizeBytes, sorobanConfig, cfg);
    mConsumedRefundableFee += consumedFee.refundable_fee;
    if (mFeeRefund < consumedFee.refundable_fee)
    {
        pushApplyTimeDiagnosticError(
            cfg, SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
            "refundable resource fee was not sufficient to cover the events "
            "fee after paying for ledger storage rent: {} > {}",
            {makeU64SCVal(consumedFee.refundable_fee),
             makeU64SCVal(mFeeRefund)});
        return false;
    }
    mFeeRefund -= consumedFee.refundable_fee;
    return true;
}

MutableTransactionResult::MutableTransactionResult(TransactionFrame const& tx,
                                                   int64_t feeCharged)
    : MutableTransactionResultBase()
{
    auto const& ops = tx.getOperations();

    // pre-allocates the results for all operations
    mTxResult->result.code(txSUCCESS);
    mTxResult->result.results().resize(static_cast<uint32_t>(ops.size()));

    // Initialize op results to the correct op type
    for (size_t i = 0; i < ops.size(); i++)
    {
        auto const& opFrame = ops[i];
        auto& opFrameResult = mTxResult->result.results()[i];
        opFrameResult.code(opINNER);
        opFrameResult.tr().type(opFrame->getOperation().body.type());
    }

    mTxResult->feeCharged = feeCharged;

    // resets Soroban related fields
    if (tx.isSoroban())
    {
        mSorobanExtension = std::make_shared<SorobanTxData>();
    }
}

TransactionResult&
MutableTransactionResult::getInnermostResult()
{
    return *mTxResult;
}

void
MutableTransactionResult::setInnermostResultCode(TransactionResultCode code)
{
    mTxResult->result.code(code);
}

TransactionResult&
MutableTransactionResult::getResult()
{
    return *mTxResult;
}

TransactionResult const&
MutableTransactionResult::getResult() const
{
    return *mTxResult;
}

TransactionResultCode
MutableTransactionResult::getResultCode() const
{
    return getResult().result.code();
}

void
MutableTransactionResult::setResultCode(TransactionResultCode code)
{
    getResult().result.code(code);
}

std::shared_ptr<SorobanTxData>
MutableTransactionResult::getSorobanData()
{
    return mSorobanExtension;
}

OperationResult&
MutableTransactionResult::getOpResultAt(size_t index)
{
    return mTxResult->result.results().at(index);
}

xdr::xvector<DiagnosticEvent> const&
MutableTransactionResult::getDiagnosticEvents() const
{
    static xdr::xvector<DiagnosticEvent> const empty;
    if (mSorobanExtension)
    {
        return mSorobanExtension->getDiagnosticEvents();
    }
    else
    {
        return empty;
    }
}

void
MutableTransactionResult::refundSorobanFee(int64_t feeRefund,
                                           uint32_t ledgerVersion)
{
    releaseAssertOrThrow(mSorobanExtension);
    mTxResult->feeCharged -= feeRefund;
}

bool
MutableTransactionResult::isSuccess() const
{
    return getResult().result.code() == txSUCCESS;
}

#ifdef BUILD_TESTS
void
MutableTransactionResult::setReplayTransactionResult(
    TransactionResult const& replayResult)
{
    mReplayTransactionResult = std::make_optional(replayResult);
}

std::optional<TransactionResult> const&
MutableTransactionResult::getReplayTransactionResult() const
{
    return mReplayTransactionResult;
}
#endif // BUILD_TESTS

FeeBumpMutableTransactionResult::FeeBumpMutableTransactionResult(
    MutableTxResultPtr innerTxResult)
    : MutableTransactionResultBase(), mInnerTxResult(innerTxResult)
{
    releaseAssertOrThrow(mInnerTxResult);
}

FeeBumpMutableTransactionResult::FeeBumpMutableTransactionResult(
    MutableTxResultPtr&& outerTxResult, MutableTxResultPtr&& innerTxResult,
    TransactionFrameBasePtr innerTx)
    : MutableTransactionResultBase(std::move(*outerTxResult))
    , mInnerTxResult(std::move(innerTxResult))
{
    releaseAssertOrThrow(mInnerTxResult);
    innerTxResult.reset();
    outerTxResult.reset();

    updateResult(innerTx, *this);
}

void
FeeBumpMutableTransactionResult::updateResult(
    TransactionFrameBasePtr innerTx, MutableTransactionResultBase& txResult)
{
    if (txResult.getInnermostResult().result.code() == txSUCCESS)
    {
        txResult.setResultCode(txFEE_BUMP_INNER_SUCCESS);
    }
    else
    {
        txResult.setResultCode(txFEE_BUMP_INNER_FAILED);
    }

    auto& feeBumpIrp = txResult.getResult().result.innerResultPair();
    feeBumpIrp.transactionHash = innerTx->getContentsHash();

    auto const& innerTxRes = txResult.getInnermostResult();
    auto& feeBumpIrpRes = feeBumpIrp.result;
    feeBumpIrpRes.feeCharged = innerTxRes.feeCharged;
    feeBumpIrpRes.result.code(innerTxRes.result.code());
    switch (feeBumpIrpRes.result.code())
    {
    case txSUCCESS:
    case txFAILED:
        feeBumpIrpRes.result.results() = innerTxRes.result.results();
        break;
    default:
        break;
    }
}

TransactionResult&
FeeBumpMutableTransactionResult::getInnermostResult()
{
    return mInnerTxResult->getResult();
}

void
FeeBumpMutableTransactionResult::setInnermostResultCode(
    TransactionResultCode code)
{
    mInnerTxResult->setResultCode(code);
}

TransactionResult&
FeeBumpMutableTransactionResult::getResult()
{
    return *mTxResult;
}

TransactionResult const&
FeeBumpMutableTransactionResult::getResult() const
{
    return *mTxResult;
}

TransactionResultCode
FeeBumpMutableTransactionResult::getResultCode() const
{
    return getResult().result.code();
}

void
FeeBumpMutableTransactionResult::setResultCode(TransactionResultCode code)
{
    getResult().result.code(code);
}

OperationResult&
FeeBumpMutableTransactionResult::getOpResultAt(size_t index)
{
    return mInnerTxResult->getOpResultAt(index);
}

std::shared_ptr<SorobanTxData>
FeeBumpMutableTransactionResult::getSorobanData()
{
    return mInnerTxResult->getSorobanData();
}

xdr::xvector<DiagnosticEvent> const&
FeeBumpMutableTransactionResult::getDiagnosticEvents() const
{
    return mInnerTxResult->getDiagnosticEvents();
}

void
FeeBumpMutableTransactionResult::refundSorobanFee(int64_t feeRefund,
                                                  uint32_t ledgerVersion)
{
    // First update feeCharged of the inner result
    mInnerTxResult->refundSorobanFee(feeRefund, ledgerVersion);

    // The result codes and a feeCharged without the refund should have been set
    // already in updateResult in FeeBumpTransactionFrame::apply. At this point,
    // feeCharged is set correctly on the inner transaction, so update the
    // feeBump result.
    if (protocolVersionStartsFrom(ledgerVersion, ProtocolVersion::V_21))
    {
        auto& irp = mTxResult->result.innerResultPair();
        auto& innerRes = irp.result;
        innerRes.feeCharged = mInnerTxResult->getResult().feeCharged;

        // Now set the updated feeCharged on the fee bump.
        mTxResult->feeCharged -= feeRefund;
    }
}

bool
FeeBumpMutableTransactionResult::isSuccess() const
{
    return mTxResult->result.code() == txFEE_BUMP_INNER_SUCCESS;
}

#ifdef BUILD_TESTS
void
FeeBumpMutableTransactionResult::setReplayTransactionResult(
    TransactionResult const& replayResult)
{
    /* NO-OP */
}

std::optional<TransactionResult> const&
FeeBumpMutableTransactionResult::getReplayTransactionResult() const
{
    return mReplayTransactionResult;
}
#endif // BUILD_TESTS
}