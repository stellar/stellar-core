#include "transactions/CancelOfferFrame.h"
#include "ledger/OfferFrame.h"
#include "ledger/LedgerMaster.h"
#include "database/Database.h"

namespace stellar
{
    CancelOfferFrame::CancelOfferFrame(const TransactionEnvelope& envelope) : TransactionFrame(envelope)
    {

    }
// look up this Offer
bool CancelOfferFrame::doApply(LedgerDelta& delta, LedgerMaster& ledgerMaster)
{
    OfferFrame offerFrame;
    if(!ledgerMaster.getDatabase().loadOffer(mSigningAccount->mEntry.account().accountID, 
        mEnvelope.tx.body.offerSeqNum(), offerFrame))
    {
        mResultCode = txOFFER_NOT_FOUND;
        return false;
    }
    mResultCode = txSUCCESS;
    mSigningAccount->mEntry.account().ownerCount--;

    offerFrame.storeDelete(delta, ledgerMaster);

    return true;
}

bool CancelOfferFrame::doCheckValid(Application& app)
{
    return true;
}

}
