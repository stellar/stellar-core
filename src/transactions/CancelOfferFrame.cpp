#include "transactions/CancelOfferFrame.h"
#include "ledger/OfferFrame.h"
#include "ledger/LedgerMaster.h"
#include "database/Database.h"

namespace stellar
{

CancelOfferFrame::CancelOfferFrame(Operation const& op, OperationResult &res,
    TransactionFrame &parentTx) :
    OperationFrame(op, res, parentTx)
{
}

bool CancelOfferFrame::doApply(LedgerDelta& delta, LedgerMaster& ledgerMaster)
{
    OfferFrame offerFrame;
    Database &db = ledgerMaster.getDatabase();
    if(!OfferFrame::loadOffer(getSourceID(),
        mOperation.body.offerID(), offerFrame, db))
    {
        innerResult().code(CancelOffer::NOT_FOUND);
        return false;
    }

    innerResult().code(CancelOffer::SUCCESS);
    
    mSourceAccount->getAccount().numSubEntries--;
    offerFrame.storeDelete(delta, db);
    mSourceAccount->storeChange(delta, db);

    return true;
}

bool CancelOfferFrame::doCheckValid(Application& app)
{
    return true;
}

}
