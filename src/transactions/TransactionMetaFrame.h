#pragma once

// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTxn.h"
#include "transactions/EventManager.h"
#include "transactions/MutableTransactionResult.h"
#include "xdr/Stellar-ledger.h"
#include <variant>

namespace stellar
{

class OperationMetaBuilder
{
  public:
    void setLedgerChanges(AbstractLedgerTxn& opLtx);
    void setSorobanReturnValue(SCVal const& val);
    OpEventManager& getEventManager();
    DiagnosticEventBuffer& getDiagnosticEventBuffer();

  private:
    friend class TransactionMetaBuilder;

    OperationMetaBuilder(bool metaEnabled, OperationMeta& meta,
                         OperationFrame const& op, uint32_t protocolVersion,
                         Config const& config,
                         DiagnosticEventBuffer& diagnosticEventBuffer);
    OperationMetaBuilder(bool metaEnabled, OperationMetaV2& meta,
                         OperationFrame const& op, uint32_t protocolVersion,
                         Config const& config,
                         DiagnosticEventBuffer& diagnosticEventBuffer);
    bool maybeFinalizeOpEvents();

    std::optional<SCVal> mSorobanReturnValue{};

    bool mEnabled = false;
    uint32_t mProtocolVersion = 0;
    OperationFrame const& mOp;
    std::variant<std::reference_wrapper<OperationMeta>,
                 std::reference_wrapper<OperationMetaV2>>
        mMeta;
    OpEventManager mEventManager;
    DiagnosticEventBuffer& mDiagnosticEventBuffer;
};

#ifdef BUILD_TESTS
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
    xdr::xvector<stellar::DiagnosticEvent> const& getDiagnosticEvents() const;
    xdr::xvector<stellar::ContractEvent> getSorobanContractEvents() const;
    stellar::LedgerEntryChanges const&
    getLedgerEntryChangesAtOp(size_t opIdx) const;
    TransactionMeta const& getXDR() const;

  private:
    TransactionMeta mTransactionMeta;
};
#endif

class TransactionMetaBuilder
{
  public:
    TransactionMetaBuilder(bool metaEnabled, TransactionFrameBase const& tx,
                           uint32_t protocolVersion, Config const& config);
    OperationMetaBuilder& getOperationMetaBuilderAt(size_t i);

    void pushTxChangesBefore(AbstractLedgerTxn& changesBeforeLtx);
    void pushTxChangesAfter(AbstractLedgerTxn& changesAfterLtx);

    void setNonRefundableResourceFee(int64_t fee);
    void maybeSetRefundableFeeMeta(
        std::optional<RefundableFeeTracker> const& refundableFeeTracker);

    DiagnosticEventBuffer& getDiagnosticEventBuffer();

    TransactionMeta finalize(bool success);

  private:
    class TransactionMetaWrapper
    {
      public:
        TransactionMetaWrapper(uint32_t protocolVersion, Config const& config);

        LedgerEntryChanges& getChangesBefore();
        LedgerEntryChanges& getCangesAfter();

        SorobanTransactionMetaExt& getSorobanMetaExt();

        void setOperationMetas(xdr::xvector<OperationMeta>&& opMetas);
        void setOperationMetas(xdr::xvector<OperationMetaV2>&& opMetas);
        void setReturnValue(SCVal const& returnValue);
        void setDiagnosticEvents(xdr::xvector<DiagnosticEvent>&& events);
        void
        maybeSetContractEventsAtTxLevel(xdr::xvector<ContractEvent>&& events);
        void maybeActivateSorobanMeta(bool success);
        TransactionMeta mTransactionMeta;
    };

    void maybePushChanges(AbstractLedgerTxn& changesLtx,
                          LedgerEntryChanges& destChanges);

    TransactionMetaWrapper mTransactionMeta;
    DiagnosticEventBuffer mDiagnosticEventBuffer;
    bool mIsSoroban = false;
    std::variant<xdr::xvector<OperationMeta>, xdr::xvector<OperationMetaV2>>
        mOperationMetas;

    std::vector<OperationMetaBuilder> mOperationMetaBuilders;
    bool mEnabled = false;
    bool mSorobanMetaExtEnabled = false;
};

} // namespace stellar
