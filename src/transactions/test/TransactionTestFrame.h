#pragma once

// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/ParallelApplyUtils.h"
#include "transactions/TransactionFrameBase.h"

namespace stellar
{
class TransactionTestFrame;
class ThreadParallelApplyLedgerState;
using TransactionTestFramePtr = std::shared_ptr<TransactionTestFrame>;

// The normal TransactionFrame object is immutable, and the caller needs to
// manage mutable result state via MutableTransactionResultBase. This class
// wraps an immutable TransactionFrame object with its associated mutable
// MutableTransactionResultBase for the purpose of supporting the legacy
// tests that don't use the result directly.
// Prefer using the regular transaction frame and external result explicitly
// in the new tests when possible.
class TransactionTestFrame : public TransactionFrameBase
{
  private:
    TransactionFrameBasePtr const mTransactionFrame;
    mutable MutableTxResultPtr mTransactionTxResult;

    TransactionTestFrame(TransactionFrameBasePtr tx);

  public:
    static TransactionTestFramePtr fromTxFrame(TransactionFrameBasePtr txFrame);

    virtual ~TransactionTestFrame() = default;

    // Test only functions
    bool apply(AppConnector& app, AbstractLedgerTxn& ltx,
               TransactionMetaBuilder& meta,
               Hash const& sorobanBasePrngSeed = Hash{});

    bool checkValidForTesting(AppConnector& app, AbstractLedgerTxn& ltxOuter,
                              SequenceNumber current,
                              uint64_t lowerBoundCloseTimeOffset,
                              uint64_t upperBoundCloseTimeOffset);

    void addSignature(SecretKey const& secretKey);
    void addSignature(DecoratedSignature const& signature);

    OperationResult& getOperationResultAt(size_t i) const;

    xdr::xvector<DiagnosticEvent> const& getDiagnosticEvents() const;

    TransactionFrame const& getRawTransactionFrame() const;
    TransactionFrameBasePtr getTxFramePtr() const;

    // Redefinitions of TransactionFrameBase functions
    bool apply(AppConnector& app, AbstractLedgerTxn& ltx,
               TransactionMetaBuilder& meta,
               MutableTransactionResultBase& txResult,
               Hash const& sorobanBasePrngSeed = Hash{}) const override;

    MutableTxResultPtr checkValid(AppConnector& app,
                                  AbstractLedgerTxn& ltxOuter,
                                  SequenceNumber current,
                                  uint64_t lowerBoundCloseTimeOffset,
                                  uint64_t upperBoundCloseTimeOffset) const;
    MutableTxResultPtr checkValid(AppConnector& app, LedgerSnapshot const& ls,
                                  SequenceNumber current,
                                  uint64_t lowerBoundCloseTimeOffset,
                                  uint64_t upperBoundCloseTimeOffset) const;
    MutableTxResultPtr
    checkValid(AppConnector& app, LedgerSnapshot const& ls,
               SequenceNumber current, uint64_t lowerBoundCloseTimeOffset,
               uint64_t upperBoundCloseTimeOffset,
               DiagnosticEventManager& diagnosticEvents) const override;
    bool checkSorobanResources(
        SorobanNetworkConfig const& cfg, uint32_t ledgerVersion,
        DiagnosticEventManager& diagnosticEvents) const override;

    MutableTxResultPtr
    createTxErrorResult(TransactionResultCode txErrorCode) const override;

    MutableTxResultPtr createValidationSuccessResult() const override;

    TransactionEnvelope const& getEnvelope() const override;
    TransactionEnvelope& getMutableEnvelope() const override;

    // clear pre-computed hashes
    void clearCached() const override;

    // Returns the total fee of this transaction, including the 'flat',
    // non-market part.
    int64_t getFullFee() const override;
    // Returns the part of the full fee used to make decisions as to
    // whether this transaction should be included into ledger.
    int64_t getInclusionFee() const override;
    int64_t getFee(LedgerHeader const& header, std::optional<int64_t> baseFee,
                   bool applying) const override;

    bool checkSignature(SignatureChecker& signatureChecker,
                        LedgerEntryWrapper const& account,
                        int32_t neededWeight) const override;

    Hash const& getContentsHash() const override;
    Hash const& getFullHash() const override;

    uint32_t getNumOperations() const override;
    std::vector<std::shared_ptr<OperationFrame const>> const&
    getOperationFrames() const override;
    Resource getResources(bool useByteLimitInClassic,
                          uint32_t ledgerVersion) const override;

    std::vector<Operation> const& getRawOperations() const override;

    TransactionResult const& getResult() const;
    TransactionResultCode getResultCode() const;

    SequenceNumber getSeqNum() const override;
    AccountID getFeeSourceID() const override;
    AccountID getSourceID() const override;
    std::optional<SequenceNumber const> const getMinSeqNum() const override;
    Duration getMinSeqAge() const override;
    uint32 getMinSeqLedgerGap() const override;

    void
    insertKeysForFeeProcessing(UnorderedSet<LedgerKey>& keys) const override;
    void insertKeysForTxApply(UnorderedSet<LedgerKey>& keys) const override;

    void preParallelApply(AppConnector& app, AbstractLedgerTxn& ltx,
                          TransactionMetaBuilder& meta,
                          MutableTransactionResultBase& resPayload,
                          bool chargeFee) const;

    void
    preParallelApply(AppConnector& app, AbstractLedgerTxn& ltx,
                     TransactionMetaBuilder& meta,
                     MutableTransactionResultBase& resPayload) const override;

    ParallelTxReturnVal parallelApply(
        AppConnector& app, ThreadParallelApplyLedgerState const& threadState,
        Config const& config, SorobanNetworkConfig const& sorobanConfig,
        ParallelLedgerInfo const& ledgerInfo,
        MutableTransactionResultBase& resPayload,
        SorobanMetrics& sorobanMetrics, Hash const& sorobanBasePrngSeed,
        TxEffects& effects) const override;

    MutableTxResultPtr
    processFeeSeqNum(AbstractLedgerTxn& ltx,
                     std::optional<int64_t> baseFee) const override;

    void
    processPostApply(AppConnector& app, AbstractLedgerTxn& ltx,
                     TransactionMetaBuilder& meta,
                     MutableTransactionResultBase& txResult) const override;

    void processPostTxSetApply(AppConnector& app, AbstractLedgerTxn& ltx,
                               MutableTransactionResultBase& txResult,
                               TxEventManager& txEventManager) const override;

    std::shared_ptr<StellarMessage const> toStellarMessage() const override;

    bool hasDexOperations() const override;

    bool isSoroban() const override;
    SorobanResources const& sorobanResources() const override;
    SorobanTransactionData::_ext_t const& getResourcesExt() const override;
    int64 declaredSorobanResourceFee() const override;
    bool XDRProvidesValidFee() const override;
    bool isRestoreFootprintTx() const override;

    bool
    isTestTx() const override
    {
        return true;
    }

    void overrideResult(MutableTxResultPtr result);
    void overrideResultXDR(TransactionResult const& resultXDR);
    void overrideResultFeeCharged(int64_t feeCharged);
};
}
