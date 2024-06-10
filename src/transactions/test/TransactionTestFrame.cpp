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
    : mTransactionFrame(tx)
    , mTransactionResultPayload(tx->createResultPayload())
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

TransactionResultPayloadPtr
TransactionTestFrame::createResultPayloadWithFeeCharged(
    LedgerHeader const& header, std::optional<int64_t> baseFee,
    bool applying) const
{
    return mTransactionFrame->createResultPayloadWithFeeCharged(header, baseFee,
                                                                applying);
}

TransactionResultPayloadPtr
TransactionTestFrame::createResultPayload() const
{
    return mTransactionFrame->createResultPayload();
}

bool
TransactionTestFrame::apply(Application& app, AbstractLedgerTxn& ltx,
                            TransactionMetaFrame& meta,
                            Hash const& sorobanBasePrngSeed)
{
    return mTransactionFrame->apply(app, ltx, meta, mTransactionResultPayload,
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
    return mTransactionResultPayload->getOpResultAt(i);
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
TransactionTestFrame::apply(Application& app, AbstractLedgerTxn& ltx,
                            TransactionMetaFrame& meta,
                            TransactionResultPayloadPtr txResult,
                            Hash const& sorobanBasePrngSeed) const
{
    auto ret =
        mTransactionFrame->apply(app, ltx, meta, txResult, sorobanBasePrngSeed);
    mTransactionResultPayload = txResult;
    return ret;
}

std::pair<bool, TransactionResultPayloadPtr>
TransactionTestFrame::checkValid(Application& app, AbstractLedgerTxn& ltxOuter,
                                 SequenceNumber current,
                                 uint64_t lowerBoundCloseTimeOffset,
                                 uint64_t upperBoundCloseTimeOffset) const
{
    auto result = mTransactionFrame->checkValid(app, ltxOuter, current,
                                                lowerBoundCloseTimeOffset,
                                                upperBoundCloseTimeOffset);
    mTransactionResultPayload = result.second;
    return result;
}

void
TransactionTestFrame::processFeeSeqNum(AbstractLedgerTxn& ltx,
                                       std::optional<int64_t> baseFee)
{
    mTransactionResultPayload =
        mTransactionFrame->processFeeSeqNum(ltx, baseFee);
}

void
TransactionTestFrame::processPostApply(Application& app, AbstractLedgerTxn& ltx,
                                       TransactionMetaFrame& meta)
{
    mTransactionFrame->processPostApply(app, ltx, meta,
                                        mTransactionResultPayload);
}

bool
TransactionTestFrame::checkValidForTesting(Application& app,
                                           AbstractLedgerTxn& ltxOuter,
                                           SequenceNumber current,
                                           uint64_t lowerBoundCloseTimeOffset,
                                           uint64_t upperBoundCloseTimeOffset)
{
    bool res;
    std::tie(res, mTransactionResultPayload) =
        checkValid(app, ltxOuter, current, lowerBoundCloseTimeOffset,
                   upperBoundCloseTimeOffset);
    return res;
}

bool
TransactionTestFrame::checkSorobanResourceAndSetError(
    Application& app, uint32_t ledgerVersion,
    TransactionResultPayloadPtr txResult) const
{
    auto ret = mTransactionFrame->checkSorobanResourceAndSetError(
        app, ledgerVersion, txResult);
    mTransactionResultPayload = txResult;
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
    return mTransactionResultPayload->getResult();
}

TransactionResultCode
TransactionTestFrame::getResultCode() const
{
    return mTransactionResultPayload->getResult().result.code();
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

TransactionResultPayloadPtr
TransactionTestFrame::processFeeSeqNum(AbstractLedgerTxn& ltx,
                                       std::optional<int64_t> baseFee) const
{
    mTransactionResultPayload =
        mTransactionFrame->processFeeSeqNum(ltx, baseFee);
    return mTransactionResultPayload;
}

void
TransactionTestFrame::processPostApply(
    Application& app, AbstractLedgerTxn& ltx, TransactionMetaFrame& meta,
    TransactionResultPayloadPtr txResult) const
{
    mTransactionFrame->processPostApply(app, ltx, meta, txResult);
    mTransactionResultPayload = txResult;
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
    return mTransactionResultPayload->getDiagnosticEvents();
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