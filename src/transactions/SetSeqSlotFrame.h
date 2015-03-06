#pragma once

#include "transactions/TransactionFrame.h"

namespace stellar
{
    class SetSeqSlotFrame : public OperationFrame
    {
        
        SetSeqSlot::SetSeqSlotResult &innerResult() { return mResult.tr().setSeqSlotResult(); }
        SetSeqSlotTx const& mSetSlot;
    public:
        SetSeqSlotFrame(Operation const& op, OperationResult &res, TransactionFrame &parentTx);

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