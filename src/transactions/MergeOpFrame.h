#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/OperationFrame.h"
#include <memory>

namespace stellar
{
class LedgerState;

class MergeOpFrame : public OperationFrame
{
    AccountMergeResult&
    innerResult()
    {
        return mResult.tr().accountMergeResult();
    }

    ThresholdLevel getThresholdLevel() const override;

  public:
    MergeOpFrame(Operation const& op, OperationResult& res,
                 TransactionFrame& parentTx);

    bool doApply(Application& app, LedgerState& ls) override;
    bool doCheckValid(Application& app, uint32_t ledgerVersion) override;

    static AccountMergeResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().accountMergeResult().code();
    }
};
}
