// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/test/TransactionTestFrame.h"
#include "transactions/MutableTransactionResult.h"
#include "transactions/SignatureUtils.h"
#include "transactions/TransactionBridge.h"
#include "transactions/TransactionFrame.h"

namespace stellar
{
TransactionTestFrame::TransactionTestFrame(TransactionFrameBasePtr tx)
    : mTransactionFrame(tx), mTransactionTxResult(tx->createSuccessResult())
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

MutableTxResultPtr
TransactionTestFrame::createSuccessResultWithFeeCharged(
    LedgerHeader const& header, std::optional<int64_t> baseFee,
    bool applying) const
{
    return mTransactionFrame->createSuccessResultWithFeeCharged(header, baseFee,
                                                                applying);
}

MutableTxResultPtr
TransactionTestFrame::createSuccessResult() const
{
    return mTransactionFrame->createSuccessResult();
}

bool
TransactionTestFrame::apply(AppConnector& app, AbstractLedgerTxn& ltx,
                            TransactionMetaFrame& meta,
                            Hash const& sorobanBasePrngSeed)
{
    return mTransactionFrame->apply(app, ltx, meta, mTransactionTxResult,
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
                            TransactionMetaFrame& meta,
                            MutableTxResultPtr txResult,
                            Hash const& sorobanBasePrngSeed) const
{
    auto ret =
        mTransactionFrame->apply(app, ltx, meta, txResult, sorobanBasePrngSeed);
    mTransactionTxResult = txResult;
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
    mTransactionTxResult = mTransactionFrame->checkValid(
        app, ls, current, lowerBoundCloseTimeOffset, upperBoundCloseTimeOffset);
    return mTransactionTxResult;
}

MutableTxResultPtr
TransactionTestFrame::checkValid(AppConnector& app, LedgerSnapshot const& ls,
                                 SequenceNumber current,
                                 uint64_t lowerBoundCloseTimeOffset,
                                 uint64_t upperBoundCloseTimeOffset) const
{
    mTransactionTxResult = mTransactionFrame->checkValid(
        app, ls, current, lowerBoundCloseTimeOffset, upperBoundCloseTimeOffset);
    return mTransactionTxResult;
}

void
TransactionTestFrame::processFeeSeqNum(AbstractLedgerTxn& ltx,
                                       std::optional<int64_t> baseFee)
{
    mTransactionTxResult = mTransactionFrame->processFeeSeqNum(ltx, baseFee);
}

void
TransactionTestFrame::processPostApply(AppConnector& app,
                                       AbstractLedgerTxn& ltx,
                                       TransactionMetaFrame& meta)
{
    mTransactionFrame->processPostApply(app, ltx, meta, mTransactionTxResult);
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
TransactionTestFrame::checkSorobanResourceAndSetError(
    AppConnector& app, SorobanNetworkConfig const& cfg, uint32_t ledgerVersion,
    MutableTxResultPtr txResult) const
{
    auto ret = mTransactionFrame->checkSorobanResourceAndSetError(
        app, cfg, ledgerVersion, txResult);
    mTransactionTxResult = txResult;
    return ret;
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

Resource
TransactionTestFrame::getResources(bool useByteLimitInClassic) const
{
    return mTransactionFrame->getResources(useByteLimitInClassic);
}

std::vector<Operation> const&
TransactionTestFrame::getRawOperations() const
{
    return mTransactionFrame->getRawOperations();
}

TransactionResult&
TransactionTestFrame::getResult()
{
    return mTransactionTxResult->getResult();
}

TransactionResultCode
TransactionTestFrame::getResultCode() const
{
    return mTransactionTxResult->getResult().result.code();
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
TransactionTestFrame::insertKeysForTxApply(UnorderedSet<LedgerKey>& keys,
                                           LedgerKeyMeter* lkMeter) const
{
    mTransactionFrame->insertKeysForTxApply(keys, lkMeter);
}

MutableTxResultPtr
TransactionTestFrame::processFeeSeqNum(AbstractLedgerTxn& ltx,
                                       std::optional<int64_t> baseFee) const
{
    mTransactionTxResult = mTransactionFrame->processFeeSeqNum(ltx, baseFee);
    return mTransactionTxResult;
}

void
TransactionTestFrame::processPostApply(AppConnector& app,
                                       AbstractLedgerTxn& ltx,
                                       TransactionMetaFrame& meta,
                                       MutableTxResultPtr txResult) const
{
    mTransactionFrame->processPostApply(app, ltx, meta, txResult);
    mTransactionTxResult = txResult;
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

xdr::xvector<DiagnosticEvent> const&
TransactionTestFrame::getDiagnosticEvents() const
{
    return mTransactionTxResult->getDiagnosticEvents();
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
}