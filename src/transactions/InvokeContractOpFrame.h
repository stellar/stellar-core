#pragma once

// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/OperationFrame.h"

namespace stellar
{
class AbstractLedgerTxn;
class InvokeContractOpFrame : public OperationFrame
{
    InvokeContractResult&
    innerResult()
    {
        return mResult.tr().invokeContractResult();
    }

    InvokeContractOp const& mInvokeContract;

  public:
    InvokeContractOpFrame(Operation const& op, OperationResult& res,
                          TransactionFrame& parentTx);

    bool isOpSupported(LedgerHeader const& header) const override;

    bool doApply(AbstractLedgerTxn& ltx) override;
    bool doCheckValid(uint32_t ledgerVersion) override;
    void
    insertLedgerKeysToPrefetch(UnorderedSet<LedgerKey>& keys) const override;

    TransactionFrame&
    getParentTx()
    {
        return mParentTx;
    }

    static InvokeContractResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().invokeContractResult().code();
    }
};
}
