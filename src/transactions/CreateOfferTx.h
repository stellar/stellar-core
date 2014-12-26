#include "transactions/Transaction.h"

namespace stellar
{
    class CreateOfferTx : public Transaction
    {

    public:
        void doApply(TxDelta& delta, LedgerMaster& ledgerMaster);
        bool doCheckValid(LedgerMaster& ledgerMaster);
    };
}