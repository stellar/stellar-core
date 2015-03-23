// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "transactions/CancelOfferOpFrame.h"
#include "ledger/OfferFrame.h"
#include "ledger/LedgerManagerImpl.h"
#include "database/Database.h"

namespace stellar
{

CancelOfferOpFrame::CancelOfferOpFrame(Operation const& op,
                                       OperationResult& res,
                                       TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
{
}

bool
CancelOfferOpFrame::doApply(LedgerDelta& delta, LedgerManagerImpl& ledgerMaster)
{
    OfferFrame offerFrame;
    Database& db = ledgerMaster.getDatabase();
    if (!OfferFrame::loadOffer(getSourceID(), mOperation.body.offerID(),
                               offerFrame, db))
    {
        innerResult().code(CancelOffer::CANCEL_OFFER_NOT_FOUND);
        return false;
    }

    innerResult().code(CancelOffer::CANCEL_OFFER_SUCCESS);

    mSourceAccount->getAccount().numSubEntries--;
    offerFrame.storeDelete(delta, db);
    mSourceAccount->storeChange(delta, db);

    return true;
}

bool
CancelOfferOpFrame::doCheckValid(Application& app)
{
    return true;
}
}
