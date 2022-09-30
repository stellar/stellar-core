#pragma once

// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "xdr/Stellar-ledger.h"

namespace stellar
{

// Wrapper around TransactionMeta XDR that provides mutable access to fields
// in the proper version of meta.
class TransactionMetaFrame
{
  public:
    TransactionMetaFrame(uint32_t protocolVersion);

    void pushTxChangesBefore(LedgerEntryChanges&& changes);
    size_t getNumChangesBefore() const;
    LedgerEntryChanges getChangesBefore() const;
    void clearOperationMetas();
    void pushOperationMetas(xdr::xvector<OperationMeta>&& opMetas);
    size_t getNumOperations() const;
    void pushTxChangesAfter(LedgerEntryChanges&& changes);
    void clearTxChangesAfter();

    void setTxResult(TransactionResult const& res);
    void finalizeHashes();
    TransactionMeta const& getXDR() const;

#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    void pushContractEvents(xdr::xvector<ContractEvent>&& events);
    static Hash getHashOfMetaHashes(TransactionMeta const&);
#endif

  private:
    TransactionMeta mTransactionMeta;
    int mVersion;
    bool mHashesFinalized{false};
};

}
