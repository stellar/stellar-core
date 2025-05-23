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

bool
RefundableFeeTracker::consumeRefundableSorobanResources(
    uint32_t contractEventSizeBytes, int64_t rentFee, uint32_t protocolVersion,
    SorobanNetworkConfig const& sorobanConfig, Config const& cfg,
    TransactionFrame const& tx, DiagnosticEventManager& diagnosticEvents)
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
        static_cast<uint32>(tx.getResources(false, protocolVersion)
                                .getVal(Resource::Type::TX_BYTE_SIZE)),
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
RefundableFeeTracker::resetConsumedFee()
{
    mConsumedContractEventsSizeBytes = 0;
    mConsumedRentFee = 0;
    mConsumedRefundableFee = 0;
}

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
MutableTransactionResultBase::setError(TransactionResultCode code)
{
    // Due to the way `applyCheck` is setup we set the same error code twice
    // on the same result. This is generally not necessary, but requires
    // updating `applyCheck` properly.
    releaseAssert(code == getResultCode() || isSuccess());
    // Note: changing "code" normally causes the XDR structure to be destructed,
    // then a different XDR structure is constructed. However,
    // txSUCCESS/txFAILED and txFEE_BUMP_INNER_SUCCESS/txFEE_BUMP_INNER_FAILED
    // result pairs have the same underlying field number so this does not occur
    // when changing between these codes.
    mTxResult.result.code(code);
    releaseAssert(!isSuccess());
    if (mRefundableFeeTracker)
    {
        mRefundableFeeTracker->resetConsumedFee();
    }
}

int64_t
MutableTransactionResultBase::getFeeCharged() const
{
    return mTxResult.feeCharged;
}

#ifdef BUILD_TESTS

std::unique_ptr<MutableTransactionResultBase>
MutableTransactionResult::clone() const
{
    return std::unique_ptr<MutableTransactionResultBase>(
        new MutableTransactionResult(*this));
}

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
    if (mRefundableFeeTracker)
    {
        mRefundableFeeTracker->resetConsumedFee();
    }
    return true;
}
bool
MutableTransactionResultBase::hasReplayTransactionResult() const
{
    return mReplayTransactionResult.has_value();
}
#endif

void
MutableTransactionResultBase::setInsufficientFeeErrorWithFeeCharged(
    int64_t feeCharged)
{
    setError(txINSUFFICIENT_FEE);
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
MutableTransactionResult::setInnermostError(TransactionResultCode code)
{
    setError(code);
}

void
MutableTransactionResult::finalizeFeeRefund(uint32_t ledgerVersion)
{
    if (mRefundableFeeTracker)
    {
        mTxResult.feeCharged -= mRefundableFeeTracker->getFeeRefund();
    }
}

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
    releaseAssert(mTxResult.result.code() == txFEE_BUMP_INNER_SUCCESS ||
                  mTxResult.result.code() == txFEE_BUMP_INNER_FAILED);
    return mTxResult.result.innerResultPair().result;
}

InnerTransactionResult const&
FeeBumpMutableTransactionResult::getInnerResult() const
{
    releaseAssert(mTxResult.result.code() == txFEE_BUMP_INNER_SUCCESS ||
                  mTxResult.result.code() == txFEE_BUMP_INNER_FAILED);
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
FeeBumpMutableTransactionResult::setInnermostError(TransactionResultCode code)
{
    auto& innerRes = getInnerResult();
    releaseAssert(code != txSUCCESS && code != txFEE_BUMP_INNER_SUCCESS &&
                  code != txFEE_BUMP_INNER_FAILED &&
                  innerRes.result.code() == txSUCCESS);
    innerRes.result.code(code);
    mTxResult.result.code(txFEE_BUMP_INNER_FAILED);
    if (mRefundableFeeTracker)
    {
        mRefundableFeeTracker->resetConsumedFee();
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
    // Due to a bug, refunds were not reflected in the fee bump result prior
    // to protocol 21.
    if (protocolVersionStartsFrom(protocolVersion, ProtocolVersion::V_21))
    {
        int64_t refund = mRefundableFeeTracker->getFeeRefund();
        mTxResult.feeCharged -= refund;
        // This shouldn't be necessary as the inner result should always have 0
        // fee. However, in fact we do populate the inner `feeCharged` field
        // for the fee bump transactions and thus we also need to apply the
        // refund to it.
        // Changing this is a protocol change.
        getInnerResult().feeCharged -= refund;
    }
}

#ifdef BUILD_TESTS
std::unique_ptr<MutableTransactionResultBase>
FeeBumpMutableTransactionResult::clone() const
{
    return std::unique_ptr<MutableTransactionResultBase>(
        new FeeBumpMutableTransactionResult(*this));
}
#endif
}
