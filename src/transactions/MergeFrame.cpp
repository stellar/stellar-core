#include "transactions/MergeFrame.h"

// TODO.2 still 

namespace stellar
{

    MergeFrame::MergeFrame(const TransactionEnvelope& envelope) : TransactionFrame(envelope)
    {

    }

    // make sure the deleted Account hasn't issued credit
    // make sure the we delete all the offers
    // move the STR to the new account
    // move any credit to the new account
    void MergeFrame::doApply(TxDelta& delta, LedgerMaster& ledgerMaster)
    {
        // TODO.2
        
    }

    bool MergeFrame::doCheckValid(Application& app)
    {
        // TODO.2
        return false;
    }
}
