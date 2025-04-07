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
namespace
{
template <typename T>
void
initializeOperationResults(TransactionFrame const& tx, T& txResult)
{
    auto const& ops = tx.getOperations();

    // Pre-allocate the results for all operations
    txResult.result.results().resize(static_cast<uint32_t>(ops.size()));

    // Initialize op results to the correct op type
    for (size_t i = 0; i < ops.size(); i++)
    {
        auto const& opFrame = ops[i];
        auto& opFrameResult = txResult.result.results()[i];
        opFrameResult.code(opINNER);
        opFrameResult.tr().type(opFrame->getOperation().body.type());
    }
}
} // namespace

// MutableTransactionResultBase::MutableTransactionResultBase(
//     MutableTransactionResultBase&& rhs)
//     : mTxResult(std::move(rhs.mTxResult))
//{
//     releaseAssert(mTxResult);
// }

// void
// SorobanTxData::setSorobanConsumedNonRefundableFee(int64_t fee)
//{
//     mConsumedNonRefundableFee = fee;
// }
//
// int64_t
// SorobanTxData::getSorobanFeeRefund() const
//{
//     return mFeeRefund;
// }
//
// void
// SorobanTxData::setSorobanFeeRefund(int64_t fee)
//{
//     mFeeRefund = fee;
// }
//
// void
// SorobanTxData::setReturnValue(SCVal const& returnValue)
//{
//     mReturnValue = returnValue;
// }

// void
// SorobanTxData::publishSuccessMeta(TransactionMetaBuilder& meta, Config const&
// cfg)
//{
//     meta.setReturnValue(std::move(mReturnValue));
//     if (cfg.EMIT_SOROBAN_TRANSACTION_META_EXT_V1)
//     {
//         meta.setSorobanFeeInfo(mConsumedNonRefundableFee,
//                                mConsumedRefundableFee, mConsumedRentFee);
//     }
// }
//
// void
// SorobanTxData::publishFailureMeta(TransactionMetaBuilder& meta, Config const&
// cfg)
//{
//     if (cfg.EMIT_SOROBAN_TRANSACTION_META_EXT_V1)
//     {
//         meta.setSorobanFeeInfo(mConsumedNonRefundableFee,
//                                /* totalRefundableFeeSpent */ 0,
//                                /* rentFeeCharged */ 0);
//     }
// }

// bool
// SorobanTxData::consumeRefundableSorobanResources(
//     uint32_t contractEventSizeBytes, int64_t rentFee, uint32_t
//     protocolVersion, SorobanNetworkConfig const& sorobanConfig, Config const&
//     cfg, TransactionFrame const& tx, DiagnosticEventBuffer& diagnosticEvents)
//{
//     ZoneScoped;
//     releaseAssert(tx.isSoroban());
//     mConsumedContractEventsSizeBytes += contractEventSizeBytes;
//
//     mConsumedRentFee += rentFee;
//     mConsumedRefundableFee += rentFee;
//
//     // mFeeRefund was set in apply
//     if (mFeeRefund < mConsumedRentFee)
//     {
//         diagnosticEvents.pushSimpleDiagnosticError(
//             SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
//             "refundable resource fee was not sufficient to cover the ledger "
//             "storage rent: {} > {}",
//             {makeU64SCVal(mConsumedRentFee), makeU64SCVal(mFeeRefund)});
//         return false;
//     }
//     mFeeRefund -= mConsumedRentFee;
//
//     FeePair consumedFee = TransactionFrame::computeSorobanResourceFee(
//         protocolVersion, tx.sorobanResources(),
//         static_cast<uint32>(
//             tx.getResources(false).getVal(Resource::Type::TX_BYTE_SIZE)),
//         mConsumedContractEventsSizeBytes, sorobanConfig, cfg);
//     mConsumedRefundableFee += consumedFee.refundable_fee;
//     if (mFeeRefund < consumedFee.refundable_fee)
//     {
//         diagnosticEvents.pushSimpleDiagnosticError(
//             SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
//             "refundable resource fee was not sufficient to cover the events "
//             "fee after paying for ledger storage rent: {} > {}",
//             {makeU64SCVal(consumedFee.refundable_fee),
//              makeU64SCVal(mFeeRefund)});
//         return false;
//     }
//     mFeeRefund -= consumedFee.refundable_fee;
//     return true;
// }

