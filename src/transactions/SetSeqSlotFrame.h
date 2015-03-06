#pragma once

#include "transactions/TransactionFrame.h"

namespace stellar
{
    class SetSeqSlotOpFrame : public OperationFrame
    {
        
        SetSeqSlot::SetSeqSlotResult &innerResult() { return mResult.tr().setSeqSlotResult(); }
        SetSeqSlotOp const& mSetSlot;
    public:
        SetSeqSlotOpFrame(Operation const& op, OperationResult &res, TransactionFrame &parentTx);

        bool doApply(LedgerDelta& delta, LedgerMaster& ledgerMaster);
        bool doCheckValid(Application& app);
    };

    namespace SetSeqSlot
    {
        inline SetSeqSlot::SetSeqSlotResultCode getInnerCode(OperationResult const & res)
        {
            return res.tr().setSeqSlotResult().code();
        }
    }

}