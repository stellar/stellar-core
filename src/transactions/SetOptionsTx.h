#include "transactions/Transaction.h"

namespace stellar
{
    class SetOptionsTx : public Transaction
    {

    public:
        void doApply(TxDelta& delta, LedgerMaster& ledgerMaster);
        bool doCheckValid(LedgerMaster& ledgerMaster);
    };
}