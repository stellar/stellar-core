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

static constexpr ContractDataDurability CONTRACT_INSTANCE_CONTRACT_DURABILITY =
    ContractDataDurability::PERSISTENT;

class InvokeHostFunctionOpFrame : public OperationFrame
{
    InvokeHostFunctionResult&
    innerResult()
    {
        return mResult.tr().invokeHostFunctionResult();
    }

    void maybePopulateDiagnosticEvents(Config const& cfg,
                                       InvokeHostFunctionOutput const& output,
                                       HostFunctionMetrics const& metrics);

    bool validateContractLedgerEntry(LedgerEntry const& le, size_t entrySize,
                                     SorobanNetworkConfig const& config);

    InvokeHostFunctionOp const& mInvokeHostFunction;

  public:
    InvokeHostFunctionOpFrame(Operation const& op, OperationResult& res,
                              TransactionFrame& parentTx);

    bool isOpSupported(LedgerHeader const& header) const override;

    bool doApply(AbstractLedgerTxn& ltx) override;
    bool doApply(Application& app, AbstractLedgerTxn& ltx,
                 Hash const& sorobanBasePrngSeed) override;

    bool doCheckValid(SorobanNetworkConfig const& config,
                      uint32_t ledgerVersion) override;
    bool doCheckValid(uint32_t ledgerVersion) override;

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
#endif // ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
