#include "transactions/SetSeqSlotFrame.h"
#include "crypto/Base58.h"
#include "database/Database.h"


namespace stellar
{
    SetSeqSlotFrame::SetSeqSlotFrame(const TransactionEnvelope& envelope) :
        TransactionFrame(envelope)
    {

    }

    bool SetSeqSlotFrame::doCheckValid(Application& app)
    {
        return false;
    }


    // change the seq of an existing slot or create a new slot
    // make sure a new slot has the correct slot index
    bool SetSeqSlotFrame::doApply(LedgerDelta& delta, LedgerMaster& ledgerMaster)
    {
        Database &db = ledgerMaster.getDatabase();

        uint32_t slotIndex = mEnvelope.tx.body.setSeqSlotTx().slotIndex;

        uint32_t maxSlot = mSigningAccount->getMaxSeqSlot(db);
        if(slotIndex <= maxSlot)
        {  // changing old slot
            uint32_t curNum = mSigningAccount->getSeq(slotIndex,db);
            if(curNum >= mEnvelope.tx.body.setSeqSlotTx().slotValue)
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
