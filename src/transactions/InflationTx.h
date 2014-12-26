#include "transactions/Transaction.h"

namespace stellar
{
    class InflationTx : public Transaction
    {

    public:
        void doApply(TxDelta& delta, LedgerMaster& ledgerMaster);
        bool doCheckValid(LedgerMaster& ledgerMaster);
    };
}