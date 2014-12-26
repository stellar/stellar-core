#include "transactions/CancelOfferTx.h"
#include "ledger/OfferEntry.h"
#include "ledger/LedgerMaster.h"
#include "database/Database.h"

namespace stellar
{
// look up this Offer
void CancelOfferTx::doApply(TxDelta& delta, LedgerMaster& ledgerMaster)
{
    stellarxdr::LedgerEntry offerEntry;
    if(!ledgerMaster.getDatabase().loadOffer(mSigningAccount->mEntry.account().accountID, 
        mEnvelope.tx.body.offerSeqNum(),offerEntry))
    {
        mResultCode = txOFFER_NOT_FOUND;
        return;
    }
    mResultCode = txSUCCESS;
    OfferEntry offer(offerEntry);
    delta.setStart(offer);
}
}