bool
RefundableFeeTracker::consumeRefundableSorobanResources(
    uint32_t contractEventSizeBytes, int64_t rentFee, uint32_t protocolVersion,
    SorobanNetworkConfig const& sorobanConfig, Config const& cfg,
    TransactionFrame const& tx, DiagnosticEventBuffer& diagnosticEvents)
{
    ZoneScoped;
    releaseAssert(tx.isSoroban());
    mConsumedContractEventsSizeBytes += contractEventSizeBytes;

    mConsumedRentFee += rentFee;

    if (mMaximumRefundableFee < mConsumedRentFee)
    {
        diagnosticEvents.pushError(
            SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
            "refundable resource fee was not sufficient to cover the ledger "
            "storage rent: {} > {}",
            {makeU64SCVal(mConsumedRentFee),
             makeU64SCVal(mMaximumRefundableFee)});
        return false;
    }

    FeePair consumedFee = TransactionFrame::computeSorobanResourceFee(
        protocolVersion, tx.sorobanResources(),
        static_cast<uint32>(
            tx.getResources(false).getVal(Resource::Type::TX_BYTE_SIZE)),
        mConsumedContractEventsSizeBytes, sorobanConfig, cfg);
    mConsumedRefundableFee = mConsumedRentFee + consumedFee.refundable_fee;

    if (mMaximumRefundableFee < mConsumedRefundableFee)
    {
        diagnosticEvents.pushError(
            SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
            "refundable resource fee was not sufficient to cover the events "
            "fee after paying for ledger storage rent: {} > {}",
            {makeU64SCVal(consumedFee.refundable_fee),
             makeU64SCVal(mMaximumRefundableFee)});
        return false;
    }
    return true;
}

int64_t
RefundableFeeTracker::getFeeRefund() const
{
    return mMaximumRefundableFee - mConsumedRefundableFee;
}

int64_t
RefundableFeeTracker::getConsumedRentFee() const
{
    return mConsumedRentFee;
}

int64_t
RefundableFeeTracker::getConsumedRefundableFee() const
{
    return mConsumedRefundableFee;
}

RefundableFeeTracker::RefundableFeeTracker(int64_t maximumRefundableFee)
    : mMaximumRefundableFee(maximumRefundableFee)
{
}

void
RefundableFeeTracker::reset()
{
    mConsumedContractEventsSizeBytes = 0;
    mConsumedRentFee = 0;
    mConsumedRefundableFee = 0;
}

// MutableOperationResult::MutableOperationResult(
//     OperationResult& result,
//     std::optional<RefundableFeeTracker>& refundableFeeTracker)
//     : mResult(result), mRefundableFeeTracker(refundableFeeTracker)
//{
// }

// void
// MutableOperationResult::code(OperationResultCode code)
//{
//     mResult.code(code);
// }
//
// OperationResult::_tr_t&
// MutableOperationResult::tr() const
//{
//     return mResult.tr();
// }
//
// std::optional<RefundableFeeTracker>&
// MutableOperationResult::getRefundableFeeTracker() const
//{
//     // TODO: insert return statement here
// }

MutableTransactionResultBase::MutableTransactionResultBase(
    TransactionResultCode resultCode)
{
    mTxResult.result.code(resultCode);
    mTxResult.feeCharged = 0;
}

void
MutableTransactionResultBase::initializeRefundableFeeTracker(
    int64_t totalRefundableFee)
{
    releaseAssert(!mRefundableFeeTracker);
    mRefundableFeeTracker =
        std::make_optional(RefundableFeeTracker(totalRefundableFee));
}

