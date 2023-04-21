#pragma once

// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "xdr/Stellar-transaction.h"
#include <medida/metrics_registry.h>
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
#include "rust/RustBridge.h"
#include "transactions/OperationFrame.h"

namespace stellar
{
class AbstractLedgerTxn;

class InvokeHostFunctionOpFrame : public OperationFrame
{
    InvokeHostFunctionResult&
    innerResult()
    {
        return mResult.tr().invokeHostFunctionResult();
    }

    void maybePopulateDiagnosticEvents(Config const& cfg,
                                       InvokeHostFunctionOutput const& output);

    InvokeHostFunctionOp const& mInvokeHostFunction;

    uint64_t mCpuLimit;

    uint64_t mMemLimit;

    std::shared_ptr<ContractCostParams const> mCpuParams;

    std::shared_ptr<ContractCostParams const> mMemParams;

  public:
    InvokeHostFunctionOpFrame(Operation const& op, OperationResult& res,
                              TransactionFrame& parentTx);

    ThresholdLevel getThresholdLevel() const override;

    bool isOpSupported(LedgerHeader const& header) const override;

    bool doApply(AbstractLedgerTxn& ltx) override;
    bool doApply(AbstractLedgerTxn& ltx, Config const& cfg,
                 medida::MetricsRegistry& metrics) override;
    bool doCheckValid(uint32_t ledgerVersion) override;

    void
    insertLedgerKeysToPrefetch(UnorderedSet<LedgerKey>& keys) const override;

    static InvokeHostFunctionResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().invokeHostFunctionResult().code();
    }

    virtual bool isSmartOperation() const override;

    virtual void setSmartOperationLimitsAndCostParams(
        uint64_t cpuLimit, uint64_t memLimit,
        std::shared_ptr<ContractCostParams const> const& cpuParams,
        std::shared_ptr<ContractCostParams const> const& memParams) override;

    virtual void getRemainingSmartOperationLimits(uint64_t& cpu,
                                                  uint64_t& mem) const override;
};
}
#endif // ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
