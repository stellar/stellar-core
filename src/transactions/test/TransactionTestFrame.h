#pragma once

// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/TransactionFrameBase.h"

namespace stellar
{
class TransactionTestFrame;
using TransactionTestFramePtr = std::shared_ptr<TransactionTestFrame>;

// The normal TransactionFrame object is immutable, and the caller needs to
// manage mutable result state via MutableTransactionResultBase. This class
// wraps an immutable TransactionFrame object with its associated mutable
// MutableTransactionResultBase for testing purposes.
class TransactionTestFrame : public TransactionFrameBase
{
  private:
    TransactionFrameBasePtr const mTransactionFrame;
    mutable TransactionResultPayloadPtr mTransactionResultPayload;

    TransactionTestFrame(TransactionFrameBasePtr tx);

  public:
    static TransactionTestFramePtr fromTxFrame(TransactionFrameBasePtr txFrame);

    virtual ~TransactionTestFrame()
    {
    }

    // Test only functions
    bool apply(Application& app, AbstractLedgerTxn& ltx,
               TransactionMetaFrame& meta,
               Hash const& sorobanBasePrngSeed = Hash{});

    bool checkValidForTesting(Application& app, AbstractLedgerTxn& ltxOuter,
                              SequenceNumber current,
                              uint64_t lowerBoundCloseTimeOffset,
                              uint64_t upperBoundCloseTimeOffset);

    void processFeeSeqNum(AbstractLedgerTxn& ltx,
                          std::optional<int64_t> baseFee);

    void processPostApply(Application& app, AbstractLedgerTxn& ltx,
                          TransactionMetaFrame& meta);

    void addSignature(SecretKey const& secretKey);
    void addSignature(DecoratedSignature const& signature);

    OperationResult& getOperationResultAt(size_t i) const;

    xdr::xvector<DiagnosticEvent> const& getDiagnosticEvents() const;

    TransactionFrame const& getRawTransactionFrame() const;
    TransactionFrameBasePtr getTxFramePtr() const;

    // Redefinitions of TransactionFrameBase functions
    bool apply(Application& app, AbstractLedgerTxn& ltx,
               TransactionMetaFrame& meta, TransactionResultPayloadPtr txResult,
               Hash const& sorobanBasePrngSeed = Hash{}) const override;

    std::pair<bool, TransactionResultPayloadPtr>
    checkValid(Application& app, AbstractLedgerTxn& ltxOuter,
               SequenceNumber current, uint64_t lowerBoundCloseTimeOffset,
               uint64_t upperBoundCloseTimeOffset) const override;
    bool checkSorobanResourceAndSetError(
        Application& app, uint32_t ledgerVersion,
        TransactionResultPayloadPtr txResult) const override;

    TransactionResultPayloadPtr createResultPayload() const override;

    TransactionResultPayloadPtr
    createResultPayloadWithFeeCharged(LedgerHeader const& header,
                                      std::optional<int64_t> baseFee,
                                      bool applying) const override;

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

    Hash const& getContentsHash() const override;
    Hash const& getFullHash() const override;

    uint32_t getNumOperations() const override;
    Resource getResources(bool useByteLimitInClassic) const override;

    std::vector<Operation> const& getRawOperations() const override;

    TransactionResult& getResult();
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

    TransactionResultPayloadPtr
    processFeeSeqNum(AbstractLedgerTxn& ltx,
                     std::optional<int64_t> baseFee) const override;

    void processPostApply(Application& app, AbstractLedgerTxn& ltx,
                          TransactionMetaFrame& meta,
                          TransactionResultPayloadPtr txResult) const override;

    std::shared_ptr<StellarMessage const> toStellarMessage() const override;

    bool hasDexOperations() const override;

    bool isSoroban() const override;
    SorobanResources const& sorobanResources() const override;
    int64 declaredSorobanResourceFee() const override;
    bool XDRProvidesValidFee() const override;

    bool
    isTestTx() const override
    {
        return true;
    }
};
}