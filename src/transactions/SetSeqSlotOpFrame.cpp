#include "transactions/SetSeqSlotOpFrame.h"
#include "crypto/Base58.h"
#include "database/Database.h"


namespace stellar
{
    SetSeqSlotOpFrame::SetSeqSlotOpFrame(Operation const& op, OperationResult &res,
        TransactionFrame &parentTx) :
        OperationFrame(op, res, parentTx), mSetSlot(mOperation.body.setSeqSlotOp())
    {

    }

    bool SetSeqSlotOpFrame::doCheckValid(Application& app)
    {
        return false;
    }


    // change the seq of an existing slot or create a new slot
    // make sure a new slot has the correct slot index
    bool SetSeqSlotOpFrame::doApply(LedgerDelta& delta, LedgerMaster& ledgerMaster)
    {
        Database &db = ledgerMaster.getDatabase();

        uint32_t slotIndex = mSetSlot.slotIndex;

        uint32_t maxSlot = mSourceAccount->getMaxSeqSlot(db);
        if(slotIndex <= maxSlot)
        {  // changing old slot
            uint32_t curNum = mSourceAccount->getSeq(slotIndex,db);
            if(curNum >= mSetSlot.slotValue)
            {
                innerResult().code(SetSeqSlot::INVALID_SEQ_NUM);
                return false;
            } else
            {
                
            }

        } else if(slotIndex == maxSlot + 1)
        { // creating a new slot
           
        } else
        { 
            innerResult().code(SetSeqSlot::INVALID_SLOT);
            return false;
        }

        innerResult().code(SetSeqSlot::SUCCESS);
        return true;
    }
}
