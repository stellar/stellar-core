// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/test/TransactionTestFrame.h"
#include "transactions/EventManager.h"
#include "transactions/MutableTransactionResult.h"
#include "transactions/SignatureUtils.h"
#include "transactions/TransactionBridge.h"
#include "transactions/TransactionFrame.h"
#include <thread>

namespace stellar
{
class ThreadParallelApplyLedgerState;
TransactionTestFrame::TransactionTestFrame(TransactionFrameBasePtr tx)
    : mTransactionFrame(tx)
    , mTransactionTxResult(tx->createValidationSuccessResult())
{
    releaseAssert(mTransactionFrame);
    releaseAssert(!mTransactionFrame->isTestTx());
}

TransactionTestFramePtr
TransactionTestFrame::fromTxFrame(TransactionFrameBasePtr txFrame)
{
    releaseAssert(txFrame);
    releaseAssert(!txFrame->isTestTx());
    return std::shared_ptr<TransactionTestFrame>(
        new TransactionTestFrame(txFrame));
}

bool
TransactionTestFrame::apply(AppConnector& app, AbstractLedgerTxn& ltx,
                            TransactionMetaBuilder& meta,
                            Hash const& sorobanBasePrngSeed)
{
    return mTransactionFrame->apply(app, ltx, meta, *mTransactionTxResult,
                                    sorobanBasePrngSeed);
}

void
TransactionTestFrame::clearCached() const
{
    mTransactionFrame->clearCached();
}

OperationResult&
TransactionTestFrame::getOperationResultAt(size_t i) const
{
    return mTransactionTxResult->getOpResultAt(i);
}

void
TransactionTestFrame::addSignature(SecretKey const& secretKey)
{
    auto sig = SignatureUtils::sign(secretKey, getContentsHash());
    addSignature(sig);
}

void
TransactionTestFrame::addSignature(DecoratedSignature const& signature)
{
    clearCached();
    txbridge::getSignatures(getMutableEnvelope()).push_back(signature);
}

bool
TransactionTestFrame::apply(AppConnector& app, AbstractLedgerTxn& ltx,
                            TransactionMetaBuilder& meta,
                            MutableTransactionResultBase& txResult,
                            Hash const& sorobanBasePrngSeed) const
{
    auto ret =
        mTransactionFrame->apply(app, ltx, meta, txResult, sorobanBasePrngSeed);
    mTransactionTxResult = txResult.clone();
    return ret;
}

MutableTxResultPtr
TransactionTestFrame::checkValid(AppConnector& app, AbstractLedgerTxn& ltxOuter,
                                 SequenceNumber current,
                                 uint64_t lowerBoundCloseTimeOffset,
                                 uint64_t upperBoundCloseTimeOffset) const
{
    LedgerTxn ltx(ltxOuter);
    auto ls = LedgerSnapshot(ltx);
    auto diagnostics = DiagnosticEventManager::createDisabled();
    mTransactionTxResult = mTransactionFrame->checkValid(
        app, ls, current, lowerBoundCloseTimeOffset, upperBoundCloseTimeOffset,
        diagnostics);
    return mTransactionTxResult->clone();
}

MutableTxResultPtr
TransactionTestFrame::checkValid(AppConnector& app, LedgerSnapshot const& ls,
                                 SequenceNumber current,
                                 uint64_t lowerBoundCloseTimeOffset,
                                 uint64_t upperBoundCloseTimeOffset) const
{
    auto diagnostics = DiagnosticEventManager::createDisabled();
    return checkValid(app, ls, current, lowerBoundCloseTimeOffset,
                      upperBoundCloseTimeOffset, diagnostics);
}

MutableTxResultPtr
TransactionTestFrame::checkValid(AppConnector& app, LedgerSnapshot const& ls,
                                 SequenceNumber current,
                                 uint64_t lowerBoundCloseTimeOffset,
                                 uint64_t upperBoundCloseTimeOffset,
                                 DiagnosticEventManager& diagnosticEvents) const
{
    mTransactionTxResult = mTransactionFrame->checkValid(
        app, ls, current, lowerBoundCloseTimeOffset, upperBoundCloseTimeOffset,
        diagnosticEvents);
    return mTransactionTxResult->clone();
}

bool
TransactionTestFrame::checkValidForTesting(AppConnector& app,
                                           AbstractLedgerTxn& ltxOuter,
                                           SequenceNumber current,
                                           uint64_t lowerBoundCloseTimeOffset,
                                           uint64_t upperBoundCloseTimeOffset)
{
    mTransactionTxResult =
        checkValid(app, ltxOuter, current, lowerBoundCloseTimeOffset,
                   upperBoundCloseTimeOffset);
    return mTransactionTxResult->isSuccess();
}

bool
TransactionTestFrame::checkSorobanResources(
    SorobanNetworkConfig const& cfg, uint32_t ledgerVersion,
    DiagnosticEventManager& diagnosticEvents) const
{
    return mTransactionFrame->checkSorobanResources(cfg, ledgerVersion,
                                                    diagnosticEvents);
}

MutableTxResultPtr
TransactionTestFrame::createTxErrorResult(
    TransactionResultCode txErrorCode) const
{
    return mTransactionFrame->createTxErrorResult(txErrorCode);
}

MutableTxResultPtr
TransactionTestFrame::createValidationSuccessResult() const
{
    return mTransactionFrame->createValidationSuccessResult();
}

TransactionEnvelope const&
TransactionTestFrame::getEnvelope() const
{
    return mTransactionFrame->getEnvelope();
}

TransactionEnvelope&
TransactionTestFrame::getMutableEnvelope() const
{
    return mTransactionFrame->getMutableEnvelope();
}

TransactionFrame const&
TransactionTestFrame::getRawTransactionFrame() const
{
    auto ret =
        std::dynamic_pointer_cast<TransactionFrame const>(mTransactionFrame);
    releaseAssertOrThrow(ret);
    return *ret;
}

TransactionFrameBasePtr
TransactionTestFrame::getTxFramePtr() const
{
    return mTransactionFrame;
}

int64_t
TransactionTestFrame::getFullFee() const
{
    return mTransactionFrame->getFullFee();
}

int64_t
TransactionTestFrame::getInclusionFee() const
{
    return mTransactionFrame->getInclusionFee();
}

int64_t
TransactionTestFrame::getFee(LedgerHeader const& header,
                             std::optional<int64_t> baseFee,
                             bool applying) const
{
    return mTransactionFrame->getFee(header, baseFee, applying);
}

bool
TransactionTestFrame::checkSignature(SignatureChecker& signatureChecker,
                                     LedgerEntryWrapper const& account,
                                     int32_t neededWeight) const
{
    return mTransactionFrame->checkSignature(signatureChecker, account,
                                             neededWeight);
}

Hash const&
TransactionTestFrame::getContentsHash() const
{
    return mTransactionFrame->getContentsHash();
}

Hash const&
TransactionTestFrame::getFullHash() const
{
    return mTransactionFrame->getFullHash();
}

uint32_t
TransactionTestFrame::getNumOperations() const
{
    return mTransactionFrame->getNumOperations();
}

std::vector<std::shared_ptr<OperationFrame const>> const&
TransactionTestFrame::getOperationFrames() const
{
    return mTransactionFrame->getOperationFrames();
}

Resource
TransactionTestFrame::getResources(bool useByteLimitInClassic,
                                   uint32_t ledgerVersion) const
{
    return mTransactionFrame->getResources(useByteLimitInClassic,
                                           ledgerVersion);
}

std::vector<Operation> const&
TransactionTestFrame::getRawOperations() const
{
    return mTransactionFrame->getRawOperations();
}

TransactionResult const&
TransactionTestFrame::getResult() const
{
    return mTransactionTxResult->getXDR();
}

TransactionResultCode
TransactionTestFrame::getResultCode() const
{
    return mTransactionTxResult->getResultCode();
}

SequenceNumber
TransactionTestFrame::getSeqNum() const
{
    return mTransactionFrame->getSeqNum();
}

AccountID
TransactionTestFrame::getFeeSourceID() const
{
    return mTransactionFrame->getFeeSourceID();
}

AccountID
TransactionTestFrame::getSourceID() const
{
    return mTransactionFrame->getSourceID();
}

std::optional<SequenceNumber const> const
TransactionTestFrame::getMinSeqNum() const
{
    return mTransactionFrame->getMinSeqNum();
}

Duration
TransactionTestFrame::getMinSeqAge() const
{
    return mTransactionFrame->getMinSeqAge();
}

uint32
TransactionTestFrame::getMinSeqLedgerGap() const
{
    return mTransactionFrame->getMinSeqLedgerGap();
}

void
TransactionTestFrame::insertKeysForFeeProcessing(
    UnorderedSet<LedgerKey>& keys) const
{
    mTransactionFrame->insertKeysForFeeProcessing(keys);
}

void
TransactionTestFrame::insertKeysForTxApply(UnorderedSet<LedgerKey>& keys) const
{
    mTransactionFrame->insertKeysForTxApply(keys);
}

void
TransactionTestFrame::preParallelApply(
    AppConnector& app, AbstractLedgerTxn& ltx, TransactionMetaBuilder& meta,
    MutableTransactionResultBase& resPayload) const
{
    mTransactionFrame->preParallelApply(app, ltx, meta, resPayload);
}

ParallelTxReturnVal
TransactionTestFrame::parallelApply(
    AppConnector& app, ThreadParallelApplyLedgerState const& threadState,
    Config const& config, SorobanNetworkConfig const& sorobanConfig,
    ParallelLedgerInfo const& ledgerInfo,
    MutableTransactionResultBase& resPayload, SorobanMetrics& sorobanMetrics,
    Hash const& txPrngSeed, TxEffects& effects) const
{
    return mTransactionFrame->parallelApply(
        app, threadState, config, sorobanConfig, ledgerInfo, resPayload,
        sorobanMetrics, txPrngSeed, effects);
}

MutableTxResultPtr
TransactionTestFrame::processFeeSeqNum(AbstractLedgerTxn& ltx,
                                       std::optional<int64_t> baseFee) const
{
    mTransactionTxResult = mTransactionFrame->processFeeSeqNum(ltx, baseFee);
    return mTransactionTxResult->clone();
}

void
TransactionTestFrame::processPostApply(
    AppConnector& app, AbstractLedgerTxn& ltx, TransactionMetaBuilder& meta,
    MutableTransactionResultBase& txResult) const
{
    mTransactionFrame->processPostApply(app, ltx, meta, txResult);
    mTransactionTxResult = txResult.clone();
}

void
TransactionTestFrame::processPostTxSetApply(
    AppConnector& app, AbstractLedgerTxn& ltx,
    MutableTransactionResultBase& txResult,
    TxEventManager& txEventManager) const
{
    mTransactionFrame->processPostTxSetApply(app, ltx, txResult,
                                             txEventManager);
}

std::shared_ptr<StellarMessage const>
TransactionTestFrame::toStellarMessage() const
{
    return mTransactionFrame->toStellarMessage();
}

bool
TransactionTestFrame::hasDexOperations() const
{
    return mTransactionFrame->hasDexOperations();
}

bool
TransactionTestFrame::isSoroban() const
{
    return mTransactionFrame->isSoroban();
}

SorobanResources const&
TransactionTestFrame::sorobanResources() const
{
    return mTransactionFrame->sorobanResources();
}

SorobanTransactionData::_ext_t const&
TransactionTestFrame::getResourcesExt() const
{
    return mTransactionFrame->getResourcesExt();
}

int64
TransactionTestFrame::declaredSorobanResourceFee() const
{
    return mTransactionFrame->declaredSorobanResourceFee();
}

bool
TransactionTestFrame::XDRProvidesValidFee() const
{
    return mTransactionFrame->XDRProvidesValidFee();
}

bool
TransactionTestFrame::isRestoreFootprintTx() const
{
    return mTransactionFrame->isRestoreFootprintTx();
}

void
TransactionTestFrame::overrideResult(MutableTxResultPtr result)
{
    mTransactionTxResult = std::move(result);
}

void
TransactionTestFrame::overrideResultXDR(TransactionResult const& resultXDR)
{
    mTransactionTxResult->overrideXDR(resultXDR);
}

void
TransactionTestFrame::overrideResultFeeCharged(int64_t feeCharged)
{
    mTransactionTxResult->overrideFeeCharged(feeCharged);
}
}
