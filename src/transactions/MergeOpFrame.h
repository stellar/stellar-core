#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "transactions/OperationFrame.h"

namespace stellar
{
class MergeOpFrame : public OperationFrame
{
    AccountMergeResult&
    innerResult()
    {
        return mResult.tr().accountMergeResult();
    }

  public:
    MergeOpFrame(Operation const& op, OperationResult& res,
                 TransactionFrame& parentTx);

    int32_t getNeededThreshold() const;
    bool doApply(LedgerDelta& delta, LedgerManager& ledgerManager);
    bool doCheckValid(Application& app);

    static AccountMergeResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().accountMergeResult().code();
    }
};
}
