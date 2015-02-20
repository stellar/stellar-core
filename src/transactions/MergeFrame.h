#pragma once

#include "transactions/TransactionFrame.h"

namespace stellar
{
    class MergeFrame : public TransactionFrame
    {
        AccountMerge::AccountMergeResult &innerResult() { return mResult.body.tr().accountMergeResult(); }
    public:
        MergeFrame(const TransactionEnvelope& envelope);

        bool doApply(LedgerDelta& delta, LedgerMaster& ledgerMaster);
        bool doCheckValid(Application& app);
    };

    namespace AccountMerge
    {
        inline AccountMerge::AccountMergeResultCode getInnerCode(TransactionResult const & res)
        {
            return res.body.tr().accountMergeResult().code();
        }
    }

}