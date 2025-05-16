#pragma once

// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/TxSetFrame.h"
#include "xdr/Stellar-ledger.h"

namespace stellar
{

// Wrapper around LedgerCloseMeta XDR that provides mutable access to fields
// in the proper version of meta.
class LedgerCloseMetaFrame
{
  public:
    LedgerCloseMetaFrame(uint32_t protocolVersion);

    LedgerHeaderHistoryEntry& ledgerHeader();
    void reserveTxProcessing(size_t n);
    void pushTxProcessingEntry();
    void
    setLastTxProcessingFeeProcessingChanges(LedgerEntryChanges const& changes);
    void setTxProcessingMetaAndResultPair(TransactionMeta&& tm,
                                          TransactionResultPair&& rp,
                                          int index);
    void setPostTxApplyFeeProcessing(LedgerEntryChanges&& changes, int index);

    xdr::xvector<UpgradeEntryMeta>& upgradesProcessing();

    void populateTxSet(TxSetXDRFrame const& txSet);

    // Used for populating meta from background eviction scan
    void populateEvictedEntries(EvictedStateVectors const& evictedState);

    void setNetworkConfiguration(SorobanNetworkConfig const& networkConfig,
                                 bool emitExtV1);

    LedgerCloseMeta const& getXDR() const;

#ifdef BUILD_TESTS
    LedgerCloseMetaFrame(LedgerCloseMeta const& lm);
    LedgerHeader const& getLedgerHeader() const;
    xdr::xvector<LedgerKey> const& getEvictedKeys() const;
    size_t getTransactionResultMetaCount() const;
    TransactionMeta const& getTransactionMeta(size_t index) const;
    LedgerEntryChanges const& getPostTxApplyFeeProcessing(size_t index) const;

#endif

  private:
    LedgerCloseMeta mLedgerCloseMeta;
    int mVersion;
};

}
