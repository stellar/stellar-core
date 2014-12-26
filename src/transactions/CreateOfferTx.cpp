#include "transactions/CreateOfferTx.h"
#include "ledger/OfferEntry.h"

namespace stellar
{
    // see if this is modifying an old offer
    // see if this offer crosses any existing offers
    void CreateOfferTx::doApply(TxDelta& delta, LedgerMaster& ledgerMaster)
    {

        OfferEntry offer(mEnvelope.tx);
    }
}