#include "transactions/CancelOfferFrame.h"
#include "ledger/OfferFrame.h"
#include "ledger/LedgerMaster.h"
#include "database/Database.h"

namespace stellar
{
// look up this Offer
void CancelOfferFrame::doApply(TxDelta& delta, LedgerMaster& ledgerMaster)
{
    OfferFrame offerFrame;
    if(!ledgerMaster.getDatabase().loadOffer(mSigningAccount.mEntry.account().accountID, 
        mEnvelope.tx.body.offerSeqNum(), offerFrame))
    {
        mResultCode = txOFFER_NOT_FOUND;
        return;
    }
    mResultCode = txSUCCESS;
    mSigningAccount.mEntry.account().ownerCount--;
    
    delta.setStart(offerFrame);  // setting the start but no final deletes the entry
    delta.setFinal(mSigningAccount);
}
}