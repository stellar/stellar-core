#pragma once

#include "transactions/TransactionFrame.h"

namespace stellar
{
    class MergeFrame : public OperationFrame
    {
        AccountMerge::AccountMergeResult &innerResult() { return mResult.tr().accountMergeResult(); }
    public:
        MergeFrame(Operation const& op, OperationResult &res, TransactionFrame &parentTx);

        bool doApply(LedgerDelta& delta, LedgerMaster& ledgerMaster);
        bool doCheckValid(Application& app);
    };

    namespace AccountMerge
    {
        inline AccountMerge::AccountMergeResultCode getInnerCode(OperationResult const & res)
        {
            return res.tr().accountMergeResult().code();
        }
    }

}