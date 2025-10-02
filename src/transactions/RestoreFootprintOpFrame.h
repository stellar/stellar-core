#pragma once

// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/OperationFrame.h"
#include "xdr/Stellar-transaction.h"

namespace stellar
{
class AbstractLedgerTxn;
class MutableTransactionResultBase;

class RestoreFootprintOpFrame : public OperationFrame
{
    RestoreFootprintOp const& mRestoreFootprintOp;

  public:
    RestoreFootprintOpFrame(Operation const& op,
                            TransactionFrame const& parentTx);

    bool isOpSupported(LedgerHeader const& header) const override;

    virtual bool
    doApplyForSoroban(AppConnector& app, AbstractLedgerTxn& ltx,
                      SorobanNetworkConfig const& sorobanConfig,
                      Hash const& sorobanBasePrngSeed, OperationResult& res,
                      std::optional<RefundableFeeTracker>& refundableFeeTracker,
                      OperationMetaBuilder& opMeta) const override;
    bool doApply(AppConnector& app, AbstractLedgerTxn& ltx,
                 OperationResult& res,
                 OperationMetaBuilder& opMeta) const override;

    bool doCheckValidForSoroban(
        SorobanNetworkConfig const& networkConfig, Config const& appConfig,
        uint32_t ledgerVersion, OperationResult& res,
        DiagnosticEventManager& diagnosticEvents) const override;
    bool doCheckValid(uint32_t ledgerVersion,
                      OperationResult& res) const override;

    ParallelTxReturnVal
    doParallelApply(AppConnector& app,
                    ThreadParallelApplyLedgerState const& threadState,
                    Config const& appConfig, Hash const& txPrngSeed,
                    ParallelLedgerInfo const& ledgerInfo,
                    SorobanMetrics& sorobanMetrics, OperationResult& res,
                    std::optional<RefundableFeeTracker>& refundableFeeTracker,
                    OperationMetaBuilder& opMeta) const override;

    void
    insertLedgerKeysToPrefetch(UnorderedSet<LedgerKey>& keys) const override;

    static RestoreFootprintResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().restoreFootprintResult().code();
    }

    virtual bool isSoroban() const override;

    ThresholdLevel getThresholdLevel() const override;
    friend class RestoreFootprintApplyHelper;
    friend class RestoreFootprintPreV23ApplyHelper;
    friend class RestoreFootprintParallelApplyHelper;
};
}
