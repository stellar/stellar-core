#pragma once

// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/OperationFrame.h"

namespace stellar
{

class AbstractLedgerTxn;

class ClawbackOpFrame : public OperationFrame
{
    ClawbackResult&
    innerResult()
    {
        return mResult.tr().clawbackResult();
    }

    ClawbackOp const& mClawback;

  public:
    ClawbackOpFrame(Operation const& op, OperationResult& res,
                    TransactionFrame& parentTx);

    bool isVersionSupported(uint32_t protocolVersion) const override;

    bool doApply(AbstractLedgerTxn& ltx) override;
    bool doCheckValid(uint32_t ledgerVersion) override;
    void
    insertLedgerKeysToPrefetch(UnorderedSet<LedgerKey>& keys) const override;

    static ClawbackResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().clawbackResult().code();
    }
};
}
