#pragma once

#include "transactions/TransactionFrame.h"

namespace stellar
{
    class SetSeqSlotFrame : public TransactionFrame
    {
        
        SetSeqSlot::SetSeqSlotResult &innerResult() { return mResult.body.tr().setSeqSlotResult(); }
    public:
        SetSeqSlotFrame(const TransactionEnvelope& envelope);

        bool doApply(LedgerDelta& delta, LedgerMaster& ledgerMaster);
        bool doCheckValid(Application& app);
    };

    namespace SetSeqSlot
    {
        inline SetSeqSlot::SetSeqSlotResultCode getInnerCode(TransactionResult const & res)
        {
            return res.body.tr().setSeqSlotResult().code();
        }
    }

}