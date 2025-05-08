#pragma once

// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketUtils.h"
#include "ledger/SorobanMetrics.h"
#include "rust/RustBridge.h"
#include "transactions/OperationFrame.h"
#include "xdr/Stellar-transaction.h"
#include <medida/metrics_registry.h>

namespace stellar
{
class AbstractLedgerTxn;
class MutableTransactionResultBase;

static constexpr ContractDataDurability CONTRACT_INSTANCE_ENTRY_DURABILITY =
    ContractDataDurability::PERSISTENT;

// Metrics for host function execution
struct HostFunctionMetrics
{
    SorobanMetrics& mMetrics;

    uint32_t mReadEntry{0};
    uint32_t mWriteEntry{0};

    uint32_t mLedgerReadByte{0};
    uint32_t mLedgerWriteByte{0};

    uint32_t mReadKeyByte{0};
    uint32_t mWriteKeyByte{0};

    uint32_t mReadDataByte{0};
    uint32_t mWriteDataByte{0};

    uint32_t mReadCodeByte{0};
    uint32_t mWriteCodeByte{0};

    uint32_t mEmitEvent{0};
    uint32_t mEmitEventByte{0};

    // host runtime metrics
    uint64_t mCpuInsn{0};
    uint64_t mMemByte{0};
    uint64_t mInvokeTimeNsecs{0};
    uint64_t mCpuInsnExclVm{0};
    uint64_t mInvokeTimeNsecsExclVm{0};
    uint64_t mDeclaredCpuInsn{0};

    // max single entity size metrics
    uint32_t mMaxReadWriteKeyByte{0};
    uint32_t mMaxReadWriteDataByte{0};
    uint32_t mMaxReadWriteCodeByte{0};
    uint32_t mMaxEmitEventByte{0};

    bool mSuccess{false};

    HostFunctionMetrics(SorobanMetrics& metrics);
    ~HostFunctionMetrics();

    void noteReadEntry(bool isCodeEntry, uint32_t keySize, uint32_t entrySize);
    void noteWriteEntry(bool isCodeEntry, uint32_t keySize, uint32_t entrySize);
    medida::TimerContext getExecTimer();
};

class InvokeHostFunctionOpFrame : public OperationFrame
{
    InvokeHostFunctionResult&
    innerResult(OperationResult& res) const
    {
        return res.tr().invokeHostFunctionResult();
    }

    void maybePopulateDiagnosticEvents(Config const& cfg,
                                       InvokeHostFunctionOutput const& output,
                                       HostFunctionMetrics const& metrics,
                                       DiagnosticEventBuffer& buffer) const;

    InvokeHostFunctionOp const& mInvokeHostFunction;

    // Inner helper class for handling reads in doApply
    class ApplyHelper
    {
      private:
        AppConnector& mApp;
        AbstractLedgerTxn& mLtx;
        OperationResult& mRes;
        std::shared_ptr<SorobanTxData> mSorobanData;
        OpEventManager& mOpEventManager;
        InvokeHostFunctionOpFrame const& mOpFrame;
        Hash const& mSorobanBasePrngSeed;

        // Config and resources - derived from app and parentTx
        SorobanResources const& mResources;
        SorobanNetworkConfig const& mSorobanConfig;
        Config const& mAppConfig;

        // Derived data
        rust::Vec<CxxBuf> mLedgerEntryCxxBufs;
        rust::Vec<CxxBuf> mTtlEntryCxxBufs;
        HostFunctionMetrics mMetrics;
        SearchableHotArchiveSnapshotConstPtr mHotArchive;
        DiagnosticEventBuffer& mDiagnosticEvents;

        // Contains restoration info for all autorestored entries, will be
        // charged following invocation.
        rust::Vec<CxxLedgerEntryRentChange> mAutorestoreRustEntryRentChanges;

        // Helper called on all archived keys in the footprint. Returns false if
        // the operation should fail and populates result code and diagnostic
        // events. Returns true if no failure occurred.
        bool handleArchivedEntry(LedgerKey const& lk, LedgerEntry const& le,
                                 bool isReadOnly,
                                 uint32_t restoredLiveUntilLedger,
                                 bool isHotArchiveEntry);

        // Checks and meters the given keys. Returns false if the operation
        // should fail and populates result code and diagnostic events. Returns
        // true if no failure occurred.
        bool addReads(xdr::xvector<LedgerKey> const& keys, bool isReadOnly);

      public:
        ApplyHelper(AppConnector& app, AbstractLedgerTxn& ltx,
                    Hash const& sorobanBasePrngSeed, OperationResult& res,
                    std::shared_ptr<SorobanTxData> sorobanData,
                    OpEventManager& opEventManager,
                    InvokeHostFunctionOpFrame const& opFrame);

        bool apply();
    };

  public:
    InvokeHostFunctionOpFrame(Operation const& op,
                              TransactionFrame const& parentTx);

    bool isOpSupported(LedgerHeader const& header) const override;

    bool doApply(AppConnector& app, AbstractLedgerTxn& ltx,
                 Hash const& sorobanBasePrngSeed, OperationResult& res,
                 std::shared_ptr<SorobanTxData> sorobanData,
                 OpEventManager& opEventManager) const override;

    bool doCheckValidForSoroban(
        SorobanNetworkConfig const& networkConfig, Config const& appConfig,
        uint32_t ledgerVersion, OperationResult& res,
        DiagnosticEventBuffer* diagnosticEvents) const override;
    bool doCheckValid(uint32_t ledgerVersion,
                      OperationResult& res) const override;

    void
    insertLedgerKeysToPrefetch(UnorderedSet<LedgerKey>& keys) const override;

    static InvokeHostFunctionResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().invokeHostFunctionResult().code();
    }

    virtual bool isSoroban() const override;
};
}