std::optional<RefundableFeeTracker>&
MutableTransactionResultBase::getRefundableFeeTracker()
{
    return mRefundableFeeTracker;
}

TransactionResult const&
MutableTransactionResultBase::getXDR() const
{
    return mTxResult;
}

TransactionResultCode
MutableTransactionResultBase::getResultCode() const
{
    return mTxResult.result.code();
}

void
MutableTransactionResultBase::setResultCode(TransactionResultCode code)
{
    // Due to the way `applyCheck` is setup we set the same error code twice
    // on the same result. This is generally not necessary, but requires
    // updating `applyCheck` properly.
    releaseAssert(code == getResultCode() || isSuccess());
    mTxResult.result.code(code);
    releaseAssert(!isSuccess());
    if (mRefundableFeeTracker)
    {
        mRefundableFeeTracker->reset();
    }
}

#ifdef BUILD_TESTS
void
MutableTransactionResultBase::overrideFeeCharged(int64_t feeCharged)
{
    mTxResult.feeCharged = feeCharged;
}

void
MutableTransactionResultBase::overrideXDR(TransactionResult const& resultXDR)
{
    mTxResult = resultXDR;
}

void
MutableTransactionResultBase::setReplayTransactionResult(
    TransactionResult const& replayResult)
{
    mReplayTransactionResult.emplace(replayResult);
}

bool
MutableTransactionResultBase::adoptFailedReplayResult()
{
    if (!mReplayTransactionResult)
    {
        return false;
    }
    if (mReplayTransactionResult->result.code() == txSUCCESS ||
        mReplayTransactionResult->result.code() == txFEE_BUMP_INNER_SUCCESS)
    {
        return false;
    }
    mTxResult = *mReplayTransactionResult;
    return true;
}
bool
MutableTransactionResultBase::hasReplayTransactionResult() const
{
    return mReplayTransactionResult.has_value();
}
#endif

int64_t
MutableTransactionResultBase::getFeeCharged() const
{
    return mTxResult.feeCharged;
}

void
MutableTransactionResultBase::setInsufficientFeeErrorWithFeeCharged(
    int64_t feeCharged)
{
    setResultCode(txINSUFFICIENT_FEE);
    mTxResult.feeCharged = feeCharged;
}

MutableTransactionResult::MutableTransactionResult(
    TransactionResultCode txErrorCode)
    : MutableTransactionResultBase(txErrorCode)
{
}

MutableTransactionResult::MutableTransactionResult(TransactionFrame const& tx,
                                                   int64_t feeCharged)
    : MutableTransactionResultBase(txSUCCESS)
{
    initializeOperationResults(tx, mTxResult);
    mTxResult.feeCharged = feeCharged;
}

std::unique_ptr<MutableTransactionResult>
MutableTransactionResult::createTxError(TransactionResultCode txErrorCode)
{
    releaseAssert(txErrorCode != txSUCCESS && txErrorCode != txFAILED &&
                  txErrorCode != txFEE_BUMP_INNER_SUCCESS &&
                  txErrorCode != txFEE_BUMP_INNER_FAILED);
    return std::unique_ptr<MutableTransactionResult>(
        new MutableTransactionResult(txErrorCode));
}

std::unique_ptr<MutableTransactionResult>
MutableTransactionResult::createSuccess(TransactionFrame const& tx,
                                        int64_t feeCharged)
{
    return std::unique_ptr<MutableTransactionResult>(
        new MutableTransactionResult(tx, feeCharged));
}

TransactionResultCode
MutableTransactionResult::getInnermostResultCode() const
{
    return getResultCode();
}

void
MutableTransactionResult::setInnermostResultCode(TransactionResultCode code)
{
    setResultCode(code);
}

void
MutableTransactionResult::finalizeFeeRefund(uint32_t ledgerVersion)
{
    if (mRefundableFeeTracker)
    {
        mTxResult.feeCharged -= mRefundableFeeTracker->getFeeRefund();
    }
}

