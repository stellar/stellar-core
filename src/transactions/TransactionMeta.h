#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTxn.h"
#include "transactions/EventManager.h"
#include "transactions/MutableTransactionResult.h"
#include "xdr/Stellar-ledger.h"
#include <variant>

namespace stellar
{

// The builder class for OperationMeta. This can only be instantiated by
// TransactionMetaBuilder and accessed via `getOperationMetaBuilderAt`.
class OperationMetaBuilder
{
  public:
    // Sets all the ledger changes caused by this operation from the provided
    // ledger transaction used for this operation.
    void setLedgerChanges(AbstractLedgerTxn& opLtx, uint32_t ledgerSeq);

    // Similar to the above function, but used during parallel apply, which uses
    // thread state and return value maps to track entry changes.
    void setLedgerChangesFromSuccessfulOp(
        ThreadParallelApplyLedgerState const& threadState,
        ParallelTxReturnVal const& res, uint32_t ledgerSeq);

    // Sets the return value for a Soroban operation.
    void setSorobanReturnValue(SCVal const& val);
    // Returns the event manager for this operation.
    OpEventManager& getEventManager();
    // Returns the diagnostic event manager for the transaction.
    // There is only one diagnstic event manager for the transaction, so this
    // is just provided for convenience.
    DiagnosticEventManager& getDiagnosticEventManager();

  private:
    friend class TransactionMetaBuilder;

    OperationMetaBuilder(Config const& cfg, bool metaEnabled,
                         OperationMeta& meta, OperationFrame const& op,
                         uint32_t protocolVersion, Hash const& networkID,
                         Config const& config,
                         DiagnosticEventManager& diagnosticEventManager);
    OperationMetaBuilder(Config const& cfg, bool metaEnabled,
                         OperationMetaV2& meta, OperationFrame const& op,
                         uint32_t protocolVersion, Hash const& networkID,
                         Config const& config,
                         DiagnosticEventManager& diagnosticEventManager);
    bool maybeFinalizeOpEvents();

    std::optional<SCVal> mSorobanReturnValue{};

    bool mEnabled = false;
    uint32_t mProtocolVersion = 0;
    OperationFrame const& mOp;
    std::variant<std::reference_wrapper<OperationMeta>,
                 std::reference_wrapper<OperationMetaV2>>
        mMeta;
    OpEventManager mEventManager;
    DiagnosticEventManager& mDiagnosticEventManager;
    Config const& mConfig;
};

#ifdef BUILD_TESTS
// Test-only view container of TransactionMeta XDR. Outside of tests
// TransactionMeta is write-only and thus doesn't need a view.
class TransactionMetaFrame
{
  public:
    TransactionMetaFrame() = default;
    explicit TransactionMetaFrame(TransactionMeta const& meta);

    size_t getNumOperations() const;
    size_t getNumChangesBefore() const;
    LedgerEntryChanges getChangesBefore() const;
    LedgerEntryChanges getChangesAfter() const;
    SCVal const& getReturnValue() const;
    bool eventsAreSupported() const;
    xdr::xvector<TransactionEvent> const& getTxEvents() const;
    xdr::xvector<DiagnosticEvent> const& getDiagnosticEvents() const;
    xdr::xvector<ContractEvent> const& getOpEventsAtOp(size_t opIdx) const;
    xdr::xvector<ContractEvent> getSorobanContractEvents() const;
    LedgerEntryChanges const& getLedgerEntryChangesAtOp(size_t opIdx) const;
    TransactionMeta const& getXDR() const;

  private:
    TransactionMeta mTransactionMeta;
};
#endif

// Builder class for transaction's meta (TransactionMeta XDR).
// This encapsulates all the logic for routing the meta information into proper
// XDR fields, as well as control over which parts of meta should be enabled.
class TransactionMetaBuilder
{
  public:
    // Creates a meta builder for a provided transaction.
    // The builder can can be disabled by setting `metaEnabled` to false,
    // which will disable all the logic both in this builder, as well as in
    // the dependent data structures, thus making them very cheap.
    TransactionMetaBuilder(bool metaEnabled, TransactionFrameBase const& tx,
                           uint32_t protocolVersion, AppConnector const& app);

    TxEventManager& getTxEventManager();

    // Returns an operation builder for the i-th operation in the corresponding
    // transaction.
    OperationMetaBuilder& getOperationMetaBuilderAt(size_t i);

    // Adds changes from the provided ledger transaction to `changesBefore`
    // vector.
    void pushTxChangesBefore(AbstractLedgerTxn& changesBeforeLtx);
    // Adds changes from the provided ledger transaction to `changesAfter`
    // vector.
    void pushTxChangesAfter(AbstractLedgerTxn& changesAfterLtx);
    // Sets fee for non-refundable resources. Currently this is only meaningful
    // for Soroban transactions.
    void setNonRefundableResourceFee(int64_t fee);
    // Sets the metadata for the refundable Soroban fee if
    // `refundableFeeTracker` is provided (non-nullopt).
    void maybeSetRefundableFeeMeta(
        std::optional<RefundableFeeTracker> const& refundableFeeTracker);

    // Returns the diagnostic event buffer for the transaction.
    DiagnosticEventManager& getDiagnosticEventManager();

    // Moves the finalized transaction meta XDR out of the builder thus
    // invalidating it.
    // `success` indicates whether the transaction was successful, which
    // impacts the fields populated in the XDR.
    TransactionMeta finalize(bool success);

  private:
    // Helper for accessing and updating the transaction meta XDR.
    class TransactionMetaWrapper
    {
      public:
        TransactionMetaWrapper(uint32_t protocolVersion, Config const& config);

        LedgerEntryChanges& getChangesBefore();
        LedgerEntryChanges& getChangesAfter();

        SorobanTransactionMetaExt& getSorobanMetaExt();

        void setOperationMetas(xdr::xvector<OperationMeta>&& opMetas);
        void setOperationMetas(xdr::xvector<OperationMetaV2>&& opMetas);
        void setReturnValue(SCVal const& returnValue);
        void setTransactionEvents(xdr::xvector<TransactionEvent>&& events);
        void setDiagnosticEvents(xdr::xvector<DiagnosticEvent>&& events);
        void maybeSetContractEventsAtTxLevel(
            xdr::xvector<ContractEvent>&& events);
        void maybeActivateSorobanMeta(bool success);

        TransactionMeta mTransactionMeta;
    };

    void maybePushChanges(AbstractLedgerTxn& changesLtx,
                          LedgerEntryChanges& destChanges);

    TransactionMetaWrapper mTransactionMeta;
    TxEventManager mTxEventManager;
    DiagnosticEventManager mDiagnosticEventManager;
    bool mIsSoroban = false;
    std::variant<xdr::xvector<OperationMeta>, xdr::xvector<OperationMetaV2>>
        mOperationMetas;

    std::vector<OperationMetaBuilder> mOperationMetaBuilders;
    bool mEnabled = false;
    bool mSorobanMetaExtEnabled = false;
    bool mFinalized = false;
};

} // namespace stellar
