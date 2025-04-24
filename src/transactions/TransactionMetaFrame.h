#pragma once

// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/OperationMetaArray.h"
#include "xdr/Stellar-ledger.h"

namespace stellar
{
class Config;

// Wrapper around TransactionMeta XDR that provides mutable access to fields
// in the proper version of meta.
class TransactionMetaFrame
{
  public:
    TransactionMetaFrame(uint32_t protocolVersion, Config const& config);

    void pushTxChangesBefore(LedgerEntryChanges&& changes);
    size_t getNumChangesBefore() const;
    LedgerEntryChanges getChangesBefore() const;
    LedgerEntryChanges getChangesAfter() const;
    void clearOperationMetas();
    void pushOperationMetas(OperationMetaArray&& opMetas);
    size_t getNumOperations() const;
    void pushTxChangesAfter(LedgerEntryChanges&& changes);
    void clearTxChangesAfter();

    TransactionMeta const& getXDR() const;

    void maybePushSorobanContractEvents(OperationMetaArray& opMetas);
    void pushTxContractEvents(xdr::xvector<ContractEvent>&& events);
    void maybePushDiagnosticEvents(xdr::xvector<DiagnosticEvent>&& events,
                                   bool isSoroban);
    void setReturnValue(SCVal&& returnValue);
    void setSorobanFeeInfo(int64_t nonRefundableFeeSpent,
                           int64_t totalRefundableFeeSpent,
                           int64_t rentFeeCharged);
#ifdef BUILD_TESTS
    TransactionMetaFrame(TransactionMeta meta);
    SCVal const& getReturnValue() const;
    xdr::xvector<stellar::DiagnosticEvent> const& getDiagnosticEvents() const;
    xdr::xvector<stellar::ContractEvent> const& getTxEvents() const;
    xdr::xvector<stellar::ContractEvent> getSorobanContractEvents() const;
    stellar::LedgerEntryChanges const&
    getLedgerEntryChangesAtOp(size_t opIdx) const;
    xdr::xvector<ContractEvent> const& getOpEventsAtOp(size_t opIdx) const;
#endif

  private:
    TransactionMeta mTransactionMeta;
    int mVersion;
};

}