// TransactionResult const&
// MutableTransactionResult::getResult() const
//{
//     return mTxResult;
// }

OperationResult&
MutableTransactionResult::getOpResultAt(size_t index)
{
    return mTxResult.result.results().at(index);
}

bool
MutableTransactionResult::isSuccess() const
{
    return mTxResult.result.code() == txSUCCESS;
}

std::unique_ptr<MutableTransactionResultBase>
MutableTransactionResult::clone() const
{
    return std::unique_ptr<MutableTransactionResultBase>(
        new MutableTransactionResult(*this));
}

// FeeBumpMutableTransactionResult::FeeBumpMutableTransactionResult(
//     MutableTxResultPtr innerTxResult)
//     : MutableTransactionResultBase(), mInnerTxResult(innerTxResult)
//{
//     releaseAssert(mInnerTxResult);
// }
//
// FeeBumpMutableTransactionResult::FeeBumpMutableTransactionResult(
//     MutableTxResultPtr&& outerTxResult, MutableTxResultPtr&& innerTxResult,
//     TransactionFrameBasePtr innerTx)
//     : MutableTransactionResultBase(std::move(*outerTxResult))
//     , mInnerTxResult(std::move(innerTxResult))
//{
//     releaseAssert(mInnerTxResult);
//     innerTxResult.reset();
//     outerTxResult.reset();
//
//     updateResult(innerTx, *this);
// }

// void
// FeeBumpMutableTransactionResult::applyFeeRefund(int64_t feeRefund,
//                                                 uint32_t ledgerVersion)
//{
//     // First apply the refund to the inner result.
//     mInnerTxResult.applyFeeRefund(feeRefund, ledgerVersion);
//
//     // The result codes and a feeCharged without the refund should have been
//     set
//     // already in updateResult in FeeBumpTransactionFrame::apply. At this
//     point,
//     // feeCharged is set correctly on the inner transaction, so update the
//     // feeBump result.
//     if (protocolVersionStartsFrom(ledgerVersion, ProtocolVersion::V_21))
//     {
//         auto& irp = mTxResult.result.innerResultPair();
//         auto& innerRes = irp.result;
//         innerRes.feeCharged = mInnerTxResult.getResult().feeCharged;
//
//         // Now set the updated feeCharged on the fee bump.
//         mTxResult.feeCharged -= feeRefund;
//     }
// }

// void
// FeeBumpMutableTransactionResult::updateResult(
//     TransactionFrameBase const& innerTx, MutableTransactionResultBase&
//     txResult)
//{
//     if (txResult.getInnermostResult().result.code() == txSUCCESS)
//     {
//         txResult.setResultCode(txFEE_BUMP_INNER_SUCCESS);
//     }
//     else
//     {
//         txResult.setResultCode(txFEE_BUMP_INNER_FAILED);
//     }
//
//     auto& feeBumpIrp = txResult.getResult().result.innerResultPair();
//     feeBumpIrp.transactionHash = innerTx.getContentsHash();
//
//     auto const& innerTxRes = txResult.getInnermostResult();
//     auto& feeBumpIrpRes = feeBumpIrp.result;
//     feeBumpIrpRes.feeCharged = innerTxRes.feeCharged;
//     feeBumpIrpRes.result.code(innerTxRes.result.code());
//     switch (feeBumpIrpRes.result.code())
//     {
//     case txSUCCESS:
//     case txFAILED:
//         feeBumpIrpRes.result.results() = innerTxRes.result.results();
//         break;
//     default:
//         break;
//     }
// }

FeeBumpMutableTransactionResult::FeeBumpMutableTransactionResult(
    TransactionResultCode txErrorCode)
    : MutableTransactionResultBase(txErrorCode)
{
}

