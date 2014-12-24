#include "transactions/Transaction.h"

namespace stellar
{
    class PaymentTx : public Transaction
    {
        void sendCredit(AccountEntry& receiver, TxDelta& delta, LedgerMaster& ledgerMaster);
    public:
        void doApply(TxDelta& delta, LedgerMaster& ledgerMaster);
        bool doCheckValid(LedgerMaster& ledgerMaster);
    };
}
