#include "transactions/TransactionFrame.h"

namespace stellar
{
    class AllowTrustTxFrame : public TransactionFrame
    {
    public:
        void doApply(TxDelta& delta, LedgerMaster& ledgerMaster);
        bool doCheckValid(LedgerMaster& ledgerMaster);
    };
}
