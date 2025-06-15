#pragma once

// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

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

struct HostFunctionMetrics;
class ApplyHelperBase;
class PreV23ApplyHelper;
class ParallelApplyHelper;

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
                                       DiagnosticEventManager& buffer) const;

    InvokeHostFunctionOp const& mInvokeHostFunction;

  public:
    InvokeHostFunctionOpFrame(Operation const& op,
                              TransactionFrame const& parentTx);

    bool isOpSupported(LedgerHeader const& header) const override;

    bool doApply(AppConnector& app, AbstractLedgerTxn& ltx,
                 Hash const& sorobanBasePrngSeed, OperationResult& res,
                 std::optional<RefundableFeeTracker>& refundableFeeTracker,
                 OperationMetaBuilder& opMeta) const override;

    bool doCheckValidForSoroban(
        SorobanNetworkConfig const& networkConfig, Config const& appConfig,
        uint32_t ledgerVersion, OperationResult& res,
        DiagnosticEventManager& diagnosticEvents) const override;
    bool doCheckValid(uint32_t ledgerVersion,
                      OperationResult& res) const override;

    ParallelTxReturnVal doParallelApply(
        AppConnector& app,
        ThreadEntryMap const& entryMap, // Must not be shared between threads!
        Config const& appConfig, SorobanNetworkConfig const& sorobanConfig,
        Hash const& txPrngSeed, ParallelLedgerInfo const& ledgerInfo,
        SorobanMetrics& sorobanMetrics, OperationResult& res,
        std::optional<RefundableFeeTracker>& refundableFeeTracker,
        OperationMetaBuilder& opMeta) const override;

    void
    insertLedgerKeysToPrefetch(UnorderedSet<LedgerKey>& keys) const override;

    static InvokeHostFunctionResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().invokeHostFunctionResult().code();
    }

    virtual bool isSoroban() const override;

    friend class ApplyHelperBase;
    friend class PreV23ApplyHelper;
    friend class ParallelApplyHelper;
};
}