FeeBumpMutableTransactionResult::FeeBumpMutableTransactionResult(
    TransactionFrame const& innerTx, int64_t feeCharged,
    int64_t innerFeeCharged)
    : MutableTransactionResultBase(txFEE_BUMP_INNER_SUCCESS)
{
    mTxResult.feeCharged = feeCharged;
    auto& innerResultPair = mTxResult.result.innerResultPair();
    innerResultPair.transactionHash = innerTx.getContentsHash();
    auto& innerResult = innerResultPair.result;
    innerResult.result.code(txSUCCESS);
    innerResult.feeCharged = innerFeeCharged;
    initializeOperationResults(innerTx, innerResult);
}

InnerTransactionResult&
FeeBumpMutableTransactionResult::getInnerResult()
{
    /*releaseAssert(mTxResult.result.code() == txFEE_BUMP_INNER_SUCCESS
       || mTxResult.result.code() == txFEE_BUMP_INNER_FAILED);*/
    return mTxResult.result.innerResultPair().result;
}

InnerTransactionResult const&
FeeBumpMutableTransactionResult::getInnerResult() const
{
    /*releaseAssert(mTxResult.result.code() == txFEE_BUMP_INNER_SUCCESS
       || mTxResult.result.code() == txFEE_BUMP_INNER_FAILED);*/
    return mTxResult.result.innerResultPair().result;
}

std::unique_ptr<FeeBumpMutableTransactionResult>
FeeBumpMutableTransactionResult::createTxError(
    TransactionResultCode txErrorCode)
{
    return std::unique_ptr<FeeBumpMutableTransactionResult>(
        new FeeBumpMutableTransactionResult(txErrorCode));
}

std::unique_ptr<FeeBumpMutableTransactionResult>
FeeBumpMutableTransactionResult::createSuccess(TransactionFrame const& innerTx,
                                               int64_t feeCharged,
                                               int64_t innerFeeCharged)
{
    return std::unique_ptr<FeeBumpMutableTransactionResult>(
        new FeeBumpMutableTransactionResult(innerTx, feeCharged,
                                            innerFeeCharged));
}

TransactionResultCode
FeeBumpMutableTransactionResult::getInnermostResultCode() const
{
    return getInnerResult().result.code();
}

void
FeeBumpMutableTransactionResult::setInnermostResultCode(
    TransactionResultCode code)
{
    auto& innerRes = getInnerResult();
    releaseAssert(code != txSUCCESS && code != txFEE_BUMP_INNER_SUCCESS &&
                  code != txFEE_BUMP_INNER_FAILED &&
                  innerRes.result.code() == txSUCCESS);
    innerRes.result.code(code);
    mTxResult.result.code(txFEE_BUMP_INNER_FAILED);
    if (mRefundableFeeTracker)
    {
        mRefundableFeeTracker->reset();
    }
}

OperationResult&
FeeBumpMutableTransactionResult::getOpResultAt(size_t index)
{
    return getInnerResult().result.results().at(index);
}

bool
FeeBumpMutableTransactionResult::isSuccess() const
{
    return mTxResult.result.code() == txFEE_BUMP_INNER_SUCCESS;
}

void
FeeBumpMutableTransactionResult::finalizeFeeRefund(uint32_t protocolVersion)
{
    if (!mRefundableFeeTracker)
    {
        return;
    }
    int64_t refund = mRefundableFeeTracker->getFeeRefund();
    // The inner result always gets the refund at the moment. We might want
    // to make it 0 and thus not do the refund, but that needs to a be a
    // protocol change.
    getInnerResult().feeCharged -= refund;
    // Due to a bug, refunds were not reflected in the fee bump result prior
    // to protocol 21.
    if (protocolVersionStartsFrom(protocolVersion, ProtocolVersion::V_21))
    {
        mTxResult.feeCharged -= refund;
    }
}

std::unique_ptr<MutableTransactionResultBase>
FeeBumpMutableTransactionResult::clone() const
{
    return std::unique_ptr<MutableTransactionResultBase>(
        new FeeBumpMutableTransactionResult(*this));
}

// BUILD_TESTS
}
