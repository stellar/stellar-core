#pragma once

#include "transactions/OperationFrame.h"

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

namespace stellar
{
class ChangeTrustOpFrame : public OperationFrame
{
    ChangeTrustResult&
    innerResult()
    {
        return mResult.tr().changeTrustResult();
    }
    ChangeTrustOp const& mChangeTrust;

  public:
    ChangeTrustOpFrame(Operation const& op, OperationResult& res,
                       TransactionFrame& parentTx);

    bool doApply(AbstractLedgerTxn& ltx) override;
    bool doCheckValid(uint32_t ledgerVersion) override;

    static ChangeTrustResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().changeTrustResult().code();
    }
};
}
