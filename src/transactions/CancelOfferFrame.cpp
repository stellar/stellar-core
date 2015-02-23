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
    Database &db = ledgerMaster.getDatabase();
    if(!OfferFrame::loadOffer(mSigningAccount->getAccount().accountID, 
        mEnvelope.tx.body.offerSeqNum(), offerFrame, db))
    {
        innerResult().code(CancelOffer::NOT_FOUND);
        return false;
    }

    innerResult().code(CancelOffer::SUCCESS);
    
    mSigningAccount->getAccount().ownerCount--;
    offerFrame.storeDelete(delta, db);
    mSigningAccount->storeChange(delta, db);

    return true;
}

bool CancelOfferFrame::doCheckValid(Application& app)
{
    return true;
}

}
