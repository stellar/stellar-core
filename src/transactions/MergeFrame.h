#include "transactions/TransactionFrame.h"

namespace stellar
{
    class MergeFrame : public TransactionFrame
    {

    public:
        void doApply(TxDelta& delta, LedgerMaster& ledgerMaster);
        bool doCheckValid(LedgerMaster& ledgerMaster);
    };
}